// Layer 3 (Gears of War 1, TU5): GitHub #245 - DualSense adaptive triggers per weapon (--pad_adaptive_triggers, off by
// default; the launcher's Controls tab "DualSense adaptive triggers"). Gears2-Native's layer3_gow2/adaptive_triggers.cpp
// with Gears 1's classes (WarfareGame: WarPawn, Weap_*), its globals, and this game's own object reflection
// (layer2_ue3/objects.cpp, the layout given - the one developer mode's scripted controller uses). Idea and the first
// working version: HubertFer's separate plugin (#245). Our code (MIT).
//
// The runtime (SDK patch 98, InputSystem::SetTriggerEffect) sends a trigger effect to the PS5 pads of one player and
// remembers it: the same effect again writes nothing, a reconnected pad gets it back on the next call, COGBox's own
// menus and the system dialogs turn the triggers off while open, and the pads are reset when the game's window closes.
// Other controllers, the keyboard and mouse are never written to.
//
// This file decides the effect. Every 125 ms (game thread: cheats.cpp's FEngineLoop::Tick override, which runs whether
// --gow1_cheats is on or not), by reflection (ue3/objects.h): GEngine.GamePlayers[i] (LocalPlayer) -> ControllerId (the
// pad's XInput user) and Actor (its PlayerController) -> Pawn -> Weapon -> the weapon's class, looked up by name up its
// superclass chain (most derived first) in kWeapons below. Off when: no pawn (menus, loading, spectating), the pawn is
// dead, the game is paused (WorldInfo.Pauser), a cutscene runs (PlayerController.bCinematicMode). The Lancer's chainsaw
// (holding a Lancer and WarPawn.bChainsawing or the Lancer's bChainSawRunning) gets its own rumble on both triggers.
// A player's effect is sent when it changes, plus once a second (the runtime drops a repeat without a write - that is
// how a pad that reconnected or joined that player gets it).
//
// Everything that searches (the tables, every class and property lookup - this game's objects.cpp has only full-table
// FindClass) runs once on a worker thread when the option is on; the game thread starts only after that
// (g_objects_warm), so it never stalls a frame. With the option off: one flag test per tick, no thread.
//
// DS5 trigger effect bytes (11 per trigger; byte 0 = mode). Positions and strengths 0-255 (0 = trigger at rest).
//   0x05 off                                         {0x05}
//   0x01 continuous resistance from a position       {0x01, start, force}                  [V] SDL 3.4 testcontroller.c
//   0x02 section resistance (a "click" to pass)      {0x02, start, end, force}             [E] Nielk1's DS5 notes
//   0x06 vibration beyond a position                 {0x06, frequency Hz, amplitude, start} [V] SDL test {0x06,15,63,128}
#include <rex/input/input_system.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>

#include <ue3/objects.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace {
namespace obj = ue3::obj;
using Clock = std::chrono::steady_clock;
using Effect = std::array<uint8_t, 11>;

// Gears 1 TU5 (the one build: gow1_manifest.toml), from the recompiled code:
//  GNames (TArray<FNameEntry*>): FName::ToString sub_82377A20 `lis r11,-32001` / `lwz r11,-11024(r11)`, `rlwinm idx,2`,
//    `lwzx`, text at entry +16 (16-bit characters), the FName number at +4 appended - the layout below says the same.
//    14 loads / 11 address-of readers. [V code]
//  GObjObjects (TArray<UObject*>): UObject's destructor sub_822BA970 reads Index at +4, `lwz r11,-11836(lis -32002)`,
//    stores 0 with `stwx`, adds the index to GObjAvailable (`addi r3,..,-11824` = +12) - the same sequence as Gears 2's
//    destructor (sub_82286650 there). 85 address-of readers. [V code]
//  GEngine: cheats.cpp's (r7: FEngineLoop::Init's "-EXEC=" code). [V code]
//  Layout: gow1_app.h's (developer mode's; found by Discover on TU5, KiltedKernel) [V there]. Given, so no Discover here.
constexpr const char* kVersion = "TU5";
constexpr uint32_t kGNamesVar = 0x82FED4F0u;
constexpr uint32_t kGObjectsVar = 0x82FDD1C4u;
constexpr uint32_t kGEngine = 0x82FCDD50u;
constexpr const char* kLayout = "4,40,44,52,60,76,64,72,96,124,16,1,1";

