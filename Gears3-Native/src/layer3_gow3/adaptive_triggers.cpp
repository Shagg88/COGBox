// Layer 3 (Gears of War 3): GitHub #245 - DualSense adaptive triggers per weapon (--pad_adaptive_triggers, off by
// default; the launcher's Controls tab "DualSense adaptive triggers"). Idea and the first working version: HubertFer's
// separate plugin (#245); this is a rewrite on COGBox's own pieces. Our code (MIT).
//
// 0.6.3: the same weapon table and "playing" test also drive Xbox One / Series impulse triggers (--pad_trigger_rumble,
// off by default; the launcher's Controls tab "Trigger rumble (Xbox controllers)"; SDK patch 104,
// InputSystem::SetTriggerRumble): a short pulse on the trigger motors per shot, scaled by weapon, and a buzz while a
// continuous weapon fires or the chainsaw revs - the "impulse triggers" section at the end.
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
#include <rex/cvar.h>
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

// 0.6.3 (GitHub #245): Xbox One / Series impulse triggers (--pad_trigger_rumble) - a pulse on the trigger motors:
// strengths in % of the motor (right = the fire trigger, left = the aim trigger), length in ms. A kick is one pulse per
// shot; hold = a buzz for as long as the weapon fires (flamethrower, beam, miniguns - continuous actions only).
struct Kick { uint8_t right, left; uint16_t ms; bool hold; };
constexpr Kick K(uint8_t right, uint8_t left, uint16_t ms) { return {right, left, ms, false}; }
constexpr Kick Hold(uint8_t right, uint8_t left) { return {right, left, 100, true}; }
constexpr Kick NoKick() { return {0, 0, 0, false}; }

struct Feel {
  const char* cls;   // a weapon class or one of its base classes (Gears 3's GearGame script)
  const char* what;  // for the log
  Effect right, left;  // DualSense
  Kick kick;           // Xbox impulse triggers
};
// Right trigger = fire, left = aim. First tuning (needs a DualSense in hand - test plan in the notes): automatics buzz,
// single shots click, heavy and charged weapons push back; aiming is a light pull, heavier with heavy weapons.
// Checked in order of the weapon's class chain, most derived class first, so a more specific class wins.
const Feel kWeapons[] = {
    {"GearWeap_AssaultRifle", "Lancer", Vib(12, 80, 40), Res(32, 60), K(30, 0, 45)},
    {"GearWeap_RetroLancer", "Retro Lancer", Vib(9, 150, 40), Res(32, 80), K(45, 10, 60)},
    {"GearWeap_LocustAssaultRifle", "Hammerburst", Click(64, 128, 160), Res(32, 60), K(40, 0, 55)},
    {"GearWeap_Shotgun_SawedOff", "Sawed-off Shotgun", Click(96, 176, 255), Res(32, 96), K(100, 45, 150)},
    {"GearWeap_Shotgun", "Gnasher", Click(80, 160, 224), Res(32, 60), K(85, 25, 110)},
    {"GearWeap_SniperRifle", "Longshot", Click(112, 160, 192), Res(16, 128), K(75, 15, 100)},
    {"GearWeap_BoomshotBase", "Boomshot", Click(80, 176, 255), Res(32, 96), K(100, 35, 150)},
    {"GearWeap_LocustBurstPistolBase", "Gorgon", Vib(15, 96, 64), Res(32, 48), K(30, 0, 40)},
    {"GearWeap_LocustPistol", "Boltok", Click(96, 160, 224), Res(32, 60), K(70, 10, 90)},
    {"GearWeap_PistolBase", "pistol", Click(48, 96, 128), Res(32, 48), K(40, 0, 50)},
    {"GearWeap_HeavyMiniGunBase", "Mulcher", Vib(20, 176, 32), Res(0, 160), Hold(45, 25)},
    {"GearWeap_HeavyMortarBase", "Mortar", Res(32, 192), Res(0, 160), K(100, 50, 180)},
    {"GearWeap_HeavySniperBase", "OneShot", Res(48, 255), Res(0, 144), K(100, 40, 160)},
    {"GearWeap_DiggerLauncher", "Digger", Click(64, 144, 176), Res(32, 80), K(70, 15, 100)},
    {"GearWeap_BowBase", "Torque Bow", Res(16, 160), Res(32, 80), K(70, 15, 100)},
    {"GearWeap_FlameThrowerBase", "Scorcher", Vib(40, 64, 32), Res(32, 60), Hold(25, 0)},
    {"GearWeap_GrenadeBase", "grenade", Res(64, 80), Res(32, 48), K(20, 0, 40)},
    {"GearWeap_HODBase", "Hammer of Dawn", Res(32, 96), Res(32, 60), Hold(20, 0)},
    {"GearWeap_CleaverBase", "Cleaver", Off(), Off(), NoKick()},
    {"GearWeap_HeavyBase", "heavy weapon", Res(32, 144), Res(0, 144), K(80, 25, 120)},
    {"GearWeap_Melee", "melee", Off(), Off(), NoKick()},
    {"GearWeapon", "weapon", Res(48, 64), Res(32, 48), K(30, 0, 45)},
};
constexpr int kNone = -1;      // nothing sent to this player yet
constexpr int kOff = -2;       // no weapon / not playing
constexpr int kChainsaw = -3;  // the Lancer's chainsaw
const Feel kChainsawFeel = {"", "chainsaw", Vib(30, 255, 0), Vib(30, 255, 0), {35, 35, 100, true}};   // (while it revs)
const Feel kOffFeel = {"", "off", Off(), Off(), NoKick()};

