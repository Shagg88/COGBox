// Layer 2 (Unreal Engine 3): GitHub #342 - the cheats the player picks in the launcher, simply on. No keys: god mode, all
// weapons and ammo, game speed and a hidden HUD are applied whenever this PC's game has a player with a body, and again
// after every level start, checkpoint and respawn (a new level makes a new player controller, a respawn a new pawn).
// Never in a Versus match: every step first asks ue3/match_guard.h, as the console's cheat commands do.
//
// What is set, and how (each read from the engine's own objects by name - ue3/objects.h - so a property this game does not
// have is left out and logged once):
//  - god mode:  Controller.bGodMode, the flag the engine's own "God" command sets (Pawn.InGodMode reads it). Written
//    directly, so it needs no cheat manager - it works where the game's AllowCheats says no (Gears 3 with Title Update 6,
//    whose script package we leave unpatched) - and it is only ever turned on, never toggled.
//  - hide HUD:  the player controller's HUD.bShowHUD, the flag "ToggleHUD" flips. Written directly and kept off (a cutscene
//    shows the HUD again when it ends - PlayerController.SetCinematicMode). With the cheat keys on (F11 = ToggleHUD) it is
//    hidden only for a HUD not hidden yet and when a cutscene ends, so F11 can still show it in between.
//  - speed:     WorldInfo.TimeDilation (and GameInfo.GameSpeed), as "Slomo" sets them, whenever the game runs at its normal
//    speed (1.0) - the game's own slow-motion moments are left alone and the choice comes back after them.
//  - weapons:   the engine's "AllWeapons" and "AllAmmo" commands (they give items, which only the game's script can do), once
//    per new infantry pawn (not a turret or vehicle the player sits in), a second and a half after it appears; then
//    InventoryManager.bInfiniteAmmo where the game has it. These need the player's cheat manager: "EnableCheats" makes it
//    when GameInfo.AllowCheats agrees.
// Gears 2 (#342): its AllowCheats is the engine's `return WorldInfo.NetMode == NM_Standalone`, and a campaign started from
// the menus runs as a listen server (drop-in co-op), so EnableCheats is refused even alone. With `listen_server_enable`, a
// listen server whose only players are this PC's own has NetMode shown as NM_Standalone for that one EnableCheats call and
// put back at once - the same answer the engine gives a game with no network. Never with a remote player in the game.
// Only in a game this PC runs (WorldInfo.Game set): a client in someone else's game changes nothing. With
// `remote_players_ok` false (Gears 1, Gears 2) only while every player in the game is on this PC: when another PC's
// player joins (drop-in co-op), what was set is undone - normal speed, god mode off, and the cheat managers this code
// made (Gears 2's) taken away again - as if the game had been a listen server from the start.
//
// Header only (included by each game's cheats.cpp - Gears 3: extras.cpp - next to ue3/match_guard.h; the four copies are
// kept identical). The options are console.cpp's --ue3_cheat_god / _weapons / _speed / _hide_hud. Our code (MIT).
#pragma once

#include <ue3/console.h>
#include <ue3/match_guard.h>
#include <ue3/objects.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

REXCVAR_DECLARE(bool, ue3_cheat_god);
REXCVAR_DECLARE(bool, ue3_cheat_weapons);
REXCVAR_DECLARE(double, ue3_cheat_speed);
REXCVAR_DECLARE(bool, ue3_cheat_hide_hud);

