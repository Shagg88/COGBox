// Layer 3 (Gears of War 3): the "squad catch-up" engine-difference adapter of the mod framework (KiltedKernel; design
// docs/modding.md, [Campaign] SquadCatchUp = <distance>). Our code (MIT).
//
// Converted levels can leave a squad AI unable to find its way: in the trilogy's prison, Dom's path search on the hard
// route fails (the navigation joins between regions are not resolved at run time), he stays behind, and the hard
// route's door - kicked open only when Marcus and Dom both stand at it - never opens: the player is shut in between
// two pillars. Gears 1 ran the same Kismet with an AI that kept up.
//
// While a mod chapter plays and an enabled campaign asks for it: player 1's walk leaves a trail (a point every 150
// units, the last 40 kept: places a pawn stood); a squad AI (a COG pawn under an AI controller, alive) that is farther
// than the distance from player 1, has not moved 100 units in 6 s and whose last path search failed (its
// NavigationHandle's LastPathError 2-5: anchor pylon, no path, no valid final destination, no next move location; a
// waiting AI reads 0 - Gears 3's EPathFindingError has no "none") is put on the trail
// point 300-800 units behind player 1. A squad mate that is moving (a scripted path of its own, a split of the squad)
// or waiting where Kismet put it (its search did not fail: Dom at the prison's first door, waiting for the player) is
// never moved.
#include "gow3_addresses.h"

#include <ue3/mod_extras.h>
#include <ue3/objects.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <thread>

REXCVAR_DEFINE_BOOL(mod_squad_log, false, "Gears3",
                    "Developer mode: the squad AIs' positions every 10 s in mod chapters (squad catch-up's view)");

uint32_t Gow3ModChapterGame();   // mod_chapters.cpp: the game info while a mod chapter's map is loaded (0: none)