// --pad_trigger_rumble (0.6.3), set by the tick before the first table search (and before the worker thread starts)
bool g_impulseOn = false;

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
    // 0.6.3 impulse triggers: their classes, only with the option on; waited for as the optional ones
    const bool impulse = !g_impulseOn || step({{&ammoClass, "GearWeapon"}, {&ffmClass, "ForceFeedbackManager"}});
    if (cursor + 16384 >= total) cursor = 0, passes++;
    else cursor += 16384;
    if (!required || (!(optional && impulse) && passes < 2)) return false;
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
    if (g_impulseOn) FindImpulse();
    done = true;
    return true;
  }
  // 0.6.3 impulse triggers (GitHub #245): the engine's per-shot signals on Pawn, the weapon's ammo count and the game's
  // vibration option (VibrationAllowed). -1 / 0: not found - that signal is not used.
  uint32_t ammoClass = 0, priClass = 0, ffmClass = 0;
  int flashCount = -1, flashLocation = -1, ammoUsed = -1, pri = -1, priVibration = -1, ffm = -1, ffmAllows = -1;
  uint32_t priVibrationMask = 0, ffmAllowsMask = 0;
  void FindImpulse() {
    flashCount = obj::PropertyOffset(pawn, "FlashCount");
    if (flashCount >= 0 && obj::PropertySize(pawn, "FlashCount") != 1) flashCount = -1;   // a byte (the word's first)
    flashLocation = obj::PropertyOffset(pawn, "FlashLocation");
    if (flashLocation >= 0 && obj::PropertySize(pawn, "FlashLocation") != 12) flashLocation = -1;   // a vector
    if (ammoClass) ammoUsed = obj::PropertyOffset(ammoClass, "AmmoUsedCount");
    if (ammoUsed >= 0 && obj::PropertySize(ammoClass, "AmmoUsedCount") != 4) ammoUsed = -1;
    if (priClass) {
      pri = obj::PropertyOffset(controller, "PlayerReplicationInfo");
      priVibration = obj::PropertyOffset(priClass, "bControllerVibrationAllowed");
      priVibrationMask = priVibration >= 0 ? obj::BoolMask(priClass, "bControllerVibrationAllowed") : 0;
    }
    if (ffmClass) {
      ffm = obj::PropertyOffset(playerController, "ForceFeedbackManager");
      ffmAllows = obj::PropertyOffset(ffmClass, "bAllowsForceFeedback");
      ffmAllowsMask = ffmAllows >= 0 ? obj::BoolMask(ffmClass, "bAllowsForceFeedback") : 0;
    }
    for (uint32_t* mask : {&priVibrationMask, &ffmAllowsMask})
      if (*mask & (*mask - 1)) *mask = 0;   // one bit, or the layout is off: not used
    REXLOG_INFO("impulse triggers: Pawn.FlashCount +0x{:X}, Pawn.FlashLocation +0x{:X}, AmmoUsedCount +0x{:X}, "
                "Controller.PlayerReplicationInfo +0x{:X} .bControllerVibrationAllowed +0x{:X}/{:08X}, "
                "PlayerController.ForceFeedbackManager +0x{:X} .bAllowsForceFeedback +0x{:X}/{:08X} (-1: not found)",
                flashCount, flashLocation, ammoUsed, pri, priVibration, priVibrationMask, ffm, ffmAllows,
                ffmAllowsMask);
    if (flashCount < 0 && flashLocation < 0 && ammoUsed < 0)
      REXLOG_WARN("impulse triggers: no shot signal found - only the chainsaw and held weapons can pulse");
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

// What one local player is doing now: the feel (kOff when not playing), the pawn and weapon, and whether the player
// allows vibration in the game's options. (The 0.6.3 impulse triggers read the pawn and weapon too.)
struct Seen {
  int feel = kOff;
  uint32_t pawn = 0, weapon = 0;
  bool vibration = true;
};

// The game's own Vibration option: GearPC.UpdateLocalCacheOfProfileSettings sets ForceFeedbackManager.
// bAllowsForceFeedback = ProfileSettings.GetControllerVibrationOption() [V Gears 3's GearGame script].
// Not found: allowed (COGBox's own Vibration setting still applies).
constexpr bool kVibrationFromManager = true, kVibrationFromPri = false;
bool VibrationAllowed(uint32_t pc) {   // (every flag read must allow it)
  bool allowed = true;
  if (kVibrationFromManager && g_p.ffm >= 0 && g_p.ffmAllowsMask) {
    const uint32_t manager = obj::Read32(pc + uint32_t(g_p.ffm));
    if (manager && obj::IsA(manager, g_p.ffmClass)) allowed = g_p.Flag(manager, g_p.ffmAllows, g_p.ffmAllowsMask);
  }
  if (kVibrationFromPri && g_p.pri >= 0 && g_p.priVibrationMask) {
    const uint32_t pri = obj::Read32(pc + uint32_t(g_p.pri));
    if (pri && obj::IsA(pri, g_p.priClass)) allowed = allowed && g_p.Flag(pri, g_p.priVibration, g_p.priVibrationMask);
  }
  return allowed;
}

// One local player's effect now
Seen Look(uint32_t localPlayer, bool frontEnd) {
  Seen s;
  if (frontEnd) return s;
  const uint32_t pc = obj::Read32(localPlayer + uint32_t(g_p.actorOfPlayer));
  if (!pc || !obj::IsA(pc, g_p.playerController)) return s;
  s.vibration = VibrationAllowed(pc);
  if (g_p.Flag(pc, g_p.cinematic, g_p.cinematicMask)) return s;
  const uint32_t pawn = obj::Read32(pc + uint32_t(g_p.pawnOfController));
  if (!pawn || !obj::IsA(pawn, g_p.pawn)) return s;
  s.pawn = pawn;
  if (g_p.health >= 0 && int32_t(obj::Read32(pawn + uint32_t(g_p.health))) <= 0) return s;
  if (g_p.worldInfoOfActor >= 0 && g_p.pauser >= 0) {
    const uint32_t wi = obj::Read32(pawn + uint32_t(g_p.worldInfoOfActor));
    if (wi && obj::Read32(wi + uint32_t(g_p.pauser))) return s;
  }
  const uint32_t weapon = obj::Read32(pawn + uint32_t(g_p.weapon));
  if (!weapon) return s;
  s.weapon = weapon;
  if (obj::IsA(weapon, g_p.lancer) && obj::IsA(pawn, g_p.gearPawn) && g_p.Flag(pawn, g_p.meleeing, g_p.meleeingMask))
    s.feel = kChainsaw;
  else if (const uint32_t cls = obj::ClassOf(weapon))
    s.feel = FeelOfClass(cls);
  return s;
}

const Feel& FeelFor(int feel) { return feel >= 0 ? kWeapons[feel] : feel == kChainsaw ? kChainsawFeel : kOffFeel; }

rex::input::InputSystem* Input() {
  static rex::input::InputSystem* input = nullptr;
  if (!input)
    if (auto* rt = rex::Runtime::instance()) input = static_cast<rex::input::InputSystem*>(rt->input_system());
  return input;
}

// ---- 0.6.3 (GitHub #245): Xbox One / Series impulse triggers (--pad_trigger_rumble, SDK patch 104) ----------------
// The same weapon and the same "playing" test as the DualSense feel above, read every engine tick. The events are the
// engine's own per-shot signals on the player's pawn: Pawn.FlashCount (a byte the projectile / grenade / burst weapons
// step once per shot, 0 when firing stops) and Pawn.FlashLocation (the instant-hit weapons' hit point - a new one each
// shot, (0,0,0) when firing stops); a weapon whose flash signals never moved (a client whose shots are decided by the
// host) uses its AmmoUsedCount going up instead (a hold weapon always counts it too). A shot is one kick (the row's
// Kick); a hold weapon buzzes while its shots keep coming (one in the last 250 ms - not "a flash signal is set", which
// a game might never clear); the chainsaw buzzes both triggers while it revs. Nothing - and a running pulse stopped -
// while the player is not playing (the DualSense "off" cases: menus, pause, cutscenes, death, no weapon) or has
// vibration off in the game's options. The runtime adds COGBox's Vibration setting, its menus and the window focus, and
// writes only to pads that have trigger motors. Per player (split screen: each pad its own).
struct Pulse {
  uint32_t pawn = 0, weapon = 0;   // the counters below are this pawn's and weapon's
  uint32_t flashCount = 0, ammo = 0;
  std::array<uint32_t, 3> flashLocation{};
  bool flashMoved = false;         // this weapon's flash signals moved (then its ammo count is not used)
  Clock::time_point lastShot{};    // (hold weapons)
  bool running = false;            // a pulse was sent since the last stop
  bool continuous = false;         // the last tick asked for a hold / chainsaw buzz
};
std::array<Pulse, 4> g_pulse;

uint16_t Strength(uint8_t percent) { return uint16_t(std::min<uint32_t>(percent, 100) * 65535u / 100u); }

void ImpulseTick(rex::input::InputSystem* input, uint32_t user, const Seen& s, Clock::time_point now) {
  Pulse& p = g_pulse[user];
  if (s.feel == kOff || !s.vibration) {
    if (p.running) input->SetTriggerRumble(user, 0, 0, 0);
    p = Pulse{};
    return;
  }
  // the shot signals now (a word read at the byte's offset: big-endian, the byte is its top 8 bits)
  const uint32_t count = g_p.flashCount >= 0 ? obj::Read32(s.pawn + uint32_t(g_p.flashCount)) >> 24 : 0;
  std::array<uint32_t, 3> location{};
  if (g_p.flashLocation >= 0)
    for (uint32_t i = 0; i < 3; i++) location[i] = obj::Read32(s.pawn + uint32_t(g_p.flashLocation) + 4 * i);
  const bool located = ((location[0] | location[1] | location[2]) & 0x7FFFFFFFu) != 0;   // (-0.0 is 0.0; unset)
  const uint32_t ammo = g_p.ammoUsed >= 0 && obj::IsA(s.weapon, g_p.ammoClass)
                            ? obj::Read32(s.weapon + uint32_t(g_p.ammoUsed)) : 0;
  const Kick& k = FeelFor(s.feel).kick;
  bool fired = false;
  if (p.pawn == s.pawn && p.weapon == s.weapon) {   // (a new pawn or weapon: counters taken, no kick)
    const bool flash = (count && count != p.flashCount) || (located && location != p.flashLocation);
    p.flashMoved = p.flashMoved || flash;
    // a hold weapon also counts its ammo going up after its flash signals moved (one that steps them once per pull
    // and then holds them would otherwise stop buzzing while it still fires); a kick weapon does not, or a client
    // whose host's count arrives later would kick twice
    fired = flash || ((!p.flashMoved || k.hold) && int32_t(ammo - p.ammo) > 0);
  } else {
    p.flashMoved = false;
    p.lastShot = {};
  }
  if (fired) p.lastShot = now;
  p.pawn = s.pawn, p.weapon = s.weapon, p.flashCount = count, p.flashLocation = location, p.ammo = ammo;

  const bool firing = p.lastShot != Clock::time_point{} && now - p.lastShot < std::chrono::milliseconds(250);
  const bool continuous = (s.feel == kChainsaw || (k.hold && firing)) && (k.right || k.left);
  if (continuous || (fired && !k.hold && (k.right || k.left))) {
    // a continuous buzz is sent again every tick (the runtime / SDL only move its end), so it never outlives the action
    // by more than its 100 ms
    input->SetTriggerRumble(user, Strength(k.left), Strength(k.right), k.ms);
    p.running = true;
  } else if (p.continuous && p.running) {
    input->SetTriggerRumble(user, 0, 0, 0);   // the buzz ends with the action (a kick ends by itself)
    p.running = false;
  }
  p.continuous = continuous;
}

}  // namespace

