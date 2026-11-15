// Layer 3 (Gears of War 3): GitHub #245 - DualSense adaptive triggers per weapon (--pad_adaptive_triggers, off by
// default; the launcher's Controls tab "DualSense adaptive triggers"). Idea and the first working version: HubertFer's
// separate plugin (#245); this is a rewrite on COGBox's own pieces. Our code (MIT).
//
// The runtime (SDK patch 98, InputSystem::SetTriggerEffect) sends a trigger effect to the PS5 pads of one player and
// remembers it: the same effect again writes nothing, a reconnected pad gets it back on the next call, COGBox's own
// menus and the system dialogs turn the triggers off while open, and the pads are reset when the game's window closes.
// Other controllers, the keyboard and mouse are never written to.
//
// This file decides the effect. Every 125 ms (from the engine tick, game thread), by reflection - no guest address but
// GEngine's: GEngine.GamePlayers[i] (LocalPlayer) -> ControllerId (the pad's XInput user) and Actor (its
// PlayerController) -> Pawn -> Weapon -> the weapon's class, looked up by name up its superclass chain (most derived
// first) in kWeapons below. Off when: no pawn (menus, loading, spectating), the pawn is dead, the game is paused
// (WorldInfo.Pauser), a cutscene runs (PlayerController.bCinematicMode), the front end is open. The Lancer's chainsaw
// (GearPawn.bDoingMeleeAttack while holding a Lancer) gets its own rumble on both triggers.
// A player's effect is sent when it changes, plus once a second (the runtime drops a repeat without a write - that is
// how a pad that reconnected or joined that player gets it).
//
// DS5 trigger effect bytes (11 per trigger; byte 0 = mode). Positions and strengths 0-255 (0 = trigger at rest).
//   0x05 off                                         {0x05}
//   0x01 continuous resistance from a position       {0x01, start, force}                  [V] SDL 3.4 testcontroller.c
//   0x02 section resistance (a "click" to pass)      {0x02, start, end, force}             [E] Nielk1's DS5 notes
//   0x06 vibration beyond a position                 {0x06, frequency Hz, amplitude, start} [V] SDL test {0x06,15,63,128}
// (The newer 10-zone modes 0x21 feedback / 0x25 weapon / 0x26 vibration are not used: SDL's test doesn't cover them.)
#include <rex/input/input_system.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>

#include <ue3/objects.h>

#include "gow3_addresses.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <initializer_list>
#include <iterator>
#include <utility>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

