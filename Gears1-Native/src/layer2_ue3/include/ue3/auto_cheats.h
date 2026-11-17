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
//  - hide HUD:  the player controller's HUD.bShowHUD, the flag "ToggleHUD" flips. Written directly; kept off.
//  - speed:     WorldInfo.TimeDilation (and GameInfo.GameSpeed), as "Slomo" sets them, whenever the game runs at its normal
//    speed (1.0) - the game's own slow-motion moments are left alone and the choice comes back after them.
//  - weapons:   the engine's "AllWeapons" and "AllAmmo" commands (they give items, which only the game's script can do), once
//    per new pawn, a second after it appears; then InventoryManager.bInfiniteAmmo where the game has it. These need the
//    player's cheat manager: "EnableCheats" makes it when GameInfo.AllowCheats agrees.
// Gears 2 (#342): its AllowCheats is the engine's `return WorldInfo.NetMode == NM_Standalone`, and a campaign started from
// the menus runs as a listen server (drop-in co-op), so EnableCheats is refused even alone. With `listen_server_enable`, a
// listen server whose only players are this PC's own has NetMode shown as NM_Standalone for that one EnableCheats call and
// put back at once - the same answer the engine gives a game with no network. Never with a remote player in the game.
// Only in a game this PC runs (WorldInfo.Game set): a client in someone else's game changes nothing. With
// `remote_players_ok` false (Gears 1, Gears 2) only while every player in the game is on this PC.
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
};