constexpr Effect Off() { return {0x05}; }
constexpr Effect Res(uint8_t start, uint8_t force) { return {0x01, start, force}; }
constexpr Effect Click(uint8_t start, uint8_t end, uint8_t force) { return {0x02, start, end, force}; }
constexpr Effect Vib(uint8_t hz, uint8_t amplitude, uint8_t start) { return {0x06, hz, amplitude, start}; }

struct Feel {
  const char* cls;   // a weapon class or one of its base classes (Gears 1's WarfareGame script)
  const char* what;  // for the log
  Effect right, left;
};
// Right trigger = fire, left = aim. Gears 3's first tuning for the weapons the games share. Checked in order of the
// weapon's class chain, most derived class first.
// Class names and supers [V] Gears 1's WarfareGame script package (TU5 WarfareGame.xxx, LZO, export table parsed):
// Weap_AssaultRifle <- Weap_AssaultRifleBase <- WarWeapon <- Weapon; Weap_LocustAssaultRifle, Weap_SniperRifle,
// Weap_Grenade, Weap_PistolBase, Weap_WretchMelee <- WarWeapon; Weap_Shotgun <- Weap_MeleeComboBase <- WarWeapon;
// Weap_COGPistol / Weap_LocustPistol <- Weap_PistolBase. Weap_Boomer, Weap_Bow and Weap_HammerOfDawn live in the
// content package WarfareGameContentWeapons [V: the package's import names] - matched by name, their parents not seen.
// Weap_MiniGun only as a name [E: the Troika's gun]. Not listed (default "weapon" feel): WarVehicleWeapon, Weap_PhysicsGun.
const Feel kWeapons[] = {
    {"Weap_AssaultRifle", "Lancer", Vib(12, 80, 40), Res(32, 60)},
    {"Weap_LocustAssaultRifle", "Hammerburst", Click(64, 128, 160), Res(32, 60)},
    {"Weap_Shotgun", "Gnasher", Click(80, 160, 224), Res(32, 60)},
    {"Weap_SniperRifle", "Longshot", Click(112, 160, 192), Res(16, 128)},
    {"Weap_Boomer", "Boomshot", Click(80, 176, 255), Res(32, 96)},
    {"Weap_Bow", "Torque Bow", Res(16, 160), Res(32, 80)},
    {"Weap_HammerOfDawn", "Hammer of Dawn", Res(32, 96), Res(32, 60)},
    {"Weap_LocustPistol", "Boltok", Click(96, 160, 224), Res(32, 60)},
    {"Weap_COGPistol", "Snub pistol", Click(48, 96, 128), Res(32, 48)},
    {"Weap_PistolBase", "pistol", Click(48, 96, 128), Res(32, 48)},
    {"Weap_Grenade", "grenade", Res(64, 80), Res(32, 48)},
    {"Weap_MiniGun", "Troika", Vib(18, 160, 32), Res(0, 128)},
    {"Weap_WretchMelee", "melee", Off(), Off()},
    {"WarWeapon", "weapon", Res(48, 64), Res(32, 48)},
};
constexpr int kNone = -1;      // nothing sent to this player yet
constexpr int kOff = -2;       // no weapon / not playing
constexpr int kChainsaw = -3;  // the Lancer's chainsaw
const Feel kChainsawFeel = {"", "chainsaw", Vib(30, 255, 0), Vib(30, 255, 0)};
const Feel kOffFeel = {"", "off", Off(), Off()};

// UGameEngine::GamePlayers in Gears 1's engine, from the code (cheats.cpp: `lwz r11,0x29C(r3)` after Cast<UGameEngine>).
// Reflection must find the same, or the given layout is wrong (then the triggers are left alone).
constexpr int kGamePlayersFromCode = 0x29C;