namespace {
namespace obj = ue3::obj;
using Clock = std::chrono::steady_clock;
using Effect = std::array<uint8_t, 11>;

constexpr Effect Off() { return {0x05}; }
constexpr Effect Res(uint8_t start, uint8_t force) { return {0x01, start, force}; }
constexpr Effect Click(uint8_t start, uint8_t end, uint8_t force) { return {0x02, start, end, force}; }
constexpr Effect Vib(uint8_t hz, uint8_t amplitude, uint8_t start) { return {0x06, hz, amplitude, start}; }

struct Feel {
  const char* cls;   // a weapon class or one of its base classes (Gears 3's GearGame script)
  const char* what;  // for the log
  Effect right, left;
};
// Right trigger = fire, left = aim. First tuning (needs a DualSense in hand - test plan in the notes): automatics buzz,
// single shots click, heavy and charged weapons push back; aiming is a light pull, heavier with heavy weapons.
// Checked in order of the weapon's class chain, most derived class first, so a more specific class wins.
const Feel kWeapons[] = {
    {"GearWeap_AssaultRifle", "Lancer", Vib(12, 80, 40), Res(32, 60)},
    {"GearWeap_RetroLancer", "Retro Lancer", Vib(9, 150, 40), Res(32, 80)},
    {"GearWeap_LocustAssaultRifle", "Hammerburst", Click(64, 128, 160), Res(32, 60)},
    {"GearWeap_Shotgun_SawedOff", "Sawed-off Shotgun", Click(96, 176, 255), Res(32, 96)},
    {"GearWeap_Shotgun", "Gnasher", Click(80, 160, 224), Res(32, 60)},
    {"GearWeap_SniperRifle", "Longshot", Click(112, 160, 192), Res(16, 128)},
    {"GearWeap_BoomshotBase", "Boomshot", Click(80, 176, 255), Res(32, 96)},
    {"GearWeap_LocustBurstPistolBase", "Gorgon", Vib(15, 96, 64), Res(32, 48)},
    {"GearWeap_LocustPistol", "Boltok", Click(96, 160, 224), Res(32, 60)},
    {"GearWeap_PistolBase", "pistol", Click(48, 96, 128), Res(32, 48)},
    {"GearWeap_HeavyMiniGunBase", "Mulcher", Vib(20, 176, 32), Res(0, 160)},
    {"GearWeap_HeavyMortarBase", "Mortar", Res(32, 192), Res(0, 160)},
    {"GearWeap_HeavySniperBase", "OneShot", Res(48, 255), Res(0, 144)},
    {"GearWeap_DiggerLauncher", "Digger", Click(64, 144, 176), Res(32, 80)},
    {"GearWeap_BowBase", "Torque Bow", Res(16, 160), Res(32, 80)},
    {"GearWeap_FlameThrowerBase", "Scorcher", Vib(40, 64, 32), Res(32, 60)},
    {"GearWeap_GrenadeBase", "grenade", Res(64, 80), Res(32, 48)},
    {"GearWeap_HODBase", "Hammer of Dawn", Res(32, 96), Res(32, 60)},
    {"GearWeap_CleaverBase", "Cleaver", Off(), Off()},
    {"GearWeap_HeavyBase", "heavy weapon", Res(32, 144), Res(0, 144)},
    {"GearWeap_Melee", "melee", Off(), Off()},
    {"GearWeapon", "weapon", Res(48, 64), Res(32, 48)},
};
constexpr int kNone = -1;      // nothing sent to this player yet
constexpr int kOff = -2;       // no weapon / not playing
constexpr int kChainsaw = -3;  // the Lancer's chainsaw
const Feel kChainsawFeel = {"", "chainsaw", Vib(30, 255, 0), Vib(30, 255, 0)};
const Feel kOffFeel = {"", "off", Off(), Off()};

struct Props {
  // classes (found a slice of the object table at a time - FindClassRange - so a class not loaded yet never costs a
  // full scan on the game thread)
  uint32_t engine = 0, player = 0, localPlayer = 0, controller = 0, playerController = 0, pawn = 0, actor = 0,
           worldInfo = 0, gearPawn = 0, lancer = 0;
  int gamePlayers = -1, actorOfPlayer = -1, controllerId = -1, pawnOfController = -1, weapon = -1, health = -1,
      worldInfoOfActor = -1, pauser = -1, cinematic = -1, meleeing = -1;
  uint32_t cinematicMask = 0, meleeingMask = 0;
  uint32_t cursor = 0, passes = 0;
  bool done = false;

