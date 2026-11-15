// Layer 2 (Unreal Engine 3): "Cheats are off in Versus" (0.6.3). The developer cheats of every COGBox game (--gow1_cheats,
// --gow2_cheats, --gow3_cheats, --gowj_cheats and any --ue3_hotkeys / --ue3_exec command) run through ue3/console.h; its
// Ue3ConsoleTarget::gate asks this guard before each command. The guard reads the running game's GameInfo class from the
// engine's objects (ue3/objects.h) at that moment - nothing is remembered between commands, so going from a campaign to a
// Versus match (or back) is seen at once:
//   GEngine -> GamePlayers[0] (ULocalPlayer) -> Actor (its PlayerController) -> WorldInfo -> Game (the GameInfo: a game this
//   PC runs or hosts - the campaign, local split screen, System Link / online host) or, with no GameInfo (a client in
//   someone else's game), WorldInfo.GRI -> GameClass (the GameReplicationInfo's copy of the GameInfo's class).
// Cheats run only in the game types each game lists as allowed (the campaign, Horde, Beast, Survival, the front end), minus
// any listed as denied under them (the party / pre-game lobbies - in Gears 3 and Judgment the front end and every lobby
// run as one, so cheats are off there too); every other class counts as Versus - a game type
// nobody listed (a DLC mode, a Gears 1 mode whose parent class was never read) is refused, never let through (review r41).
// Per-game lists are in each game's cheats.cpp (Gears 3: extras.cpp), with where the names come from. Classes that cannot be read (no GameInfo
// and no GRI class) are treated as Versus.
//
// Header only (included by each game's cheats.cpp, which compiles against that game's own ue3/objects.h; the four copies of
// this file and of console.h are kept identical). Our code (MIT).
#pragma once

#include <ue3/objects.h>

#include <rex/logging/macros.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace ue3::match {

struct Spec {
  const char* game;                        // for the log
  const char* const* allowed;              // nullptr-terminated: game types (and their subclasses) where cheats run
  const char* const* denied;               // nullptr-terminated: subclasses of those where they do not (Versus lobbies)
  uint32_t engine_global;                  // GEngine's address
  uint32_t players_data_offset;            // UGameEngine::GamePlayers data pointer / count (the console target's)
  uint32_t players_num_offset;
  bool (*open_objects)();                  // ue3::obj::Init with this game's globals (and layout); worker thread, retried
};

namespace detail {
struct Resolved {
  Spec spec{};
  std::vector<uint32_t> allowed, denied;
  int off_actor = -1, off_world = -1, off_game = -1, off_gri = -1, off_game_class = -1;
  std::atomic<bool> ready{false}, failed{false};
  // game thread only
  uint32_t last_world = 0, last_key = 0;
  bool last_refused = false, warned_failed = false;
};
inline Resolved& State() {
  static Resolved s;
  return s;
}

inline bool Derives(uint32_t cls, const std::vector<uint32_t>& list) {
  for (uint32_t base : list)
    if (obj::ClassIsA(cls, base)) return true;
  return false;
}

// Worker thread: the tables, then every class and property the check reads. Retried every 2 s while the engine starts,
// then every 15 s, about 12 minutes in all (as the DualSense trigger code); failing, the guard refuses (never allows).
inline void Resolve() {
  Resolved& s = State();
  for (int attempt = 0; attempt < 60; attempt++) {
    if (attempt) std::this_thread::sleep_for(std::chrono::seconds(attempt < 15 ? 2 : 15));
    if (!obj::Ready() && !(s.spec.open_objects && s.spec.open_objects())) continue;
    // Every listed name must be found while the engine starts (the first 30 s of tries): some are script classes that only
    // exist once the game's script package is loaded (review r42). After that, the first allowed name (the campaign class)
    // is enough; any other not found is logged and left out - a missing allowed class only refuses that mode (the safe
    // side), a missing denied class lets that lobby keep the cheats.
    std::vector<uint32_t> found[2];
    std::string missing;
    const char* const* lists[2] = {s.spec.allowed, s.spec.denied};
    for (int k = 0; k < 2; k++)
      for (const char* const* n = lists[k]; n && *n; ++n) {
        const uint32_t c = obj::FindClass(*n);
        if (c) found[k].push_back(c);
        else missing += std::string(missing.empty() ? "" : " ") + *n;
      }
    const bool first = s.spec.allowed && s.spec.allowed[0] && obj::FindClass(s.spec.allowed[0]) != 0;
    const bool all = first && (missing.empty() || attempt >= 15);
    std::vector<uint32_t>& allowed = found[0];
    std::vector<uint32_t>& denied = found[1];
    const uint32_t engine = obj::FindClass("GameEngine"), local_player = obj::FindClass("LocalPlayer"),
                   actor = obj::FindClass("Actor"), world = obj::FindClass("WorldInfo"),
                   gri = obj::FindClass("GameReplicationInfo");
    if (!all || !engine || !local_player || !actor || !world || !gri) continue;
    const int players = obj::PropertyOffset(engine, "GamePlayers");
    const int off_actor = obj::PropertyOffset(local_player, "Actor"), off_world = obj::PropertyOffset(actor, "WorldInfo"),
              off_game = obj::PropertyOffset(world, "Game"), off_gri = obj::PropertyOffset(world, "GRI"),
              off_game_class = obj::PropertyOffset(gri, "GameClass");
    if (players != int(s.spec.players_data_offset)) {   // the object layout (or the console target) is wrong: never guess
      REXLOG_WARN("match guard ({}): GamePlayers at +{} by reflection, +{} in the console target - cheats stay off", s.spec.game,
                  players, s.spec.players_data_offset);
      s.failed = true;
      return;
    }
    if (off_actor < 0 || off_world < 0 || off_game < 0) continue;   // (GRI.GameClass is the client path only)
    s.allowed = allowed, s.denied = denied;
    s.off_actor = off_actor, s.off_world = off_world, s.off_game = off_game, s.off_gri = off_gri,
    s.off_game_class = off_game_class;
    s.ready.store(true, std::memory_order_release);
    if (!missing.empty()) REXLOG_WARN("match guard ({}): game type(s) not found, left out: {}", s.spec.game, missing);
    REXLOG_INFO("match guard ({}): ready - cheats are refused in Versus (LocalPlayer.Actor +{}, Actor.WorldInfo +{}, "
                "WorldInfo.Game +{}, .GRI +{}, GameReplicationInfo.GameClass +{})",
                s.spec.game, off_actor, off_world, off_game, off_gri, off_game_class);
    return;
  }
  REXLOG_WARN("match guard ({}): the engine's classes (or {}) were not found - cheats stay off", State().spec.game,
              State().spec.allowed && State().spec.allowed[0] ? State().spec.allowed[0] : "the campaign class");
  State().failed = true;
}
}  // namespace detail

