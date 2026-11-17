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
// length and the same short-circuit skip), so the game's own background load runs mid-match as it does in the lobby -
// the same 12-class limit still applies mid-match; LoadPawnClass's callback (PawnClassLoaded) gives the bot its body and
// the game sends the class to the other players as it does for lobby loads. In any other game type the byte is put back.
// The change is to the loaded script in memory (as main_menu_exit.cpp), never to the package file, so Title Update 6's
// package check (dirty disc) never sees it. Found by its bytes and its class, not by an address of one game build.
//
// --gow3_log_bot_classes (diagnostic, read only): every 2 s, each player's name, character number and actual body
// (GearPRI PlayerName / PreferredCOGIndex / PawnClass), logged when they change.
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
// right side). 0 unless exactly one such byte is found (anything else: not the expected script - left alone).
uint32_t FindCondition(int* functions) {
  uint32_t found = 0;
  int hits = 0;
  *functions = 0;
  for (uint32_t o : obj::AllObjects()) {
    if (obj::ObjectName(o) != "SetPlayerClassFromPlayerSelection" || obj::ObjectName(obj::ClassOf(o)) != "Function") continue;
    if (obj::ObjectName(obj::Read32(o + 40)) != "GearGameMP_Base") continue;   // the Outer (KTL, SP, Wingman have their own)
    ++*functions;
    const uint32_t data = obj::Read32(o + 76), n = obj::Read32(o + 80);     // UStruct::Script {data, count}
    if (!data || n < 16 || n > 4096 || !obj::Readable(data, n)) continue;
    for (uint32_t i = 0; i + 15 <= n; ++i) {
      const uint8_t b0 = Byte(data + i);
      if (b0 != kAnd && b0 != kOr) continue;
      if (Byte(data + i + 1) != 0x61 || Byte(data + i + 2) != 0x19 || Byte(data + i + 3) != 0x21 || Byte(data + i + 12) != 0x4A ||
          Byte(data + i + 13) != 0x16 || Byte(data + i + 14) != 0x18)
        continue;
      if (Word(data + i + 8) != 0 || obj::Name(Word(data + i + 4)) != "PendingMatch") continue;
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
  bool warned = false;
  for (;; std::this_thread::sleep_for(std::chrono::seconds(2))) {
    if (!obj::Ready() && !Open()) continue;
    if (!horde_cls) horde_cls = obj::FindClass("GearGameHorde_Base");
    if (!at) {
      int functions = 0;
      at = FindCondition(&functions);
      if (at) REXLOG_INFO("gow3_horde_bot_characters: found the character-load check at {:08X}", at);
      else if (functions && !warned) {
        REXLOG_WARN("gow3_horde_bot_characters: SetPlayerClassFromPlayerSelection is not the expected script - left alone");
        warned = true;
      }
      if (!at) continue;
    }
    const uint8_t now = Byte(at);
    if (now != kAnd && now != kOr) {   // the package was reloaded / the memory reused: look again
      at = 0;
      continue;
    }
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

void LogWorker() {
  int off_name = -1, off_class = -1, off_cog = -1, cog_size = 0;
  uint32_t pri_cls = 0;
  std::map<uint32_t, std::string> last;
  for (;; std::this_thread::sleep_for(std::chrono::seconds(2))) {
    if (!obj::Ready() && !Open()) continue;
    if (!pri_cls) {
      pri_cls = obj::FindClass("GearPRI");
      if (!pri_cls) continue;
      off_name = obj::PropertyOffset(pri_cls, "PlayerName");
      off_class = obj::PropertyOffset(pri_cls, "PawnClass");
      off_cog = obj::PropertyOffset(pri_cls, "PreferredCOGIndex");
      cog_size = obj::PropertySize(pri_cls, "PreferredCOGIndex");
      REXLOG_INFO("gow3_log_bot_classes: GearPRI PlayerName +{} PawnClass +{} PreferredCOGIndex +{} ({} bytes)", off_name, off_class,
                  off_cog, cog_size);
    }
    std::map<uint32_t, std::string> now;
    for (uint32_t p : obj::FindInstancesOf(pri_cls)) {
      if (!obj::IsLive(p)) continue;
      const std::string name = off_name >= 0 ? FString(p + uint32_t(off_name)) : "?";
      const uint32_t cls = off_class >= 0 ? obj::Read32(p + uint32_t(off_class)) : 0;
      int cog = -1;
      if (off_cog >= 0) cog = cog_size == 1 ? Byte(p + uint32_t(off_cog)) : int32_t(obj::Read32(p + uint32_t(off_cog)));
      now[p] = "'" + name + "' character " + std::to_string(cog) + " body " + (cls ? obj::ObjectName(cls) : std::string("none"));
    }
    if (now == last) continue;
    for (const auto& [p, line] : now) REXLOG_INFO("gow3_log_bot_classes: {:08X} {}", p, line);
    if (now.empty()) REXLOG_INFO("gow3_log_bot_classes: no players");
    last = std::move(now);
  }
}

}  // namespace

void Gow3InstallHordeBotCharacters() {
  if (REXCVAR_GET(gow3_horde_bot_characters)) std::thread(CharactersWorker).detach();
  if (REXCVAR_GET(gow3_log_bot_classes)) std::thread(LogWorker).detach();
}