  // true once the properties the effect needs are known. 16384 table slots per call (a pass over the ~168,000 objects
  // in ~1.3 s at 8 calls a second); the two GearGame classes (the chainsaw check only) are not waited for past two passes.
  bool Find() {
    if (done) return true;
    uint32_t total = 0;
    auto step = [&](std::initializer_list<std::pair<uint32_t*, const char*>> classes) {
      bool all = true;
      for (const auto& [cls, name] : classes) {
        if (!*cls) {
          uint32_t t = 0;
          *cls = obj::FindClassRange(name, cursor, 16384, &t);
          total = std::max(total, t);
        }
        all = all && *cls;
      }
      return all;
    };
    const bool required = step({{&engine, "Engine"}, {&player, "Player"}, {&localPlayer, "LocalPlayer"},
                                {&controller, "Controller"}, {&playerController, "PlayerController"}, {&pawn, "Pawn"},
                                {&actor, "Actor"}, {&worldInfo, "WorldInfo"}});
    const bool optional = step({{&gearPawn, "GearPawn"}, {&lancer, "GearWeap_AssaultRifle"}});
    if (cursor + 16384 >= total) cursor = 0, passes++;
    else cursor += 16384;
    if (!required || (!optional && passes < 2)) return false;
    gamePlayers = obj::PropertyOffset(engine, "GamePlayers");
    actorOfPlayer = obj::PropertyOffset(player, "Actor");
    controllerId = obj::PropertyOffset(localPlayer, "ControllerId");
    pawnOfController = obj::PropertyOffset(controller, "Pawn");
    weapon = obj::PropertyOffset(pawn, "Weapon");
    health = obj::PropertyOffset(pawn, "Health");
    worldInfoOfActor = obj::PropertyOffset(actor, "WorldInfo");
    pauser = obj::PropertyOffset(worldInfo, "Pauser");
    cinematic = obj::PropertyOffset(playerController, "bCinematicMode");
    cinematicMask = obj::BoolMask(playerController, "bCinematicMode");
    meleeing = obj::PropertyOffset(gearPawn, "bDoingMeleeAttack");
    meleeingMask = obj::BoolMask(gearPawn, "bDoingMeleeAttack");
    REXLOG_INFO("adaptive triggers: GamePlayers +0x{:X}, Actor +0x{:X}, ControllerId +0x{:X}, Pawn +0x{:X}, Weapon +0x{:X}, "
                "Health +0x{:X}, Pauser +0x{:X}, bCinematicMode +0x{:X}/{:08X}, bDoingMeleeAttack +0x{:X}/{:08X}",
                gamePlayers, actorOfPlayer, controllerId, pawnOfController, weapon, health, pauser, cinematic,
                cinematicMask, meleeing, meleeingMask);
    done = true;
    return true;
  }
  bool Usable() const {
    return gamePlayers >= 0 && actorOfPlayer >= 0 && controllerId >= 0 && pawnOfController >= 0 && weapon >= 0;
  }
  bool Flag(uint32_t object, int offset, uint32_t mask) const {
    return offset >= 0 && mask && (obj::Read32(object + uint32_t(offset)) & mask) != 0;
  }
} g_p;

// weapon class -> kWeapons index (kOff: not a GearWeapon); the class's name index is kept so a class object freed with
// its level and the address reused by another is looked up again
struct Cached { uint32_t name; int feel; };
std::unordered_map<uint32_t, Cached> g_classes;

int FeelOfClass(uint32_t cls) {
  const uint32_t name = obj::Read32(cls + 44);
  if (auto it = g_classes.find(cls); it != g_classes.end() && it->second.name == name) return it->second.feel;
  int feel = kOff;
  std::string chain;
  for (uint32_t c = cls, guard = 0; c && guard < 32 && feel == kOff; c = obj::Read32(c + 64), guard++) {
    const std::string n = obj::ObjectName(c);
    for (int i = 0; i < int(std::size(kWeapons)); i++)
      if (n == kWeapons[i].cls) { feel = i; break; }
  }
  if (g_classes.size() > 256) g_classes.clear();
  g_classes[cls] = {name, feel};
  REXLOG_INFO("adaptive triggers: weapon class {} -> {}", obj::ObjectName(cls), feel >= 0 ? kWeapons[feel].what : "off");
  return feel;
}

// One local player's effect now
int FeelOf(uint32_t localPlayer, bool frontEnd) {
  if (frontEnd) return kOff;
  const uint32_t pc = obj::Read32(localPlayer + uint32_t(g_p.actorOfPlayer));
  if (!pc || !obj::IsA(pc, g_p.playerController)) return kOff;
  if (g_p.Flag(pc, g_p.cinematic, g_p.cinematicMask)) return kOff;
  const uint32_t pawn = obj::Read32(pc + uint32_t(g_p.pawnOfController));
  if (!pawn || !obj::IsA(pawn, g_p.pawn)) return kOff;
  if (g_p.health >= 0 && int32_t(obj::Read32(pawn + uint32_t(g_p.health))) <= 0) return kOff;
  if (g_p.worldInfoOfActor >= 0 && g_p.pauser >= 0) {
    const uint32_t wi = obj::Read32(pawn + uint32_t(g_p.worldInfoOfActor));
    if (wi && obj::Read32(wi + uint32_t(g_p.pauser))) return kOff;
  }
  const uint32_t weapon = obj::Read32(pawn + uint32_t(g_p.weapon));
  if (!weapon) return kOff;
  if (obj::IsA(weapon, g_p.lancer) && obj::IsA(pawn, g_p.gearPawn) && g_p.Flag(pawn, g_p.meleeing, g_p.meleeingMask))
    return kChainsaw;
  const uint32_t cls = obj::ClassOf(weapon);
  return cls ? FeelOfClass(cls) : kOff;
}

const Feel& FeelFor(int feel) { return feel >= 0 ? kWeapons[feel] : feel == kChainsaw ? kChainsawFeel : kOffFeel; }

rex::input::InputSystem* Input() {
  static rex::input::InputSystem* input = nullptr;
  if (!input)
    if (auto* rt = rex::Runtime::instance()) input = static_cast<rex::input::InputSystem*>(rt->input_system());
  return input;
}

}  // namespace