// Game thread, every engine tick (game_patches.cpp Gow3MaxTickRate; in_front_end: the title screen's menus are open).
// The DualSense feel every 125 ms; with --pad_trigger_rumble the players are read every tick (the impulse triggers'
// shots).
// --cogbox_test_state (tests only, log only): once a second, what the first local player is doing, from the engine's own
// flags - so an unattended test can prove it measured gameplay and not the pause menu, a cutscene or a film
// (Shagg, 20 Nov: jobs kept measuring the pause menu). A test must find "cogbox_test_state: GAMEPLAY" in the log.
REXCVAR_DEFINE_BOOL(cogbox_test_state, false, "COGBox",
                    "Tests: log once a second whether the first player is in gameplay (not paused, not in a cutscene, "
                    "has a living pawn) - log only");
static void TestStateLine(Clock::time_point now, bool in_front_end) {
  static Clock::time_point next{};
  if (now < next) return;
  next = now + std::chrono::seconds(1);
  static const int timeSeconds = obj::PropertyOffset(g_p.worldInfo, "TimeSeconds");
  uint32_t players = 0, pc = 0, pawn = 0, wi = 0;
  if (const uint32_t engine = obj::Read32(gow3::addr::kGEngine)) {
    const uint32_t data = obj::Read32(engine + uint32_t(g_p.gamePlayers));
    players = std::min<uint32_t>(obj::Read32(engine + uint32_t(g_p.gamePlayers) + 4), 4);
    const uint32_t lp = data && players ? obj::Read32(data) : 0;
    if (lp && obj::IsA(lp, g_p.localPlayer)) pc = obj::Read32(lp + uint32_t(g_p.actorOfPlayer));
  }
  if (pc && !obj::IsA(pc, g_p.playerController)) pc = 0;
  if (pc) pawn = obj::Read32(pc + uint32_t(g_p.pawnOfController));
  if (pawn && !obj::IsA(pawn, g_p.pawn)) pawn = 0;
  if (pc && g_p.worldInfoOfActor >= 0) wi = obj::Read32(pc + uint32_t(g_p.worldInfoOfActor));
  const bool cutscene = pc && g_p.Flag(pc, g_p.cinematic, g_p.cinematicMask);
  const int health = pawn && g_p.health >= 0 ? int(int32_t(obj::Read32(pawn + uint32_t(g_p.health)))) : 0;
  const bool pauseKnown = wi && g_p.pauser >= 0;
  const bool paused = pauseKnown && obj::Read32(wi + uint32_t(g_p.pauser)) != 0;
  const float t = wi && timeSeconds >= 0 ? obj::ReadFloat(wi + uint32_t(timeSeconds)) : -1.f;
  const bool gameplay = pawn && health > 0 && !cutscene && pauseKnown && !paused && !in_front_end;
  REXLOG_INFO("cogbox_test_state: {} | players {} | controller {} | pawn {} | health {} | cutscene {} | paused {} | front end {} | "
              "level time {:.1f} s",
              gameplay ? "GAMEPLAY" : "not gameplay", players, pc ? "yes" : "no", pawn ? "yes" : "no", health,
              cutscene ? "yes" : "no", !pauseKnown ? "?" : paused ? "yes" : "no", in_front_end ? "yes" : "no", t);
}