namespace detail {
constexpr int kMaxPlayers = 4;
constexpr uint8_t kNmStandalone = 0, kNmListenServer = 2;

struct PlayerState {
  uint32_t pc = 0, god_pc = 0, hud = 0;         // the last controller / HUD logged for
  uint32_t pawn_done = 0, pawn_seen = 0;        // weapons given to / first seen
  std::chrono::steady_clock::time_point seen_at{};   // when pawn_seen appeared
};
struct State {
  Spec spec{};
  std::atomic<bool> ready{false}, failed{false};
  // offsets (-1: this game has no such property) and bool masks
  int off_actor = -1, off_world = -1, off_pawn = -1, off_god = -1, off_hud = -1, off_cheat = -1, off_show = -1;
  int off_game = -1, off_td = -1, off_netmode = -1, off_numplayers = -1, off_speed = -1, off_inv = -1, off_inf = -1;
  uint32_t mask_god = 0, mask_show = 0, mask_inf = 0;
  // game thread only
  PlayerState player[kMaxPlayers];
  uint32_t speed_world = 0, remote_world = 0, nomgr_logged = 0;
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

// Worker thread (match guard's object tables): every class and property used. Retried while the engine starts.
inline void Resolve() {
  State& s = S();
  for (int attempt = 0; attempt < 60; attempt++) {
    if (attempt) std::this_thread::sleep_for(std::chrono::seconds(attempt < 15 ? 2 : 15));
    if (!obj::Ready()) continue;   // the match guard (asked first by every check) opens the tables
    const uint32_t local_player = obj::FindClass("LocalPlayer"), actor = obj::FindClass("Actor"),
                   controller = obj::FindClass("Controller"), pcls = obj::FindClass("PlayerController"),
                   hud = obj::FindClass("HUD"), world = obj::FindClass("WorldInfo"), game = obj::FindClass("GameInfo"),
                   pawn = obj::FindClass("Pawn"), inv = obj::FindClass("InventoryManager");
    if (!local_player || !actor || !controller || !pcls || !world || !game || !pawn) continue;
    s.off_actor = obj::PropertyOffset(local_player, "Actor");
    s.off_world = obj::PropertyOffset(actor, "WorldInfo");
    s.off_pawn = obj::PropertyOffset(controller, "Pawn");
    s.off_god = obj::PropertyOffset(controller, "bGodMode");
    s.mask_god = obj::BoolMask(controller, "bGodMode");
    s.off_hud = obj::PropertyOffset(pcls, "myHUD");
    s.off_cheat = obj::PropertyOffset(pcls, "CheatManager");
    if (hud) s.off_show = obj::PropertyOffset(hud, "bShowHUD"), s.mask_show = obj::BoolMask(hud, "bShowHUD");
    s.off_game = obj::PropertyOffset(world, "Game");
    s.off_td = obj::PropertyOffset(world, "TimeDilation");
    s.off_netmode = obj::PropertyOffset(world, "NetMode");
    s.off_numplayers = obj::PropertyOffset(game, "NumPlayers");
    s.off_speed = obj::PropertyOffset(game, "GameSpeed");
    s.off_inv = obj::PropertyOffset(pawn, "InvManager");
    if (inv) s.off_inf = obj::PropertyOffset(inv, "bInfiniteAmmo"), s.mask_inf = obj::BoolMask(inv, "bInfiniteAmmo");
    if (s.off_actor < 0 || s.off_world < 0 || s.off_pawn < 0 || s.off_game < 0) continue;
    if (!s.mask_god) s.off_god = -1;
    if (!s.mask_show) s.off_show = -1;
    if (!s.mask_inf) s.off_inf = -1;
    REXLOG_INFO("auto cheats ({}): ready (Controller.bGodMode +{} mask {:X}, PlayerController.myHUD +{} .CheatManager +{}, "
                "HUD.bShowHUD +{}, WorldInfo.TimeDilation +{} .NetMode +{}, GameInfo.NumPlayers +{} .GameSpeed +{}, "
                "Pawn.InvManager +{}, InventoryManager.bInfiniteAmmo +{})",
                s.spec.game, s.off_god, s.mask_god, s.off_hud, s.off_cheat, s.off_show, s.off_td, s.off_netmode,
                s.off_numplayers, s.off_speed, s.off_inv, s.off_inf);
    s.ready.store(true, std::memory_order_release);
    return;
  }
  REXLOG_WARN("auto cheats ({}): the engine's classes were not found - the launcher's cheats stay off", S().spec.game);
  S().failed = true;
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
// Every player in the game this PC runs is on this PC (or the game cannot say: then no).
inline bool OnlyLocalPlayers(uint32_t game, int local) {
  const State& s = S();
  if (s.off_numplayers < 0) return false;
  const int32_t n = int32_t(obj::Read32(game + uint32_t(s.off_numplayers)));
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
  if (ran && (made || s.nomgr_logged != pc)) {
    REXLOG_INFO("auto cheats ({}): EnableCheats for player {} - cheat manager {} (NetMode {}{})", s.spec.game, i + 1,
                made ? "made" : "NOT made (the game's AllowCheats said no)", unsigned(nm),
                flip ? ": a listen server with only this PC's players, seen as standalone for the call" : "");
    if (!made) s.nomgr_logged = pc;
  }
  return made;
}
}  // namespace detail

inline void Configure(const Spec& spec) { detail::S().spec = spec; }

// The console target's enable_cheats (Gears 2's cheat keys): EnableCheats for player 1 as above. false = no player yet.
inline bool EnableForConsole(PPCContext& ctx, uint8_t* base) {
  using namespace detail;
  State& s = S();
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
  if (!AnyOn()) return;
  State& s = S();
  if (++s.ticks % 15) return;
  static std::atomic<bool> started{false};
  const int verdict = ue3::match::Check();   // also opens the object tables the first time
  if (!started.exchange(true)) std::thread(Resolve).detach();
  if (s.failed.load(std::memory_order_relaxed) || !s.ready.load(std::memory_order_acquire)) return;
  if (verdict != 0) return;   // Versus (or the lobbies), or the game state is not readable yet
  const int local = LocalCount();
  if (!local) return;
  const uint32_t pc0 = ControllerOf(0);
  const uint32_t world = pc0 ? obj::Read32(pc0 + uint32_t(s.off_world)) : 0;
  const uint32_t game = world ? obj::Read32(world + uint32_t(s.off_game)) : 0;
  if (!game) return;   // a client in someone else's game: nothing applies there
  if (!s.spec.remote_players_ok && !OnlyLocalPlayers(game, local)) {
    if (s.remote_world != world)
      REXLOG_INFO("auto cheats ({}): another PC's player is in this game - the launcher's cheats are not applied here", s.spec.game);
    s.remote_world = world;
    return;
  }
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
    if (!pawn) { p.pawn_seen = 0; continue; }   // the front end, a dead player waiting to respawn, a level loading
    any_pawn = true;
    if (REXCVAR_GET(ue3_cheat_god) && s.off_god >= 0 && !BitOn(pc, s.off_god, s.mask_god)) {
      SetBit(pc, s.off_god, s.mask_god, true);
      if (p.god_pc != pc) REXLOG_INFO("auto cheats ({}): god mode on (player {})", s.spec.game, i + 1);
      p.god_pc = pc;
    }
    if (REXCVAR_GET(ue3_cheat_hide_hud) && s.off_hud >= 0 && s.off_show >= 0) {
      const uint32_t hud = obj::Read32(pc + uint32_t(s.off_hud));
      if (hud && BitOn(hud, s.off_show, s.mask_show)) {
        SetBit(hud, s.off_show, s.mask_show, false);
        if (p.hud != hud) REXLOG_INFO("auto cheats ({}): HUD hidden (player {})", s.spec.game, i + 1);
        p.hud = hud;
      }
    }
    if (REXCVAR_GET(ue3_cheat_weapons) && s.spec.cheat_manager_possible && pawn != p.pawn_done) {
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
    }
  }
}

}  // namespace ue3::cheats