namespace ue3::cheats {

struct Spec {
  const char* game;               // for the log
  bool remote_players_ok;         // Gears 3, Judgment: also in a System Link / online game this PC hosts (their AllowCheats)
  bool listen_server_enable;      // Gears 2: EnableCheats on a listen server with only local players (see above)
  bool cheat_manager_possible;    // false: the game can't make one (Gears 3 TU6 unpatched) - weapons are left out, said once
  uint32_t engine_global;         // as the console target's
  uint32_t players_data_offset;
  uint32_t players_num_offset;
  const char* infantry_class;     // the game's on-foot pawn class (GearPawn; Gears 1 WarPawn): weapons only for these
};

namespace detail {
constexpr int kMaxPlayers = 4;
constexpr uint8_t kNmStandalone = 0, kNmListenServer = 2;

struct PlayerState {
  uint32_t god_pc = 0;                          // the controller this code set god mode on
  uint32_t god_said = 0, hud_said = 0;           // job 1962: the controller / HUD the log line was written for
  uint32_t made_mgr_pc = 0, made_mgr_world = 0;  // the controller whose cheat manager this code made (Gears 2 flip), its world
  bool was_cinematic = false;                    // PlayerController.bCinematicMode at the last check
  uint32_t pawn_done = 0, pawn_seen = 0;        // weapons given to / first seen
  std::chrono::steady_clock::time_point seen_at{};   // when pawn_seen appeared
};
struct State {
  Spec spec{};
  std::atomic<bool> ready{false}, failed{false}, started{false};
  bool keys = false;                            // the game's cheat keys are on (F11 = ToggleHUD): see "hide HUD"
  // offsets (-1: this game has no such property) and bool masks
  int off_actor = -1, off_world = -1, off_pawn = -1, off_god = -1, off_hud = -1, off_cheat = -1, off_show = -1;
  int off_game = -1, off_td = -1, off_netmode = -1, off_numplayers = -1, off_travelling = -1, off_speed = -1, off_inv = -1;
  int off_inf = -1, off_cine = -1, off_players_only = -1, off_po_pending = -1;
  uint32_t mask_god = 0, mask_show = 0, mask_inf = 0, mask_cine = 0, mask_players_only = 0, mask_po_pending = 0, infantry = 0,
           pc_class = 0, debug_cam = 0;
  const char* unavailable = nullptr;            // GitHub #359: why the game knows no cheat command (Gears 3 TU6), or none
  // game thread only
  PlayerState player[kMaxPlayers];
  uint32_t speed_world = 0, remote_world = 0, nomgr_logged = 0;
  float speed_set = 1.0f;                       // the TimeDilation this code wrote into speed_world (1 = none)
  uint64_t ticks = 0;
  bool said_missing = false;
};
inline State& S() {
  static State s;
  return s;
}

inline float Speed() {   // the chosen game speed, 0.1-1 (1 = normal: nothing is changed)
  const double v = REXCVAR_GET(ue3_cheat_speed);
  if (!(v > 0.0) || v >= 0.999) return 1.0f;
  return float(v < 0.1 ? 0.1 : v);
}
inline bool AnyOn() {
  return REXCVAR_GET(ue3_cheat_god) || REXCVAR_GET(ue3_cheat_weapons) || REXCVAR_GET(ue3_cheat_hide_hud) ||
         Speed() != 1.0f;
}
inline float ReadF(uint32_t a) {
  const uint32_t w = obj::Read32(a);
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}
inline void WriteF(uint32_t a, float f) {
  uint32_t w;
  std::memcpy(&w, &f, 4);
  obj::Write32(a, w);
}
inline uint8_t Read8(uint32_t a) {   // a byte property (big-endian guest: byte 0 is the word's top byte)
  const uint32_t w = obj::Read32(a & ~3u);
  return uint8_t(w >> (24 - 8 * (a & 3u)));
}
inline bool BitOn(uint32_t object, int off, uint32_t mask) { return (obj::Read32(object + uint32_t(off)) & mask) != 0; }
inline void SetBit(uint32_t object, int off, uint32_t mask, bool on) {
  const uint32_t w = obj::Read32(object + uint32_t(off));
  obj::Write32(object + uint32_t(off), on ? (w | mask) : (w & ~mask));
}

// Worker thread: every class and property used. Retried while the engine starts. The object tables are the match
// guard's (its open_objects, asked here too so the lookup can start before the first check).
inline void Resolve() {
  State& s = S();
  for (int attempt = 0; attempt < 60; attempt++) {
    if (attempt) std::this_thread::sleep_for(std::chrono::seconds(attempt < 15 ? 2 : 15));
    if (!obj::Ready()) {
      const auto open = ue3::match::detail::State().spec.open_objects;
      if (!open || !open()) continue;
    }
    const uint32_t local_player = obj::FindClass("LocalPlayer"), actor = obj::FindClass("Actor"),
                   controller = obj::FindClass("Controller"), pcls = obj::FindClass("PlayerController"),
                   hud = obj::FindClass("HUD"), world = obj::FindClass("WorldInfo"), game = obj::FindClass("GameInfo"),
                   pawn = obj::FindClass("Pawn"), inv = obj::FindClass("InventoryManager");
    if (!local_player || !actor || !controller || !pcls || !world || !game || !pawn) continue;
    // the game's script classes (the infantry pawn) load with its package: wait for them during the first 30 s of tries
    const uint32_t infantry = s.spec.infantry_class ? obj::FindClass(s.spec.infantry_class) : 0;
    if (!infantry && attempt < 15) continue;
    s.off_actor = obj::PropertyOffset(local_player, "Actor");
    s.off_world = obj::PropertyOffset(actor, "WorldInfo");
    s.off_pawn = obj::PropertyOffset(controller, "Pawn");
    s.off_god = obj::PropertyOffset(controller, "bGodMode");
    s.mask_god = obj::BoolMask(controller, "bGodMode");
    s.off_hud = obj::PropertyOffset(pcls, "myHUD");
    s.off_cheat = obj::PropertyOffset(pcls, "CheatManager");
    s.off_cine = obj::PropertyOffset(pcls, "bCinematicMode"), s.mask_cine = obj::BoolMask(pcls, "bCinematicMode");
    s.pc_class = pcls;
    if (hud) s.off_show = obj::PropertyOffset(hud, "bShowHUD"), s.mask_show = obj::BoolMask(hud, "bShowHUD");
    s.off_game = obj::PropertyOffset(world, "Game");
    s.off_td = obj::PropertyOffset(world, "TimeDilation");
    s.off_netmode = obj::PropertyOffset(world, "NetMode");
    s.off_players_only = obj::PropertyOffset(world, "bPlayersOnly"), s.mask_players_only = obj::BoolMask(world, "bPlayersOnly");
    // (later engines: the command sets bPlayersOnlyPending and the world's tick copies it)
    s.off_po_pending = obj::PropertyOffset(world, "bPlayersOnlyPending"), s.mask_po_pending = obj::BoolMask(world, "bPlayersOnlyPending");
    s.off_numplayers = obj::PropertyOffset(game, "NumPlayers");
    s.off_travelling = obj::PropertyOffset(game, "NumTravellingPlayers");
    s.off_speed = obj::PropertyOffset(game, "GameSpeed");
    s.off_inv = obj::PropertyOffset(pawn, "InvManager");
    if (inv) s.off_inf = obj::PropertyOffset(inv, "bInfiniteAmmo"), s.mask_inf = obj::BoolMask(inv, "bInfiniteAmmo");
    s.infantry = infantry;
    s.debug_cam = obj::FindClass("DebugCameraController");   // GitHub #359: F9's free camera on (0: not in this game)
    if (s.off_actor < 0 || s.off_world < 0 || s.off_pawn < 0 || s.off_game < 0) continue;
    if (!s.mask_god) s.off_god = -1;
    if (!s.mask_show) s.off_show = -1;
    if (!s.mask_inf) s.off_inf = -1;
    if (!s.mask_cine) s.off_cine = -1;
    if (!s.mask_players_only) s.off_players_only = -1;
    if (!s.mask_po_pending) s.off_po_pending = -1;
    REXLOG_INFO("auto cheats ({}): ready (Controller.bGodMode +{} mask {:X}, PlayerController.myHUD +{} .CheatManager +{}, "
                "HUD.bShowHUD +{}, WorldInfo.TimeDilation +{} .NetMode +{}, GameInfo.NumPlayers +{} .NumTravellingPlayers +{} "
                ".GameSpeed +{}, Pawn.InvManager +{}, InventoryManager.bInfiniteAmmo +{}, infantry {} {})",
                s.spec.game, s.off_god, s.mask_god, s.off_hud, s.off_cheat, s.off_show, s.off_td, s.off_netmode,
                s.off_numplayers, s.off_travelling, s.off_speed, s.off_inv, s.off_inf,
                s.spec.infantry_class ? s.spec.infantry_class : "-", infantry ? "found" : "NOT found (any pawn)");
    s.ready.store(true, std::memory_order_release);
    return;
  }
  REXLOG_WARN("auto cheats ({}): the engine's classes were not found - the launcher's cheats stay off", S().spec.game);
  S().failed = true;
}
inline void StartResolve() {
  if (!S().started.exchange(true)) std::thread(Resolve).detach();
}

// The game's player list: how many local players, and player i's controller (0 when none).
inline int LocalCount() {
  const State& s = S();
  const uint32_t engine = obj::Read32(s.spec.engine_global);
  if (!engine) return 0;
  const int32_t n = int32_t(obj::Read32(engine + s.spec.players_num_offset));
  return n <= 0 ? 0 : n > kMaxPlayers ? kMaxPlayers : n;
}
inline uint32_t ControllerOf(int i) {
  const State& s = S();
  const uint32_t engine = obj::Read32(s.spec.engine_global);
  const uint32_t data = engine ? obj::Read32(engine + s.spec.players_data_offset) : 0;
  const uint32_t player = data ? obj::Read32(data + 4u * uint32_t(i)) : 0;
  return player ? obj::Read32(player + uint32_t(s.off_actor)) : 0;
}
// Every player in the game this PC runs is on this PC (or the game cannot say: then no). Players still travelling to
// the level (seamless travel) count too.
inline bool OnlyLocalPlayers(uint32_t game, int local) {
  const State& s = S();
  if (s.off_numplayers < 0) return false;
  int32_t n = int32_t(obj::Read32(game + uint32_t(s.off_numplayers)));
  if (s.off_travelling >= 0) n += int32_t(obj::Read32(game + uint32_t(s.off_travelling)));
  return n >= 0 && n <= local;
}

// The player's cheat manager, made with EnableCheats if it is missing (Gears 2: see the top). true when it exists now.
inline bool EnsureCheatManager(PPCContext& ctx, uint8_t* base, int i, uint32_t pc, uint32_t world, uint32_t game, int local) {
  State& s = S();
  if (s.off_cheat < 0) return true;   // can't tell: let the commands say
  if (obj::Read32(pc + uint32_t(s.off_cheat))) return true;
  bool handled = false;
  const uint32_t nm_at = s.off_netmode >= 0 ? world + uint32_t(s.off_netmode) : 0;
  const uint8_t nm = nm_at ? Read8(nm_at) : kNmStandalone;
  const bool flip = s.spec.listen_server_enable && nm_at && nm == kNmListenServer && game && OnlyLocalPlayers(game, local);
  if (flip) obj::Write8(nm_at, kNmStandalone);
  const bool ran = Ue3ConsoleExecNow(ctx, base, i, "EnableCheats", &handled);
  if (flip) obj::Write8(nm_at, nm);
  const bool made = obj::Read32(pc + uint32_t(s.off_cheat)) != 0;
  if (made && flip && i < kMaxPlayers) s.player[i].made_mgr_pc = pc, s.player[i].made_mgr_world = world;   // undone on a remote join
  if (ran && (made || s.nomgr_logged != pc)) {
    REXLOG_INFO("auto cheats ({}): EnableCheats for player {} - cheat manager {} (NetMode {}{})", s.spec.game, i + 1,
                made ? "made" : "NOT made (the game's AllowCheats said no)", unsigned(nm),
                flip ? ": a listen server with only this PC's players, seen as standalone for the call" : "");
    if (!made) s.nomgr_logged = pc;
  }
  return made;
}

// Another PC's player is in a game where the cheats are for this PC's games only: undo what this code set.
inline void UndoForRemote(uint32_t world, uint32_t game, int local) {
  State& s = S();
  bool any = false;
  for (int i = 0; i < local; i++) {
    PlayerState& p = s.player[i];
    const uint32_t pc = ControllerOf(i);
    if (pc && p.god_pc == pc && s.off_god >= 0 && BitOn(pc, s.off_god, s.mask_god)) SetBit(pc, s.off_god, s.mask_god, false), any = true;
    p.god_pc = 0;
    p.god_said = 0;   // review r1962: "god mode on" is printed again when the cheats come back
    // the controller the manager was made for - not ControllerOf(i), which is the debug camera's while F9's free camera is
    // on; only while still in that world (a level change frees the old controller) and still a player controller
    const uint32_t m = p.made_mgr_pc;
    if (m && p.made_mgr_world == world && s.off_cheat >= 0 && (!s.pc_class || obj::IsA(m, s.pc_class)) &&
        obj::Read32(m + uint32_t(s.off_cheat))) {
      obj::Write32(m + uint32_t(s.off_cheat), 0);   // the engine's garbage collector frees the object
      any = true;
    }
    p.made_mgr_pc = p.made_mgr_world = 0;
    p.pawn_done = p.pawn_seen = 0;
  }
  if (s.speed_set != 1.0f && s.speed_world == world && s.off_td >= 0 && ReadF(world + uint32_t(s.off_td)) == s.speed_set) {
    WriteF(world + uint32_t(s.off_td), 1.0f);
    if (s.off_speed >= 0) WriteF(game + uint32_t(s.off_speed), 1.0f);
    any = true;
  }
  s.speed_set = 1.0f;
  // F10's freeze (PlayersOnly) is not for a game with another PC's player in it either
  if (s.off_players_only >= 0 && BitOn(world, s.off_players_only, s.mask_players_only))
    SetBit(world, s.off_players_only, s.mask_players_only, false), any = true;
  if (s.off_po_pending >= 0 && BitOn(world, s.off_po_pending, s.mask_po_pending))
    SetBit(world, s.off_po_pending, s.mask_po_pending, false), any = true;
  if (s.remote_world != world)
    REXLOG_INFO("auto cheats ({}): another PC's player is in this game - the launcher's cheats are off here{}", s.spec.game,
                any ? " (god mode, speed, the world freeze and the cheat manager put back)" : "");
  s.remote_world = world;
}
}  // namespace detail

inline void Configure(const Spec& spec) { detail::S().spec = spec; }
// The game's cheat keys are on (F11 = ToggleHUD): Hide the HUD then leaves F11 a say (see "hide HUD" at the top).
inline void SetKeysOn(bool on) { detail::S().keys = on; }
// Gears 2 with the cheat keys: start the lookup now, so the keys' EnableCheats can use it (no launcher cheat on: Tick
// would never start it).
inline void Prepare() { detail::StartResolve(); }

// GitHub #359: the cheat key notice's text when a command the game knows nothing of is "not recognised" for a known
// reason (Gears 3 with Title Update 6: no cheat manager). A string literal; nullptr = the plain wording.
inline void SetUnavailableReason(const char* reason) { detail::S().unavailable = reason; }

// GitHub #359: what a cheat key did, for its on-screen notice (xenos/cheat_popup.h) and its log line. Game thread, right
// after THAT key's command ran (outcome 0 = done, 1 = not recognised by the engine, 2 = refused by the gate). Each cheat
// has its own entry below; ON / OFF is the engine's own flag read back after the command (the flags the launcher's
// cheats write), never a guess or a remembered toggle - so it is right whatever the state was before (the game's own god
// mode in Gears 1's opening ride, a launcher cheat, the key pressed twice). When the flag can't be read the chip says
// DONE and `read` says why. English; the popup translates.
//   God               Controller.bGodMode of GamePlayers(0)'s controller           ON / OFF
//   ToggleDebugCamera GamePlayers(0).Actor is a DebugCameraController               ON / OFF
//   Ghost / Fly / Walk  no flag: the command sets the mode (Gears 1's F9 cycles Ghost|Walk) ON / OFF
//   PlayersOnly       WorldInfo.bPlayersOnlyPending (later engines) or .bPlayersOnly  ON (frozen) / OFF
//   ToggleHUD         the controller's myHUD.bShowHUD                               ON (shown) / OFF (hidden)
//   Slomo <x>         WorldInfo.TimeDilation as it is now                            <x>x / OFF (1)
//   AllWeapons / AllAmmo  the engine's answer only (items are given by script)      GIVEN
// tone: 0 on, 1 off, 2 a value / given / done, 3 not done (no chip: the popup shows the label, or "label: state").
// icon: xenos/cheat_popup.h XenosCheatIcon's order (0 generic, 1 shield, 2 stopwatch, 3 weapon, 4 eye, 5 camera,
// 6 freeze, 7 fly).
struct KeyNotice {
  std::string label, state, read;   // read: what was read back, for the log
  int tone = 2, icon = 0;
};
inline KeyNotice DescribeKey(const std::string& command, int outcome) {
  using namespace detail;
  State& s = S();
  std::string verb = command.substr(0, command.find(' ')), arg;
  if (verb.size() < command.size()) arg = command.substr(verb.size() + 1);
  for (char& c : verb) c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  KeyNotice n;
  n.label = command;   // a command with no entry of its own is shown as typed
  if (verb == "god") n.label = "God mode", n.icon = 1;
  else if (verb == "toggledebugcamera") n.label = "Free camera", n.icon = 5;
  else if (verb == "ghost" || verb == "fly" || verb == "walk") n.label = "Fly mode", n.icon = 7;
  else if (verb == "playersonly") n.label = "Freeze the world", n.icon = 6;
  else if (verb == "togglehud") n.label = "HUD", n.icon = 4;
  else if (verb == "slomo") n.label = "Slow motion", n.icon = 2;
  else if (verb == "allweapons") n.label = "All weapons", n.icon = 3;
  else if (verb == "allammo") n.label = "All ammo", n.icon = 3;
  if (outcome == 2) {
    n.tone = 3;
    const bool versus = ue3::match::Check() == 2;
    n.label = versus ? "Cheats are off in Versus" : "Cheats are off with another PC's player in the game";
    n.read = versus ? "refused: Versus or lobby" : "refused: not only this PC's players";
    return n;
  }
  if (outcome == 1) {
    n.tone = 3;
    n.state = s.unavailable ? s.unavailable : "not recognised by the game";   // shown as "<cheat>: <state>"
    n.read = "not recognised by the engine";
    return n;
  }
  n.state = "DONE";
  n.read = "nothing read";
  auto on_off = [&](bool on, const char* flag) {
    n.tone = on ? 0 : 1;
    n.state = on ? "ON" : "OFF";
    n.read = std::string(flag) + (on ? " = 1" : " = 0");
  };
  if (!s.ready.load(std::memory_order_acquire)) {
    n.read = "engine classes not looked up yet";
    if (verb == "allweapons" || verb == "allammo") n.state = "GIVEN", n.read = "engine said done";
    return n;
  }
  const uint32_t pc = LocalCount() ? ControllerOf(0) : 0;   // the controller the command ran on
  const uint32_t world = pc ? obj::Read32(pc + uint32_t(s.off_world)) : 0;
  if (verb == "god") {
    if (pc && s.off_god >= 0) {
      on_off(BitOn(pc, s.off_god, s.mask_god), "Controller.bGodMode");
      // the launcher's God mode turns it back on within a moment (Tick above): say so rather than OFF
      if (n.tone == 1 && REXCVAR_GET(ue3_cheat_god)) n.tone = 0, n.state = "ON", n.read += " (the launcher's God mode puts it back)";
    } else {
      n.read = "no Controller.bGodMode";
    }
  } else if (verb == "toggledebugcamera") {
    if (pc && s.debug_cam) on_off(obj::IsA(pc, s.debug_cam), "player is DebugCameraController");
    else n.read = "no DebugCameraController class";
  } else if (verb == "ghost" || verb == "fly" || verb == "walk") {
    n.tone = verb == "walk" ? 1 : 0;
    n.state = verb == "walk" ? "OFF" : "ON";
    n.read = "set by the command (" + command + ")";
  } else if (verb == "playersonly") {
    if (world && s.off_po_pending >= 0) on_off(BitOn(world, s.off_po_pending, s.mask_po_pending), "WorldInfo.bPlayersOnlyPending");
    else if (world && s.off_players_only >= 0) on_off(BitOn(world, s.off_players_only, s.mask_players_only), "WorldInfo.bPlayersOnly");
    else n.read = "no WorldInfo.bPlayersOnly";
  } else if (verb == "togglehud") {
    const uint32_t hud = pc && s.off_hud >= 0 && s.off_show >= 0 ? obj::Read32(pc + uint32_t(s.off_hud)) : 0;
    if (hud) on_off(BitOn(hud, s.off_show, s.mask_show), "HUD.bShowHUD");
    else n.read = "no HUD";
  } else if (verb == "slomo") {
    if (world && s.off_td >= 0) {
      const float td = ReadF(world + uint32_t(s.off_td));
      char text[48];
      std::snprintf(text, sizeof(text), "%gx", double(td));
      const bool normal = std::fabs(td - 1.0f) < 0.001f;
      n.tone = normal ? 1 : 2;
      n.state = normal ? "OFF" : text;
      std::snprintf(text, sizeof(text), "WorldInfo.TimeDilation = %g", double(td));
      n.read = text;
      // the launcher's Game speed comes back at normal speed (Tick above)
      if (normal && Speed() != 1.0f) n.read += " (the launcher's Game speed puts it back)";
    } else {
      n.read = "no WorldInfo.TimeDilation";
    }
  } else if (verb == "allweapons" || verb == "allammo") {
    n.state = "GIVEN";
    n.read = "engine said done";
  }
  return n;
}

// The console target's gate for Gears 1 and 2 (their cheats are for this PC's games only): the match guard's answer, and
// also "refuse" while another PC's player is in the game this PC runs - F10's freeze (PlayersOnly, an engine command
// that needs no cheat manager) included. Display settings never come here (console.cpp lets them through).
inline int GateLocalOnly() {
  using namespace detail;
  const int verdict = ue3::match::Check();
  if (verdict != 0) return verdict;
  State& s = S();
  StartResolve();
  if (!s.ready.load(std::memory_order_acquire)) return s.failed.load(std::memory_order_relaxed) ? 2 : 1;
  const int local = LocalCount();
  const uint32_t pc = local ? ControllerOf(0) : 0;
  const uint32_t world = pc ? obj::Read32(pc + uint32_t(s.off_world)) : 0;
  const uint32_t game = world ? obj::Read32(world + uint32_t(s.off_game)) : 0;
  if (!world) return 1;
  if (!game) return 2;   // a client in someone else's game (the engine refuses cheats there too)
  if (OnlyLocalPlayers(game, local)) return 0;
  static uint32_t said_world = 0;   // game thread (the console's gate); once per level
  if (said_world != world)
    REXLOG_INFO("Cheat keys are off while another PC's player is in the game ({})", s.spec.game);
  said_world = world;
  return 2;
}

// The console target's enable_cheats (Gears 2's cheat keys): EnableCheats for player 1 as above. false = no player yet.
inline bool EnableForConsole(PPCContext& ctx, uint8_t* base) {
  using namespace detail;
  State& s = S();
  StartResolve();
  if (!s.ready.load(std::memory_order_acquire)) {   // before the lookup: the plain command, as before
    bool ignored = false;
    return Ue3ConsoleExecNow(ctx, base, 0, "EnableCheats", &ignored);
  }
  const int local = LocalCount();
  const uint32_t pc = local ? ControllerOf(0) : 0;
  if (!pc) return false;
  const uint32_t world = obj::Read32(pc + uint32_t(s.off_world));
  const uint32_t game = world ? obj::Read32(world + uint32_t(s.off_game)) : 0;
  if (!world) return false;
  EnsureCheatManager(ctx, base, 0, pc, world, game, local);
  return true;
}

// Game thread, once per engine frame (after Ue3ConsoleRunQueued). Works every 15th call.
inline void Tick(PPCContext& ctx, uint8_t* base) {
  using namespace detail;
  State& s = S();
  const bool keys_flip = s.started.load(std::memory_order_relaxed) && s.spec.listen_server_enable;   // Gears 2 keys
  if (!AnyOn() && !keys_flip) return;
  if (!Ue3ConsoleIsGameThread()) return;   // the engine loop's thread only (the console's)
  if (++s.ticks % 15) return;
  const int verdict = ue3::match::Check();   // also opens the object tables the first time
  StartResolve();
  if (s.failed.load(std::memory_order_relaxed) || !s.ready.load(std::memory_order_acquire)) return;
  if (verdict != 0) return;   // Versus (or the lobbies), or the game state is not readable yet
  const int local = LocalCount();
  if (!local) return;
  const uint32_t pc0 = ControllerOf(0);
  const uint32_t world = pc0 ? obj::Read32(pc0 + uint32_t(s.off_world)) : 0;
  const uint32_t game = world ? obj::Read32(world + uint32_t(s.off_game)) : 0;
  if (!game) return;   // a client in someone else's game: nothing applies there
  if (!s.spec.remote_players_ok && !OnlyLocalPlayers(game, local)) {
    UndoForRemote(world, game, local);
    return;
  }
  if (!AnyOn()) return;   // the Gears 2 keys only needed the check above
  if (!s.said_missing) {
    s.said_missing = true;
    if (REXCVAR_GET(ue3_cheat_god) && s.off_god < 0) REXLOG_WARN("auto cheats ({}): no Controller.bGodMode - god mode left out", s.spec.game);
    if (REXCVAR_GET(ue3_cheat_hide_hud) && (s.off_hud < 0 || s.off_show < 0))
      REXLOG_WARN("auto cheats ({}): no HUD.bShowHUD - hide the HUD left out", s.spec.game);
    if (Speed() != 1.0f && s.off_td < 0) REXLOG_WARN("auto cheats ({}): no WorldInfo.TimeDilation - game speed left out", s.spec.game);
    if (REXCVAR_GET(ue3_cheat_weapons) && !s.spec.cheat_manager_possible)
      REXLOG_WARN("auto cheats ({}): all weapons needs the game's cheat manager, which this game version doesn't allow - left out", s.spec.game);
  }
  bool any_pawn = false;
  for (int i = 0; i < local; i++) {
    PlayerState& p = s.player[i];
    const uint32_t pc = i ? ControllerOf(i) : pc0;
    if (!pc) continue;
    const uint32_t pawn = obj::Read32(pc + uint32_t(s.off_pawn));
    if (!pawn) { p.pawn_seen = p.pawn_done = 0; continue; }   // the front end, a dead player waiting to respawn, a load
    any_pawn = true;
    if (REXCVAR_GET(ue3_cheat_god) && s.off_god >= 0) {
      // job 1962: the line once per controller, also when the game had the flag on already (Gears 1's opening ride
      // printed nothing). god_pc stays "set by this code" only: UndoForRemote clears only what it set.
      const bool was_on = BitOn(pc, s.off_god, s.mask_god);
      if (!was_on) {
        SetBit(pc, s.off_god, s.mask_god, true);
        p.god_pc = pc;
      }
      if (p.god_said != pc) {
        p.god_said = pc;
        REXLOG_INFO("auto cheats ({}): god mode on (player {}{})", s.spec.game, i + 1, was_on ? ", the game had it on already" : "");
      }
    }
    if (REXCVAR_GET(ue3_cheat_hide_hud) && s.off_hud >= 0 && s.off_show >= 0) {
      const uint32_t hud = obj::Read32(pc + uint32_t(s.off_hud));
      const bool cine = s.off_cine >= 0 && BitOn(pc, s.off_cine, s.mask_cine);
      const bool cutscene_ended = p.was_cinematic && !cine;
      p.was_cinematic = cine;
      // kept hidden; with the cheat keys only for a HUD not hidden yet and after a cutscene (F11 can show it in between).
      // Job 1962 review: "not hidden yet" (hud_said), not "not seen yet" - a HUD first seen in a cutscene or while the game
      // itself had it off was recorded unhidden and, with the keys on, never hidden afterwards.
      if (hud && !cine && (!s.keys || hud != p.hud_said || cutscene_ended) && BitOn(hud, s.off_show, s.mask_show)) {
        SetBit(hud, s.off_show, s.mask_show, false);
        // job 1962: once per HUD when it is first hidden (a level that starts in a cutscene recorded the HUD before
        // hiding it, so the old hud != p.hud test never printed - Gears 3 / Judgment)
        if (p.hud_said != hud) {
          p.hud_said = hud;
          REXLOG_INFO("auto cheats ({}): HUD hidden (player {})", s.spec.game, i + 1);
        }
      }
    }
    // weapons: on-foot pawns only (a turret or vehicle the player sits in is a pawn too)
    const bool infantry = !s.infantry || obj::IsA(pawn, s.infantry);
    if (REXCVAR_GET(ue3_cheat_weapons) && s.spec.cheat_manager_possible && infantry && pawn != p.pawn_done) {
      // a second and a half after the body appears: the game gives its own starting weapons first
      const auto now = std::chrono::steady_clock::now();
      if (pawn != p.pawn_seen) { p.pawn_seen = pawn, p.seen_at = now; continue; }
      if (now - p.seen_at < std::chrono::milliseconds(1500)) continue;
      p.pawn_done = pawn;
      if (!EnsureCheatManager(ctx, base, i, pc, world, game, local)) continue;
      bool w = false, a = false;
      Ue3ConsoleExecNow(ctx, base, i, "AllWeapons", &w);
      Ue3ConsoleExecNow(ctx, base, i, "AllAmmo", &a);
      bool inf = false;
      const uint32_t inv = s.off_inv >= 0 ? obj::Read32(pawn + uint32_t(s.off_inv)) : 0;
      if (inv && s.off_inf >= 0) SetBit(inv, s.off_inf, s.mask_inf, true), inf = true;
      REXLOG_INFO("auto cheats ({}): player {}: AllWeapons {}, AllAmmo {}, infinite ammo {}", s.spec.game, i + 1,
                  w ? "done" : "not recognised", a ? "done" : "not recognised",
                  inf ? "on" : s.off_inf < 0 ? "(this game has no InventoryManager.bInfiniteAmmo)" : "(no inventory yet)");
    }
  }
  const float speed = Speed();
  if (speed != 1.0f && any_pawn && s.off_td >= 0) {
    const float td = ReadF(world + uint32_t(s.off_td));
    if (td == 1.0f) {
      WriteF(world + uint32_t(s.off_td), speed);
      if (s.off_speed >= 0) WriteF(game + uint32_t(s.off_speed), speed);
      if (s.speed_world != world) REXLOG_INFO("auto cheats ({}): game speed {:.2f}", s.spec.game, speed);
      s.speed_world = world;
      s.speed_set = speed;
    }
  }
}

}  // namespace ue3::cheats
