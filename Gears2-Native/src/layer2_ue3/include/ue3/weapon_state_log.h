// Layer 2 (Unreal Engine 3 on the 360). May include layer 0-1 headers only.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

#include <rex/logging/macros.h>
#include <ue3/objects.h>   // the game's own ue3::obj (Gears 3 builds its own copy with the same calls)

// --<game>_log_weapon_state (Gears 1's, 22 Nov - Discord, co-op as the joining player: the Troika would not fire, the
// sniper became a Lancer, weapon switching stopped until death, and the logs had no weapon events; one copy for all four
// games since the 23 Nov parity pass): one line per local player whenever its pawn class, weapon, pending weapon (the
// switch in progress) or network role changes. A change only: a few lines a minute in play. Read-only.
//
// Each game's adaptive_triggers.cpp already finds the engine's player chain (GamePlayers, Player.Actor, Controller.Pawn,
// Pawn.Weapon) for its trigger effects; this adds InventoryManager.PendingWeapon, Pawn.InvManager and Actor.Role, and the
// line. The InventoryManager class is found the way that game finds its others (a sliced lookup on the game thread, or a
// whole-table one on Gears 1's worker thread); the offsets from it here.
namespace ue3 {

struct WeaponLogProps {
  uint32_t invManagerClass = 0;   // set by the caller's class lookup; 0 = not found (the switch shows as "?")
  int invManager = -1, pendingWeapon = -1, role = -1;
};

// Once the classes are known: the three offsets (-1 = not found, shown as "?"). `game` prefixes the log line.
inline void WeaponLogFindProps(const char* game, WeaponLogProps& w, uint32_t pawnClass, uint32_t actorClass) {
  w.invManager = pawnClass ? obj::PropertyOffset(pawnClass, "InvManager") : -1;
  w.role = actorClass ? obj::PropertyOffset(actorClass, "Role") : -1;
  if (w.role >= 0 && obj::PropertySize(actorClass, "Role") != 1) w.role = -1;   // a byte (the word's first)
  w.pendingWeapon = w.invManagerClass ? obj::PropertyOffset(w.invManagerClass, "PendingWeapon") : -1;
  REXLOG_INFO("{} weapon log: InvManager +0x{:X}, PendingWeapon +0x{:X}, Role +0x{:X}", game, w.invManager,
              w.pendingWeapon, w.role);
}

inline std::string WeaponLogClassName(uint32_t object) {
  if (!object) return "none";
  const uint32_t cls = obj::ClassOf(object);
  return cls ? obj::ObjectName(cls) : std::string("?");
}

// The player chain as the caller found it (offsets -1 / classes 0: not found - the caller only calls this when its own
// chain is usable). Call every 125 ms or so; logs changes only.
struct WeaponLogChain {
  uint32_t engineGlobal = 0;   // the guest address holding GEngine
  int gamePlayers = -1, actorOfPlayer = -1, pawnOfController = -1, weapon = -1;
  uint32_t localPlayer = 0, playerController = 0, pawn = 0;
};

inline void WeaponStateLogTick(const char* game, const WeaponLogChain& c, const WeaponLogProps& w) {
  static std::array<std::string, 4> last;
  if (c.gamePlayers < 0 || c.actorOfPlayer < 0 || c.pawnOfController < 0) return;
  const uint32_t engine = obj::Read32(c.engineGlobal);
  if (!engine) return;
  const uint32_t data = obj::Read32(engine + uint32_t(c.gamePlayers));
  const uint32_t count = std::min<uint32_t>(obj::Read32(engine + uint32_t(c.gamePlayers) + 4), 4);
  for (uint32_t i = 0; data && i < count; i++) {
    const uint32_t lp = obj::Read32(data + 4 * i);
    if (!lp || !obj::IsA(lp, c.localPlayer)) continue;
    const uint32_t pc = obj::Read32(lp + uint32_t(c.actorOfPlayer));
    if (!pc || !obj::IsA(pc, c.playerController)) continue;
    const uint32_t pawn = obj::Read32(pc + uint32_t(c.pawnOfController));
    const bool isPawn = pawn && obj::IsA(pawn, c.pawn);
    const uint32_t weapon = isPawn && c.weapon >= 0 ? obj::Read32(pawn + uint32_t(c.weapon)) : 0;
    uint32_t pending = 0;
    if (isPawn && w.invManager >= 0 && w.pendingWeapon >= 0) {
      const uint32_t im = obj::Read32(pawn + uint32_t(w.invManager));
      if (im && (!w.invManagerClass || obj::IsA(im, w.invManagerClass))) pending = obj::Read32(im + uint32_t(w.pendingWeapon));
    }
    const int pawnRole = isPawn && w.role >= 0 ? int(obj::Read32(pawn + uint32_t(w.role)) >> 24) : -1;
    const std::string now = "pawn " + (isPawn ? WeaponLogClassName(pawn) : std::string(pawn ? "not a pawn" : "none")) +
                            " | weapon " + WeaponLogClassName(weapon) + " | switching to " + WeaponLogClassName(pending) +
                            " | role " + (pawnRole >= 0 ? std::to_string(pawnRole) : std::string("?"));
    if (now == last[i]) continue;
    last[i] = now;
    REXLOG_INFO("{} weapon state: player {} | {}", game, i + 1, now);
  }
}

}  // namespace ue3
