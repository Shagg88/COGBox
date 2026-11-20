// Layer 3 (Gears of War 1, TU5): GitHub #245 - DualSense adaptive triggers per weapon (--pad_adaptive_triggers, off by
// default; the launcher's Controls tab "DualSense adaptive triggers"). Gears2-Native's layer3_gow2/adaptive_triggers.cpp
// with Gears 1's classes (WarfareGame: WarPawn, Weap_*), its globals, and this game's own object reflection
// (layer2_ue3/objects.cpp, the layout given - the one developer mode's scripted controller uses). Idea and the first
// working version: HubertFer's separate plugin (#245). Our code (MIT).
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
// FindClass) runs once on a worker thread when either option is on; the game thread starts only after that
// (g_objects_warm), so it never stalls a frame. With both options off: one flag test per tick, no thread.
//
// DS5 trigger effect bytes (11 per trigger; byte 0 = mode). Positions and strengths 0-255 (0 = trigger at rest).
//   0x05 off                                         {0x05}
//   0x01 continuous resistance from a position       {0x01, start, force}                  [V] SDL 3.4 testcontroller.c
//   0x02 section resistance (a "click" to pass)      {0x02, start, end, force}             [E] Nielk1's DS5 notes
//   0x06 vibration beyond a position                 {0x06, frequency Hz, amplitude, start} [V] SDL test {0x06,15,63,128}
#include <rex/cvar.h>
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

// 0.6.3 (GitHub #245): Xbox One / Series impulse triggers (--pad_trigger_rumble) - a pulse on the trigger motors:
// strengths in % of the motor (right = the fire trigger, left = the aim trigger), length in ms. A kick is one pulse per
// shot; hold = a buzz for as long as the weapon fires (flamethrower, beam, miniguns - continuous actions only).
struct Kick { uint8_t right, left; uint16_t ms; bool hold; };
constexpr Kick K(uint8_t right, uint8_t left, uint16_t ms) { return {right, left, ms, false}; }
constexpr Kick Hold(uint8_t right, uint8_t left) { return {right, left, 100, true}; }
constexpr Kick NoKick() { return {0, 0, 0, false}; }

struct Feel {
  const char* cls;   // a weapon class or one of its base classes (Gears 1's WarfareGame script)
  const char* what;  // for the log
  Effect right, left;  // DualSense
  Kick kick;           // Xbox impulse triggers
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
    {"Weap_AssaultRifle", "Lancer", Vib(12, 80, 40), Res(32, 60), K(30, 0, 45)},
    {"Weap_LocustAssaultRifle", "Hammerburst", Click(64, 128, 160), Res(32, 60), K(40, 0, 55)},
    {"Weap_Shotgun", "Gnasher", Click(80, 160, 224), Res(32, 60), K(85, 25, 110)},
    {"Weap_SniperRifle", "Longshot", Click(112, 160, 192), Res(16, 128), K(75, 15, 100)},
    {"Weap_Boomer", "Boomshot", Click(80, 176, 255), Res(32, 96), K(100, 35, 150)},
    {"Weap_Bow", "Torque Bow", Res(16, 160), Res(32, 80), K(70, 15, 100)},
    {"Weap_HammerOfDawn", "Hammer of Dawn", Res(32, 96), Res(32, 60), Hold(20, 0)},
    {"Weap_LocustPistol", "Boltok", Click(96, 160, 224), Res(32, 60), K(70, 10, 90)},
    {"Weap_COGPistol", "Snub pistol", Click(48, 96, 128), Res(32, 48), K(40, 0, 50)},
    {"Weap_PistolBase", "pistol", Click(48, 96, 128), Res(32, 48), K(40, 0, 50)},
    {"Weap_Grenade", "grenade", Res(64, 80), Res(32, 48), K(20, 0, 40)},
    {"Weap_MiniGun", "Troika", Vib(18, 160, 32), Res(0, 128), Hold(50, 25)},
    {"Weap_WretchMelee", "melee", Off(), Off(), NoKick()},
    {"WarWeapon", "weapon", Res(48, 64), Res(32, 48), K(30, 0, 45)},
};
constexpr int kNone = -1;      // nothing sent to this player yet
constexpr int kOff = -2;       // no weapon / not playing
constexpr int kChainsaw = -3;  // the Lancer's chainsaw
const Feel kChainsawFeel = {"", "chainsaw", Vib(30, 255, 0), Vib(30, 255, 0), {35, 35, 100, true}};   // (while it revs)
const Feel kOffFeel = {"", "off", Off(), Off(), NoKick()};

// UGameEngine::GamePlayers in Gears 1's engine, from the code (cheats.cpp: `lwz r11,0x29C(r3)` after Cast<UGameEngine>).
// Reflection must find the same, or the given layout is wrong (then the triggers are left alone).
constexpr int kGamePlayersFromCode = 0x29C;

// --pad_trigger_rumble (0.6.3), set by the tick before the first table search (and before the worker thread starts)
bool g_impulseOn = false;

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
    if (g_impulseOn) {   // (each found once, here on the worker thread; not waited for)
      ammoClass = obj::FindClass("WarWeapon");
      priClass = obj::FindClass("PlayerReplicationInfo");
      ffmClass = obj::FindClass("ForceFeedbackManager");
      FindImpulse();
    }
    if (gamePlayers != kGamePlayersFromCode) {   // the layout is given, not found: a mismatch means it is wrong
      REXLOG_WARN("adaptive triggers: GamePlayers at +{} by reflection, +{} in the code - the object layout is wrong",
                  gamePlayers, kGamePlayersFromCode);
      gamePlayers = -1;   // (Usable() fails: triggers left alone)
    }
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

