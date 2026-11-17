// SPDX-License-Identifier: BSD-3-Clause
// gow3 - ReXGlue Recompiled Project
//
// Horde bots with their own characters (Shagg 17 Nov, Discord report: four Horde bots named Prescott, Griffin, Bernie and
// Kim, all with Marcus's body). The Horde-bots button adds bots with the game's Friends exec while a wave is on;
// GearGameMP_Base.AddBot -> SetPlayerClass picks a character nobody on the team has and names the bot after it, then
// SetPlayerClassFromPlayerSelection gives the body: a class already in memory at once, otherwise
//     if (IsInState('PendingMatch') && PendingClasses.Length + LoadedClasses.Length < 12) CharacterStore.LoadPawnClass(...)
//     else SetPawnClass(None)                                  // -> SetPlayerClass falls back to DefaultCOGClass, Marcus
// so a bot added after the lobby is Marcus whatever its name (bytecode of GearGame.xxx export 30646; Title Update 6's
// script patch does not replace it). The game's own lobby bots are added in PendingMatch, where the load runs.
//
// --gow3_horde_bot_characters: while this PC hosts a Horde game, that one && reads || (EX_ native 0x82 -> 0x84, the same
// length and the same short-circuit skip), so the game's own background load runs mid-match as it does in the lobby.
// Mid-match the 12-class limit still applies; in Horde's own lobby (PendingMatch) || skips it (at most 5 players a side,
// so 12 classes are not reached). A bot spawned before its load ends plays that life as Marcus; LoadPawnClass's callback
// (PawnClassLoaded) then gives it its body, and the game sends the class to the other players as it does for lobby
// loads. In any other game type the byte is put back.
// The change is to the loaded script in memory (as main_menu_exit.cpp), never to the package file, so Title Update 6's
// package check (dirty disc) never sees it. Found by its bytes and its class, not by an address of one game build.
//
// --gow3_log_bot_classes (diagnostic, read only): every 2 s, each player's name and character number (GearPRI), each
// player's body (the class of their GearPawn) and the game's loaded character classes, logged when they change.
//
// Worker threads only: object scans on the game thread froze the game (main_menu_exit.cpp). Our code (MIT).

#include <ue3/objects.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <thread>

#include "gow3_addresses.h"

REXCVAR_DEFINE_BOOL(gow3_horde_bot_characters, false, "Gears3",
                    "Horde bots added during a match get the character they were given (their name) instead of Marcus: "
                    "the game's own character load also runs mid-match while this PC hosts Horde (the loaded script in "
                    "memory, never the package file)");
REXCVAR_DEFINE_BOOL(gow3_log_bot_classes, false, "Gears3",
                    "Diagnostic: log each player's name, character number and body class (GearPRI) when they change");