struct Props {
  uint32_t engine = 0, player = 0, localPlayer = 0, controller = 0, playerController = 0, pawn = 0, actor = 0,
           worldInfo = 0, warPawn = 0, lancer = 0;
  int gamePlayers = -1, actorOfPlayer = -1, controllerId = -1, pawnOfController = -1, weapon = -1, health = -1,
      worldInfoOfActor = -1, pauser = -1, cinematic = -1, sawing = -1, sawRunning = -1;
  uint32_t cinematicMask = 0, sawingMask = 0, sawRunningMask = 0;

  // Worker thread only (full-table FindClass). true once the classes the effect needs are found; the two WarfareGame
  // classes (the chainsaw check only) are not waited for when `last_try`.
  bool Find(bool last_try) {
    auto find = [](std::initializer_list<std::pair<uint32_t*, const char*>> classes) {
      bool all = true;
      for (const auto& [cls, name] : classes) {
        if (!*cls) *cls = obj::FindClass(name);
        all = all && *cls;
      }
      return all;
    };
    const bool required = find({{&engine, "GameEngine"}, {&player, "Player"}, {&localPlayer, "LocalPlayer"},
                                {&controller, "Controller"}, {&playerController, "PlayerController"}, {&pawn, "Pawn"},
                                {&actor, "Actor"}, {&worldInfo, "WorldInfo"}});
    const bool optional = find({{&warPawn, "WarPawn"}, {&lancer, "Weap_AssaultRifle"}});
    if (!required || (!optional && !last_try)) return false;
    gamePlayers = obj::PropertyOffset(engine, "GamePlayers");   // (GameEngine: found where Engine or it declares it)
    actorOfPlayer = obj::PropertyOffset(player, "Actor");
    controllerId = obj::PropertyOffset(localPlayer, "ControllerId");
    pawnOfController = obj::PropertyOffset(controller, "Pawn");
    weapon = obj::PropertyOffset(pawn, "Weapon");
    health = obj::PropertyOffset(pawn, "Health");
    worldInfoOfActor = obj::PropertyOffset(actor, "WorldInfo");
    pauser = obj::PropertyOffset(worldInfo, "Pauser");
    cinematic = obj::PropertyOffset(playerController, "bCinematicMode");
    cinematicMask = obj::BoolMask(playerController, "bCinematicMode");
    if (warPawn) {   // (Gears 1's PropertyOffset / BoolMask are not given class 0)
      sawing = obj::PropertyOffset(warPawn, "bChainsawing");
      sawingMask = obj::BoolMask(warPawn, "bChainsawing");
    }
    if (lancer) {
      sawRunning = obj::PropertyOffset(lancer, "bChainSawRunning");
      sawRunningMask = obj::BoolMask(lancer, "bChainSawRunning");
    }
    REXLOG_INFO("adaptive triggers: GamePlayers +0x{:X}, Actor +0x{:X}, ControllerId +0x{:X}, Pawn +0x{:X}, Weapon +0x{:X}, "
                "Health +0x{:X}, Pauser +0x{:X}, bCinematicMode +0x{:X}/{:08X}, bChainsawing +0x{:X}/{:08X}, "
                "bChainSawRunning +0x{:X}/{:08X}",
                gamePlayers, actorOfPlayer, controllerId, pawnOfController, weapon, health, pauser, cinematic,
                cinematicMask, sawing, sawingMask, sawRunning, sawRunningMask);
    if (gamePlayers != kGamePlayersFromCode) {   // the layout is given, not found: a mismatch means it is wrong
      REXLOG_WARN("adaptive triggers: GamePlayers at +{} by reflection, +{} in the code - the object layout is wrong",
                  gamePlayers, kGamePlayersFromCode);
      gamePlayers = -1;   // (Usable() fails: triggers left alone)
    }
    return true;
  }
  bool Usable() const {
    return gamePlayers >= 0 && actorOfPlayer >= 0 && controllerId >= 0 && pawnOfController >= 0 && weapon >= 0;
  }
  bool Flag(uint32_t object, int offset, uint32_t mask) const {
    return offset >= 0 && mask && (obj::Read32(object + uint32_t(offset)) & mask) != 0;
  }
} g_p;   // written by the worker before g_objects_warm (release), read by the game thread after it (acquire)