// OnPostSetup, before the console target is set (its gate = Check). Nothing runs until the first cheat command is asked
// about: a player who never uses cheats never pays for the object tables (the worker starts from Check).
inline void Configure(const Spec& spec) { detail::State().spec = spec; }

// What to do with a cheat command now (Ue3ConsoleTarget::gate; game thread): 0 run it, 1 keep it queued (the engine is
// loading: no player / world yet, or the first call: the classes are being looked up), 2 refuse it (Versus, or not known to be anything else).
inline int Check() {
  detail::Resolved& s = detail::State();
  static std::atomic<bool> started{false};
  if (!started.exchange(true)) std::thread(detail::Resolve).detach();
  if (s.failed.load(std::memory_order_relaxed)) {
    if (!s.warned_failed) REXLOG_WARN("Cheats are off: the match type could not be read ({})", s.spec.game);
    s.warned_failed = true;
    return 2;
  }
  if (!s.ready.load(std::memory_order_acquire)) return 1;
  const uint32_t engine = obj::Read32(s.spec.engine_global);
  if (!engine) return 1;
  const uint32_t count = obj::Read32(engine + s.spec.players_num_offset), data = obj::Read32(engine + s.spec.players_data_offset);
  if (int32_t(count) <= 0 || !data) return 1;
  const uint32_t player = obj::Read32(data);
  const uint32_t pc = player ? obj::Read32(player + uint32_t(s.off_actor)) : 0;
  const uint32_t world = pc ? obj::Read32(pc + uint32_t(s.off_world)) : 0;
  if (!world) return 1;
  uint32_t cls = 0, key = 0;
  const uint32_t game = obj::Read32(world + uint32_t(s.off_game));
  if (game) {
    cls = obj::ClassOf(game), key = game;   // a game this PC runs or hosts
  } else if (s.off_gri >= 0 && s.off_game_class >= 0) {
    const uint32_t gri = obj::Read32(world + uint32_t(s.off_gri));   // a client: the host's GameInfo class, replicated
    if (gri) cls = obj::Read32(gri + uint32_t(s.off_game_class)), key = gri;
  }
  const bool versus = !cls || !detail::Derives(cls, s.allowed) || detail::Derives(cls, s.denied);
  if (!versus) {
    s.last_refused = false;
    return 0;
  }
  // once per match: a different world / GameInfo than the last refusal, or the first refusal after a command that ran
  if (!s.last_refused || world != s.last_world || key != s.last_key) {
    if (cls) REXLOG_WARN("Cheats are off in Versus and in the lobbies (game type {})", obj::ObjectName(cls));
    else REXLOG_WARN("Cheats are off in Versus (the game type is not known yet)");
  }
  s.last_refused = true, s.last_world = world, s.last_key = key;
  return 2;
}

}  // namespace ue3::match