namespace {
namespace obj = ue3::obj;

constexpr uint8_t kAnd = 0x82, kOr = 0x84;

bool Open() { return obj::Init(gow3::addr::kGNamesVar, gow3::addr::kGObjectsVar); }

uint8_t Byte(uint32_t a) { return uint8_t(obj::Read32(a & ~3u) >> (8 * (3 - (a & 3u)))); }
uint32_t Word(uint32_t a) { return (uint32_t(Byte(a)) << 24) | (uint32_t(Byte(a + 1)) << 16) | (uint32_t(Byte(a + 2)) << 8) | Byte(a + 3); }

// The && of `IsInState('PendingMatch') && ...` in GearGameMP_Base.SetPlayerClassFromPlayerSelection: 82 | 61 19 (native
// 281, IsInState) | 21 <FName 'PendingMatch', number 0> | 4A (empty optional) 16 (end of parameters) | 18 (skip, the
// right side). FindCondition: 0 unless exactly one such byte is found (anything else: not the expected script - left alone).
bool IsCondition(uint32_t at) {
  const uint8_t b0 = Byte(at);
  return (b0 == kAnd || b0 == kOr) && Byte(at + 1) == 0x61 && Byte(at + 2) == 0x19 && Byte(at + 3) == 0x21 && Byte(at + 12) == 0x4A &&
         Byte(at + 13) == 0x16 && Byte(at + 14) == 0x18 && Word(at + 8) == 0 && obj::Name(Word(at + 4)) == "PendingMatch";
}

uint32_t FindCondition(int* functions) {
  uint32_t found = 0;
  int hits = 0;
  *functions = 0;
  const uint32_t function_cls = obj::FindClass("Function");
  if (!function_cls) return 0;
  for (uint32_t o : obj::AllObjects()) {
    if (obj::ClassOf(o) != function_cls || obj::ObjectName(o) != "SetPlayerClassFromPlayerSelection") continue;
    if (obj::ObjectName(obj::Read32(o + 40)) != "GearGameMP_Base") continue;   // the Outer (KTL, SP, Wingman have their own)
    ++*functions;
    const uint32_t data = obj::Read32(o + 76), n = obj::Read32(o + 80);     // UStruct::Script {data, count}
    if (!data || n < 16 || n > 4096 || !obj::Readable(data, n) || Byte(data + n - 1) != 0x53) continue;   // EX_EndOfScript
    for (uint32_t i = 0; i + 15 <= n; ++i) {
      if (!IsCondition(data + i)) continue;
      found = data + i;
      ++hits;
    }
  }
  return hits == 1 ? found : 0;
}

// A Horde GameInfo this PC runs (only a host has one; a client's world has none).
bool HordeHosted(uint32_t horde_cls) {
  if (!horde_cls) return false;
  for (uint32_t g : obj::FindInstancesOf(horde_cls))
    if (obj::IsLive(g)) return true;
  return false;
}

void CharactersWorker() {
  uint32_t at = 0, horde_cls = 0;
  bool gave_up = false;
  for (;; std::this_thread::sleep_for(std::chrono::seconds(2))) {
    if (gave_up) return;
    if (!obj::Ready() && !Open()) continue;
    if (!horde_cls) horde_cls = obj::FindClass("GearGameHorde_Base");
    if (!at) {
      int functions = 0;
      at = FindCondition(&functions);
      if (at) REXLOG_INFO("gow3_horde_bot_characters: found the character-load check at {:08X}", at);
      else if (functions) {   // the function is loaded but not as expected: looking again won't change it
        REXLOG_WARN("gow3_horde_bot_characters: SetPlayerClassFromPlayerSelection is not the expected script - left alone");
        gave_up = true;
      }
      if (!at) continue;
    }
    if (!IsCondition(at)) {   // the package was reloaded / the memory reused: look again
      at = 0;
      continue;
    }
    const uint8_t now = Byte(at);
    const uint8_t want = HordeHosted(horde_cls) ? kOr : kAnd;
    if (now == want) continue;
    obj::Write8(at, want);
    REXLOG_INFO("gow3_horde_bot_characters: {}", want == kOr ? "on (this PC hosts Horde): bots added mid-match load their character"
                                                             : "off (not hosting Horde): the game's own check");
  }
}

std::string FString(uint32_t a) {   // TArray<TCHAR> {data, count incl. the terminator}, UTF-16 big-endian
  const uint32_t data = obj::Read32(a), n = obj::Read32(a + 4);
  if (!data || n == 0 || n > 64 || !obj::Readable(data, n * 2)) return "";
  std::string s;
  for (uint32_t i = 0; i + 1 < n; ++i) {
    const uint16_t c = uint16_t((Byte(data + 2 * i) << 8) | Byte(data + 2 * i + 1));
    s += c >= 0x20 && c < 0x7F ? char(c) : '?';
  }
  return s;
}

// Job 1982 lesson: offsets looked up while a class was still loading came back -1 (GearPRI.PawnClass in one run,
// PreferredCOGIndex in the other), so they are looked up again until found. The body is read from each live GearPawn
// (its own class) with its PlayerReplicationInfo (name, character number); GearPRI.PawnClass (the class the game
// meant for that player, set by SetPawnClass) is logged too once it resolves.
void LogWorker() {
  int off_name = -1, off_cog = -1, cog_size = 0, off_pawn_pri = -1, off_loaded = -1, off_pending = -1, off_meant = -1;
  uint32_t pri_cls = 0, pawn_cls = 0, store_cls = 0;
  std::map<uint32_t, std::string> last;
  std::string last_store;
  for (;; std::this_thread::sleep_for(std::chrono::seconds(2))) {
    if (!obj::Ready() && !Open()) continue;
    if (off_name < 0 || off_cog < 0 || off_pawn_pri < 0) {
      pri_cls = obj::FindClass("GearPRI");
      pawn_cls = obj::FindClass("GearPawn");
      const uint32_t base_pawn = obj::FindClass("Pawn");
      if (!pri_cls || !pawn_cls || !base_pawn) continue;
      off_name = obj::PropertyOffset(pri_cls, "PlayerName");
      off_cog = obj::PropertyOffset(pri_cls, "PreferredCOGIndex");
      cog_size = obj::PropertySize(pri_cls, "PreferredCOGIndex");
      off_pawn_pri = obj::PropertyOffset(base_pawn, "PlayerReplicationInfo");
      if (off_name < 0 || off_cog < 0 || off_pawn_pri < 0) continue;
      REXLOG_INFO("gow3_log_bot_classes: GearPRI PlayerName +{} PreferredCOGIndex +{} ({} bytes), Pawn.PlayerReplicationInfo +{}", off_name,
                  off_cog, cog_size, off_pawn_pri);
    }
    if (off_meant < 0) off_meant = obj::PropertyOffset(pri_cls, "PawnClass");
    if (off_loaded < 0 && (store_cls = obj::FindClass("GearMPCharacterStore")) != 0) {
      off_loaded = obj::PropertyOffset(store_cls, "LoadedClasses");
      off_pending = obj::PropertyOffset(store_cls, "PendingClasses");
    }
    auto who = [&](uint32_t pri) {
      if (!pri || !obj::IsLive(pri)) return std::string("no player");
      const int cog = cog_size == 1 ? Byte(pri + uint32_t(off_cog)) : int32_t(obj::Read32(pri + uint32_t(off_cog)));
      const uint32_t meant = off_meant >= 0 ? obj::Read32(pri + uint32_t(off_meant)) : 0;
      return "'" + FString(pri + uint32_t(off_name)) + "' character " + std::to_string(cog) + " (meant " +
             (off_meant < 0 ? std::string("?") : meant ? obj::ObjectName(meant) : std::string("none")) + ")";
    };
    std::map<uint32_t, std::string> now;
    for (uint32_t p : obj::FindInstancesOf(pri_cls))
      if (obj::IsLive(p)) now[p] = "player " + who(p);
    for (uint32_t pawn : obj::FindInstancesOf(pawn_cls)) {   // the bodies (Locust too: their names show it)
      if (!obj::IsLive(pawn)) continue;
      const uint32_t pri = obj::Read32(pawn + uint32_t(off_pawn_pri));
      if (!pri) continue;   // pawns without a player entry (Horde AI may have one: then logged too)
      now[pawn] = "body " + obj::ObjectName(obj::ClassOf(pawn)) + " for " + who(pri);
    }
    if (now != last) {
      for (const auto& [o, line] : now) REXLOG_INFO("gow3_log_bot_classes: {:08X} {}", o, line);
      if (now.empty()) REXLOG_INFO("gow3_log_bot_classes: no players");
      last = std::move(now);
    }
    // the game's character store: which character classes are loaded / loading (the fix should add the bots' ones)
    if (store_cls && off_loaded >= 0) {
      std::string st;
      for (uint32_t c : obj::FindInstancesOf(store_cls)) {
        if (!obj::IsLive(c)) continue;
        const uint32_t data = obj::Read32(c + uint32_t(off_loaded)), n = obj::Read32(c + uint32_t(off_loaded) + 4);
        st += "loaded " + std::to_string(n) + ":";
        for (uint32_t i = 0; data && i < n && i < 24; ++i) st += " " + obj::ObjectName(obj::Read32(data + 4 * i));
        if (off_pending >= 0) st += " | pending " + std::to_string(obj::Read32(c + uint32_t(off_pending) + 4));
      }
      if (st != last_store) {
        REXLOG_INFO("gow3_log_bot_classes: character store {}", st.empty() ? std::string("none") : st);
        last_store = st;
      }
    }
  }
}

}  // namespace

void Gow3InstallHordeBotCharacters() {
  if (REXCVAR_GET(gow3_horde_bot_characters)) std::thread(CharactersWorker).detach();
  if (REXCVAR_GET(gow3_log_bot_classes)) std::thread(LogWorker).detach();
}