// What one local player is doing now: the feel (kOff when not playing), the pawn and weapon, and whether the player
// allows vibration in the game's options. (The 0.6.3 impulse triggers read the pawn and weapon too.)
struct Seen {
  int feel = kOff;
  uint32_t pawn = 0, weapon = 0;
  bool vibration = true;
};

// The game's own Vibration option [G]: Gears 1's script was not read for it, so both of UE3's flags are read and either
// one saying off is off (review r70): ForceFeedbackManager.bAllowsForceFeedback (what Gears 3 and Judgment set from the
// profile) and PlayerReplicationInfo.bControllerVibrationAllowed (Gears 2's). Neither found: allowed (COGBox's own
// Vibration setting still applies).
constexpr bool kVibrationFromManager = true, kVibrationFromPri = true;
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
Seen Look(uint32_t localPlayer) {
  Seen s;
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
  if (g_p.lancer && obj::IsA(weapon, g_p.lancer) &&
      ((g_p.warPawn && obj::IsA(pawn, g_p.warPawn) && g_p.Flag(pawn, g_p.sawing, g_p.sawingMask)) ||
       g_p.Flag(weapon, g_p.sawRunning, g_p.sawRunningMask)))
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

// Game thread, every engine tick (cheats.cpp, the FEngineLoop::Tick override).
// The DualSense feel every 125 ms; with --pad_trigger_rumble the players are read every tick (the impulse triggers'
// shots).
// --cogbox_test_state (tests only, log only): once a second, what the first local player is doing, from the engine's own
// flags - so an unattended test can prove it measured gameplay and not the pause menu, a cutscene or a film
// (Shagg, 20 Nov: jobs kept measuring the pause menu). A test must find "cogbox_test_state: GAMEPLAY" in the log.
REXCVAR_DEFINE_BOOL(cogbox_test_state, false, "COGBox",
                    "Tests: log once a second whether the first player is in gameplay (not paused, not in a cutscene, "
                    "has a living pawn) - log only");
static void TestStateLine(Clock::time_point now) {
  static Clock::time_point next{};
  if (now < next) return;
  next = now + std::chrono::seconds(1);
  static const int timeSeconds = obj::PropertyOffset(g_p.worldInfo, "TimeSeconds");
  uint32_t players = 0, pc = 0, pawn = 0, wi = 0;
  if (const uint32_t engine = obj::Read32(kGEngine)) {
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
  const bool gameplay = pawn && health > 0 && !cutscene && pauseKnown && !paused;
  REXLOG_INFO("cogbox_test_state: {} | players {} | controller {} | pawn {} | health {} | cutscene {} | paused {} | "
              "level time {:.1f} s",
              gameplay ? "GAMEPLAY" : "not gameplay", players, pc ? "yes" : "no", pawn ? "yes" : "no", health,
              cutscene ? "yes" : "no", !pauseKnown ? "?" : paused ? "yes" : "no", t);
}

void Gow1AdaptiveTriggersTick() {
  static int adaptive = -1;   // --pad_adaptive_triggers / --pad_trigger_rumble are read once (a launcher restart)
  static bool impulse = false;
  static bool test = false;   // --cogbox_test_state
  if (adaptive < 0) {
    test = REXCVAR_GET(cogbox_test_state);
    adaptive = rex::input::InputSystem::AdaptiveTriggersEnabled() ? 1 : 0;
    impulse = g_impulseOn = rex::input::InputSystem::TriggerRumbleEnabled();
    if (adaptive)
      REXLOG_INFO("adaptive triggers: Gears 1 weapon feel for DualSense pads on (GitHub #245), {}",
                  kVersion);
    if (impulse)
      REXLOG_INFO("impulse triggers: Gears 1 trigger pulses for Xbox pads on (GitHub #245), {}",
                  kVersion);
    if (adaptive || impulse || test) StartObjectsInit();
  }
  if ((!adaptive && !impulse && !test) || !g_objects_warm.load(std::memory_order_acquire)) return;
  const Clock::time_point now = Clock::now();
  static Clock::time_point next{};
  const bool step = now >= next;   // the DualSense feel's 125 ms step
  if (step) next = now + std::chrono::milliseconds(125);
  if (!g_p.Usable()) {
    static bool warned = false;
    if (!warned) REXLOG_WARN("adaptive triggers: player properties not found - triggers left alone");
    warned = true;
    return;
  }
  if (test) TestStateLine(now);
  if (!step && !impulse) return;
  rex::input::InputSystem* input = Input();
  if (!input) return;

  // what each pad's player is doing now (players not in the game: off)
  std::array<Seen, 4> seen{};
  if (const uint32_t engine = obj::Read32(kGEngine)) {
    const uint32_t data = obj::Read32(engine + uint32_t(g_p.gamePlayers));   // TArray: data, count, max
    const uint32_t count = std::min<uint32_t>(obj::Read32(engine + uint32_t(g_p.gamePlayers) + 4), 4);
    for (uint32_t i = 0; data && i < count; i++) {
      const uint32_t lp = obj::Read32(data + 4 * i);
      if (!lp || !obj::IsA(lp, g_p.localPlayer)) continue;
      const uint32_t user = obj::Read32(lp + uint32_t(g_p.controllerId));
      if (user < seen.size()) seen[user] = Look(lp);
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