// Game thread, every engine tick (game_patches.cpp Gow3MaxTickRate). in_front_end: the title screen's menus are open.
void Gow3AdaptiveTriggersTick(bool in_front_end) {
  static int enabled = -1;   // --pad_adaptive_triggers is read once (a launcher restart changes it)
  if (enabled < 0) {
    enabled = rex::input::InputSystem::AdaptiveTriggersEnabled() ? 1 : 0;
    if (enabled) REXLOG_INFO("adaptive triggers: Gears 3 weapon feel for DualSense pads on (GitHub #245)");
  }
  if (!enabled) return;
  static Clock::time_point next{};
  const Clock::time_point now = Clock::now();
  if (now < next) return;
  next = now + std::chrono::milliseconds(125);
  if (!obj::Ready() && !obj::Init(gow3::addr::kGNamesVar, gow3::addr::kGObjectsVar)) return;
  if (!g_p.Find()) return;
  if (!g_p.Usable()) {
    static bool warned = false;
    if (!warned) REXLOG_WARN("adaptive triggers: player properties not found - triggers left alone");
    warned = true;
    return;
  }
  rex::input::InputSystem* input = Input();
  if (!input) return;

  // what each pad's player should feel now (players not in the game: off)
  std::array<int, 4> want;
  want.fill(kOff);
  if (const uint32_t engine = obj::Read32(gow3::addr::kGEngine)) {
    const uint32_t data = obj::Read32(engine + uint32_t(g_p.gamePlayers));   // TArray: data, count, max
    const uint32_t count = std::min<uint32_t>(obj::Read32(engine + uint32_t(g_p.gamePlayers) + 4), 4);
    for (uint32_t i = 0; data && i < count; i++) {
      const uint32_t lp = obj::Read32(data + 4 * i);
      if (!lp || !obj::IsA(lp, g_p.localPlayer)) continue;
      const uint32_t user = obj::Read32(lp + uint32_t(g_p.controllerId));
      if (user < want.size()) want[user] = FeelOf(lp, in_front_end);
    }
  }

  static std::array<int, 4> sent = {kNone, kNone, kNone, kNone};
  static std::array<Clock::time_point, 4> sent_at{};
  for (uint32_t user = 0; user < want.size(); user++) {
    if (sent[user] == kNone && want[user] == kOff) continue;   // never touched: leave the pad alone
    const bool changed = want[user] != sent[user];
    if (!changed && now - sent_at[user] < std::chrono::seconds(1)) continue;
    const Feel& f = FeelFor(want[user]);
    input->SetTriggerEffect(user, f.right.data(), f.left.data());
    if (changed) REXLOG_INFO("adaptive triggers: player pad {} - {}", user + 1, f.what);
    sent[user] = want[user];
    sent_at[user] = now;
  }
}