// weapon class -> kWeapons index (kOff: not a weapon); the class's name index is kept so a class object freed with its
// level and the address reused by another is looked up again
struct Cached { uint32_t name; int feel; };
std::unordered_map<uint32_t, Cached> g_classes;

int FeelOfClass(uint32_t cls) {
  const obj::Layout& L = obj::CurrentLayout();
  const uint32_t name = obj::Read32(cls + L.name);
  if (auto it = g_classes.find(cls); it != g_classes.end() && it->second.name == name) return it->second.feel;
  int feel = kOff;
  for (uint32_t c = cls, guard = 0; c && guard < 32 && feel == kOff; c = obj::Read32(c + L.super), guard++) {
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
int FeelOf(uint32_t localPlayer) {
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
  if (g_p.lancer && obj::IsA(weapon, g_p.lancer) &&
      ((g_p.warPawn && obj::IsA(pawn, g_p.warPawn) && g_p.Flag(pawn, g_p.sawing, g_p.sawingMask)) ||
       g_p.Flag(weapon, g_p.sawRunning, g_p.sawRunningMask)))
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

// The tables (the engine's globals, layout given) and every lookup, on a worker thread: every 2 s while the engine
// starts, then every 15 s; given up after ~12 minutes (a warning; triggers left alone). The two chainsaw classes are
// waited for 5 tries after the others are there.
std::atomic<bool> g_objects_warm{false};
void StartObjectsInit() {
  std::thread([] {
    obj::Layout layout;
    obj::ParseLayout(kLayout, &layout);
    bool found = false;
    for (int attempt = 0, after_ready = 0; attempt < 60 && !found; attempt++) {
      if (obj::Init(kGNamesVar, kGObjectsVar, &layout)) found = g_p.Find(++after_ready > 5 || attempt == 59);
      if (!found) std::this_thread::sleep_for(std::chrono::seconds(attempt < 15 ? 2 : 15));
    }
    if (!found) {
      REXLOG_WARN("adaptive triggers: the engine's object tables or classes were not found - triggers left alone");
      return;
    }
    g_objects_warm.store(true, std::memory_order_release);
  }).detach();
}

}  // namespace

// Game thread, every engine tick (cheats.cpp, the FEngineLoop::Tick override).
void Gow1AdaptiveTriggersTick() {
  static int enabled = -1;   // --pad_adaptive_triggers is read once (a launcher restart changes it)
  if (enabled < 0) {
    enabled = rex::input::InputSystem::AdaptiveTriggersEnabled() ? 1 : 0;
    if (enabled) {
      REXLOG_INFO("adaptive triggers: Gears 1 weapon feel for DualSense pads on (GitHub #245), {}", kVersion);
      StartObjectsInit();
    }
  }
  if (!enabled || !g_objects_warm.load(std::memory_order_acquire)) return;
  static Clock::time_point next{};
  const Clock::time_point now = Clock::now();
  if (now < next) return;
  next = now + std::chrono::milliseconds(125);
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
  if (const uint32_t engine = obj::Read32(kGEngine)) {
    const uint32_t data = obj::Read32(engine + uint32_t(g_p.gamePlayers));   // TArray: data, count, max
    const uint32_t count = std::min<uint32_t>(obj::Read32(engine + uint32_t(g_p.gamePlayers) + 4), 4);
    for (uint32_t i = 0; data && i < count; i++) {
      const uint32_t lp = obj::Read32(data + 4 * i);
      if (!lp || !obj::IsA(lp, g_p.localPlayer)) continue;
      const uint32_t user = obj::Read32(lp + uint32_t(g_p.controllerId));
      if (user < want.size()) want[user] = FeelOf(lp);
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