namespace {
namespace obj = ue3::obj;

struct Vec { double x = 0, y = 0, z = 0; };
double Dist2D(const Vec& a, const Vec& b) { return std::hypot(a.x - b.x, a.y - b.y); }

struct Props {
  int loc = -1, pawn = -1, controller = -1, health = -1, worldInfo = -1, pawnList = -1, nextPawn = -1, navHandle = -1,
      pathError = -1;
  uint32_t pc = 0, ai = 0;
  bool Find() {
    if (pc && ai && loc >= 0 && pawn >= 0 && controller >= 0 && worldInfo >= 0 && pawnList >= 0 && nextPawn >= 0) return true;
    const uint32_t actor = obj::FindClass("Actor"), pawnCls = obj::FindClass("Pawn"), ctrl = obj::FindClass("Controller"),
                   wi = obj::FindClass("WorldInfo");
    pc = obj::FindClass("GearPC");
    ai = obj::FindClass("GearAI");
    if (!actor || !pawnCls || !ctrl || !wi || !pc || !ai) return false;
    loc = obj::PropertyOffset(actor, "Location");
    worldInfo = obj::PropertyOffset(actor, "WorldInfo");
    pawn = obj::PropertyOffset(ctrl, "Pawn");
    controller = obj::PropertyOffset(pawnCls, "Controller");
    health = obj::PropertyOffset(pawnCls, "Health");
    nextPawn = obj::PropertyOffset(pawnCls, "NextPawn");
    pawnList = obj::PropertyOffset(wi, "PawnList");
    if (const uint32_t nh = obj::FindClass("NavigationHandle")) pathError = obj::PropertyOffset(nh, "LastPathError");
    navHandle = obj::PropertyOffset(ctrl, "NavigationHandle");
    return loc >= 0 && pawn >= 0 && controller >= 0 && worldInfo >= 0 && pawnList >= 0 && nextPawn >= 0;
  }
  // the controller's last path search failed (-1: unknown - never treated as failed)
  int PathError(uint32_t ctrl) const {
    if (navHandle < 0 || pathError < 0) return -1;
    const uint32_t nh = obj::Read32(ctrl + uint32_t(navHandle));
    return nh ? int(obj::Read32(nh + uint32_t(pathError)) >> 24) : -1;
  }
  Vec At(uint32_t a) const {
    return {obj::ReadFloat(a + uint32_t(loc)), obj::ReadFloat(a + uint32_t(loc) + 4), obj::ReadFloat(a + uint32_t(loc) + 8)};
  }
} g_p;

void Put(uint32_t actor, const Vec& v) {
  const float f[3] = {float(v.x), float(v.y), float(v.z)};
  for (int k = 0; k < 3; k++) {
    uint32_t u;
    std::memcpy(&u, &f[k], 4);
    obj::Write32(actor + uint32_t(g_p.loc) + 4 * k, u);
  }
}

void Run(int distance) {
  using Clock = std::chrono::steady_clock;
  struct Watch { Vec since; Clock::time_point at; };
  std::map<uint32_t, Watch> watched;   // squad pawn -> where it was when it last moved 100 units, and when
  std::deque<Vec> trail;
  uint32_t player = 0, lastGame = 0;
  Clock::time_point lastFind{}, lastLog{};
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const uint32_t game = Gow3ModChapterGame();
    if (!game || !obj::Ready() || !g_p.Find()) { player = 0; trail.clear(); watched.clear(); continue; }
    if (game != lastGame) { lastGame = game; player = 0; trail.clear(); watched.clear(); }
    const Clock::time_point now = Clock::now();
    if (!player || now - lastFind > std::chrono::seconds(5)) {   // player 1's pawn (the first GearPC with one)
      lastFind = now;
      player = 0;
      for (uint32_t c : obj::FindInstancesOf(g_p.pc))
        if (const uint32_t p = obj::Read32(c + uint32_t(g_p.pawn))) { player = p; break; }
    }
    if (!player) continue;
    const Vec me = g_p.At(player);
    if (std::hypot(me.x, me.y) > 50 && (trail.empty() || Dist2D(trail.back(), me) > 150)) {   // (not the spawn origin)
      trail.push_back(me);
      if (trail.size() > 40) trail.pop_front();
    }
    const bool logging = REXCVAR_GET(mod_squad_log) && now - lastLog > std::chrono::seconds(10);
    // the squad: COG pawns under an AI controller, alive (the world's pawn list)
    const uint32_t wi = obj::Read32(player + uint32_t(g_p.worldInfo));
    uint32_t p = wi ? obj::Read32(wi + uint32_t(g_p.pawnList)) : 0;
    for (int guard = 0; p && guard < 256; guard++, p = obj::Read32(p + uint32_t(g_p.nextPawn))) {
      if (p == player) continue;
      const uint32_t ctrl = obj::Read32(p + uint32_t(g_p.controller));
      if (!ctrl || !obj::IsA(ctrl, g_p.ai)) continue;
      if (g_p.health >= 0 && int32_t(obj::Read32(p + uint32_t(g_p.health))) <= 0) continue;
      const std::string cls = obj::ObjectName(obj::ClassOf(p));
      if (cls.find("COG") == std::string::npos) continue;
      const Vec at = g_p.At(p);
      Watch& w = watched.try_emplace(p, Watch{at, now}).first->second;
      if (REXCVAR_GET(mod_squad_log) && now - lastLog > std::chrono::seconds(10))
        REXLOG_INFO("mod_api: squad: {} at {:.0f},{:.0f} ({:.0f} from player 1 at {:.0f},{:.0f}), still for {:.0f} s, path error {}",
                    cls, at.x, at.y, Dist2D(at, me), me.x, me.y, std::chrono::duration<double>(now - w.at).count(),
                    g_p.PathError(ctrl));
      if (Dist2D(at, w.since) > 100) { w = {at, now}; continue; }   // moving: its own way
      if (Dist2D(at, me) <= distance || now - w.at < std::chrono::seconds(6)) continue;
      if (g_p.PathError(ctrl) < 2) continue;   // its search did not fail: waiting where it was told to
      // stuck and far: the trail point 300-800 units behind player 1 (the farthest such one)
      const Vec* spot = nullptr;
      for (const Vec& t : trail)
        if (const double d = Dist2D(t, me); d >= 300 && d <= 800 && (!spot || d > Dist2D(*spot, me))) spot = &t;
      if (!spot) continue;
      REXLOG_INFO("mod_api: squad catch-up: {} stuck {:.0f} away at {:.0f},{:.0f} - moved behind player 1 to {:.0f},{:.0f}",
                  cls, Dist2D(at, me), at.x, at.y, spot->x, spot->y);
      Put(p, *spot);
      w = {*spot, now};
    }
    if (logging) lastLog = now;
  }
}

}  // namespace

// OnPostSetup, after the mods are read: nothing unless an enabled campaign has SquadCatchUp
void Gow3InstallSquadCatchUp() {
  const int distance = ue3::ModSquadCatchUp();
  if (distance <= 0) return;
  REXLOG_INFO("mod_api: squad catch-up on in mod chapters (a squad AI stuck {}+ units away is brought behind player 1)", distance);
  std::thread(Run, distance).detach();
}