void Gow3AdaptiveTriggersTick(bool in_front_end) {
  static int adaptive = -1;   // --pad_adaptive_triggers / --pad_trigger_rumble are read once (a launcher restart)
  static bool impulse = false;
  static bool test = false;   // --cogbox_test_state
  if (adaptive < 0) {
    test = REXCVAR_GET(cogbox_test_state);
    adaptive = rex::input::InputSystem::AdaptiveTriggersEnabled() ? 1 : 0;
    impulse = g_impulseOn = rex::input::InputSystem::TriggerRumbleEnabled();
    if (adaptive) REXLOG_INFO("adaptive triggers: Gears 3 weapon feel for DualSense pads on (GitHub #245)");
    if (impulse) REXLOG_INFO("impulse triggers: Gears 3 trigger pulses for Xbox pads on (GitHub #245)");
  }
  if (!adaptive && !impulse && !test) return;
  const Clock::time_point now = Clock::now();
  static Clock::time_point next{};
  const bool step = now >= next;   // the DualSense feel's (and the table search's) 125 ms step
  if (step) next = now + std::chrono::milliseconds(125);
  if (!g_p.done) {
    if (!step) return;
    if (!obj::Ready() && !obj::Init(gow3::addr::kGNamesVar, gow3::addr::kGObjectsVar)) return;
    if (!g_p.Find()) return;
  }
  if (!g_p.Usable()) {
    static bool warned = false;
    if (!warned) REXLOG_WARN("adaptive triggers: player properties not found - triggers left alone");
    warned = true;
    return;
  }
  if (test) TestStateLine(now, in_front_end);
  if (!step && !impulse) return;
  rex::input::InputSystem* input = Input();
  if (!input) return;

  // what each pad's player is doing now (players not in the game: off)
  std::array<Seen, 4> seen{};
  if (const uint32_t engine = obj::Read32(gow3::addr::kGEngine)) {
    const uint32_t data = obj::Read32(engine + uint32_t(g_p.gamePlayers));   // TArray: data, count, max
    const uint32_t count = std::min<uint32_t>(obj::Read32(engine + uint32_t(g_p.gamePlayers) + 4), 4);
    for (uint32_t i = 0; data && i < count; i++) {
      const uint32_t lp = obj::Read32(data + 4 * i);
      if (!lp || !obj::IsA(lp, g_p.localPlayer)) continue;
      const uint32_t user = obj::Read32(lp + uint32_t(g_p.controllerId));
      if (user < seen.size()) seen[user] = Look(lp, in_front_end);
    }
  }
  if (impulse)
    for (uint32_t user = 0; user < seen.size(); user++) ImpulseTick(input, user, seen[user], now);
  if (!adaptive || !step) return;

  static std::array<int, 4> sent = {kNone, kNone, kNone, kNone};
  static std::array<Clock::time_point, 4> sent_at{};
  for (uint32_t user = 0; user < seen.size(); user++) {
    const int want = seen[user].feel;
    if (sent[user] == kNone && want == kOff) continue;   // never touched: leave the pad alone
    const bool changed = want != sent[user];
    if (!changed && now - sent_at[user] < std::chrono::seconds(1)) continue;
    const Feel& f = FeelFor(want);
    input->SetTriggerEffect(user, f.right.data(), f.left.data());
    if (changed) REXLOG_INFO("adaptive triggers: player pad {} - {}", user + 1, f.what);
    sent[user] = want;
    sent_at[user] = now;
  }
}
