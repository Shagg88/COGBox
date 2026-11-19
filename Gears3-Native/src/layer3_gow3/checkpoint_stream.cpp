// GitHub #226 (RAAM's Shadow ch.4, SP_CityHall_P): loading the checkpoint saved before the squad split sometimes gave
// "Objective failed" (reloading it worked). Shagg's log: the checkpoint (SG0_0:\GearsCheckpoint*.sav) is read, and only
// AFTER that are the streamed sections SP_CityHall_CogSplit_01_S / _02_S opened - the restored Kismet most likely
// checks its actors before those sections are in.
//
// The engine's own lever, by reflection (no guest address): LevelStreaming.bShouldBlockOnLoad (mod_chapters.cpp's
// checkpoint restore already sets it). Within --gow3_checkpoint_stream_wait_ms of a checkpoint file open, every streamed
// package (*_S.xxx / *_BG.xxx) the game opens has its LevelStreaming object(s) that should be loaded set to block on
// load, so the engine finishes them before the world goes on (as the 360's slower restore gave it time to). The flags
// are put back as they were once no streamed package has opened for 750 ms, or at the time limit.
//
// Why not hold the game thread host-side: UE3 creates a package's linker (the file open) and serializes it on the game
// thread, so sleeping there stalls the very loading it waits for. The shader prebuild's file-open listener is not in
// the way: it copies the path into its queue and returns (shader_prebuild.cpp OnFileOpened); its reader is a separate
// low-priority thread.
//
// Uncertain: whether this engine build flushes an async load already in flight when bShouldBlockOnLoad is set after the
// request (UE3 flushes on the next UpdateLevelStreaming in the builds we know); the log line says how many were set.
#include <rex/cvar.h>
#include <rex/filesystem/vfs.h>   // REX_FILESYSTEM_FILE_OPEN_OBSERVER (GitHub #273: without it this file built with the wait off)
#include <rex/logging/macros.h>

#include <ue3/objects.h>

#include "gow3_addresses.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

REXCVAR_DEFINE_BOOL(gow3_checkpoint_stream_wait, true, "Gears3",
                    "GitHub #226: after a checkpoint load, the streamed sections it opens load completely (block on load) "
                    "before the world goes on");
REXCVAR_DEFINE_INT32(gow3_checkpoint_stream_wait_ms, 3000, "Gears3",
                     "GitHub #226: how long after a checkpoint file open streamed sections are made to block on load");

// GitHub #309 (RAAM's Shadow ch.4: after a death "under the shadow" the game-over screen's Reload Checkpoint was greyed).
// The game-over screen is UIScene_GameOver, a UIScene_Pause: its options come from UIScene_Pause.PopulateOptionsList,
// which greys an option when ShouldOptionBeDisabled says so. The function the game runs (mod_chapters.cpp,
// AllowCheckpointReload) greys EPO_LoadCheckpoint while any GearGameSP_Base.PlayerSlots entry has PawnClass == None or
// Owner == None. Kismet empties slots itself: SeqAct_ClearPlayerSlot sets both to None, SeqAct_AIFactory's
// AssignToPlayerSlot destroys the slot's AI and sets Owner = None until the new one takes it. Read-only: every change
// of the four squad slots is logged with that verdict, so a death with Reload greyed shows which slot was empty and
// since when (and the Kismet trace, --mod_kismet_trace=CityHall, shows which op emptied it).
REXCVAR_DEFINE_BOOL(gow3_squad_slot_log, true, "Gears3",
                    "GitHub #309: log each change of the campaign's squad slots (GearGameSP_Base.PlayerSlots) and whether "
                    "the game would grey Reload last checkpoint");

extern std::atomic<int> g_gow3ReloadRule;   // mod_chapters.cpp
#if defined(GOW3_TU6) && GOW3_TU6
constexpr bool kTu6Build = true;   // TU6's ShouldOptionBeDisabled has the squad-slot check; the base disc's doesn't
#else
constexpr bool kTu6Build = false;
#endif

void Ue3ShaderPrebuildChainFileOpened(void (*listener)(std::string_view, const std::filesystem::path*));  // shader_prebuild.cpp

namespace {
namespace obj = ue3::obj;
using Clock = std::chrono::steady_clock;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
std::string Lower(std::string_view s) {
  std::string l(s);
  for (char& c : l) c = char(std::tolower(static_cast<unsigned char>(c)));
  return l;
}

std::mutex g_mutex;
std::atomic<int64_t> g_checkpoint_ms{-1};    // last checkpoint file open
std::atomic<int64_t> g_last_stream_ms{-1};   // last streamed package opened after it
std::atomic<bool> g_new{false};              // packages not handled on the game thread yet
std::set<std::string> g_window;              // streamed packages opened in this window (lower case, no extension)
std::set<std::string> g_pending;             // of those, not matched to a LevelStreaming object yet

void OnFileOpened(std::string_view guest, const std::filesystem::path* host) {
  const std::string l = Lower(guest);
  if (l.find("gearscheckpoint") != std::string::npos) {  // read at a load, written at a save (a save streams nothing)
    // Only a load starts the window. A save opens the file created or truncated (the SDK does that before the
    // observer is told), so it is empty here; a load opens the full checkpoint.
    std::error_code ec;
    if (host && std::filesystem::file_size(*host, ec) == 0 && !ec) return;
    g_checkpoint_ms = NowMs();
    return;
  }
  const int64_t t = g_checkpoint_ms.load();
  if (t < 0 || NowMs() - t > REXCVAR_GET(gow3_checkpoint_stream_wait_ms)) return;
  if (!(l.ends_with("_s.xxx") || l.ends_with("_bg.xxx"))) return;
  const size_t slash = l.find_last_of("\\/:");
  std::string base = l.substr(slash == std::string::npos ? 0 : slash + 1);
  base.resize(base.size() - 4);
  std::lock_guard lock(g_mutex);
  if (g_window.insert(base).second) g_pending.insert(base);
  g_last_stream_ms = NowMs();
  g_new = true;
}

struct Blocked { uint32_t ls; bool was; };
std::vector<Blocked> g_blocked;   // game thread only
int64_t g_next_scan_ms = 0;
uint32_t g_cls = 0;                // LevelStreaming and its properties, once found
int g_name = -1, g_loaded = -1, g_block = -1;
uint32_t g_loadedMask = 0, g_blockMask = 0;

}  // namespace

// Game thread, every engine tick (game_patches.cpp Gow3MaxTickRate).
void Gow3CheckpointStreamTick() {
  if (!REXCVAR_GET(gow3_checkpoint_stream_wait)) return;
  const int64_t last = g_last_stream_ms.load(std::memory_order_relaxed);
  if (last < 0) return;
  const int64_t now = NowMs();
  // 0.6.6 (review #19): taken in one step - a package opened between a separate load and clear was lost until the next
  // open; exchange leaves a later open's flag set for the next tick
  if (now >= g_next_scan_ms && g_new.exchange(false)) {
    g_next_scan_ms = now + 200;  // a full object scan: at most five a second, only in this window
    uint32_t& cls = g_cls;
    int &name = g_name, &loaded = g_loaded, &block = g_block;
    uint32_t &loadedMask = g_loadedMask, &blockMask = g_blockMask;
    if (!cls && (obj::Ready() || obj::Init(gow3::addr::kGNamesVar, gow3::addr::kGObjectsVar))) {
      cls = obj::FindClass("LevelStreaming");
      if (cls) {
        name = obj::PropertyOffset(cls, "PackageName");
        loaded = obj::PropertyOffset(cls, "bShouldBeLoaded");
        loadedMask = obj::BoolMask(cls, "bShouldBeLoaded");
        block = obj::PropertyOffset(cls, "bShouldBlockOnLoad");
        blockMask = obj::BoolMask(cls, "bShouldBlockOnLoad");
      }
    }
    if (cls && name >= 0 && loaded >= 0 && block >= 0 && loadedMask && blockMask) {
      std::set<std::string> pending;
      {
        std::lock_guard lock(g_mutex);
        pending = g_pending;
      }
      for (uint32_t ls : obj::FindInstancesOf(cls)) {
        const std::string pkg = Lower(obj::Name(obj::Read32(ls + uint32_t(name))));
        if (!pending.count(pkg)) continue;
        {
          std::lock_guard lock(g_mutex);
          g_pending.erase(pkg);
        }
        if (!(obj::Read32(ls + uint32_t(loaded)) & loadedMask)) continue;
        const uint32_t v = obj::Read32(ls + uint32_t(block));
        g_blocked.push_back({ls, (v & blockMask) != 0});
        if (!(v & blockMask)) obj::Write32(ls + uint32_t(block), v | blockMask);
      }
    } else {
      static bool warned = false;
      if (!warned) REXLOG_WARN("checkpoint restore: LevelStreaming properties not found - streamed sections not waited for");
      warned = true;
    }
  }
  const int64_t start = g_checkpoint_ms.load();
  if (now - last < 750 && now - start < REXCVAR_GET(gow3_checkpoint_stream_wait_ms) + 750) return;
  // the window is over: the flags back as they were
  size_t set = 0;
  for (const Blocked& b : g_blocked) {
    if (b.was || !obj::IsA(b.ls, g_cls)) continue;
    ++set;
    obj::Write32(b.ls + uint32_t(g_block), obj::Read32(b.ls + uint32_t(g_block)) & ~g_blockMask);
  }
  std::set<std::string> window;
  {
    std::lock_guard lock(g_mutex);
    window.swap(g_window);
    g_pending.clear();
    g_last_stream_ms = -1;  // with the swap: a package recorded after it starts a new window, not lost
  }
  std::string names;
  for (const std::string& n : window) names += " " + n;
  REXLOG_INFO("checkpoint restore: waited {} ms for {} streamed packages ({} set to block on load):{}", last - start,
              window.size(), set, names);
  g_blocked.clear();
}

namespace {
// Worker thread, twice a second; reads only (checked reads, safe off the game thread). The game info is looked up again
// (a full object scan) at most every 5 s, and only while the cached one is gone (a new map).
void SquadSlotLog() {
  while (!obj::Ready() && !obj::Init(gow3::addr::kGNamesVar, gow3::addr::kGObjectsVar))
    std::this_thread::sleep_for(std::chrono::seconds(1));
  uint32_t gameCls = 0, slotStruct = 0, game = 0;
  int slots = -1, ownerOff = -1, pawnOff = -1, stride = 0;
  int64_t nextLookup = 0;
  std::string last;
  for (;; std::this_thread::sleep_for(std::chrono::milliseconds(500))) {
    const int64_t now = NowMs();
    if (!gameCls) {
      if (now < nextLookup) continue;
      nextLookup = now + 5000;
      gameCls = obj::FindClass("GearGameSP_Base");
      slotStruct = obj::FindObject("PlayerSlotInfo");
      if (!gameCls || !slotStruct) { gameCls = 0; continue; }
      slots = obj::PropertyOffset(gameCls, "PlayerSlots");
      ownerOff = obj::PropertyOffset(slotStruct, "Owner");
      pawnOff = obj::PropertyOffset(slotStruct, "PawnClass");
      const int ownerSize = obj::PropertySize(slotStruct, "Owner"), pawnSize = obj::PropertySize(slotStruct, "PawnClass");
      stride = std::max(ownerOff + ownerSize, pawnOff + pawnSize);
      stride = (stride + 3) & ~3;
      if (slots < 0 || ownerOff < 0 || pawnOff < 0 || ownerSize != 4 || pawnSize != 4 || stride != 8) {
        // review r23 R1: while GearGame loads, the properties can be found before they are set up - tried again later
        static int tries = 0;   // r25: a minute of tries (the properties being set up), then not logged
        if (++tries > 12) {
          REXLOG_WARN("squad slots: GearGameSP_Base.PlayerSlots not as expected - not logged (GitHub #309)");
          return;
        }
        gameCls = 0;
        continue;
      }
      REXLOG_INFO("squad slots: PlayerSlots +0x{:X}, PlayerSlotInfo Owner +{} PawnClass +{}, {} bytes each", slots, ownerOff,
                  pawnOff, stride);
    }
    if (!game || !obj::IsLive(game) || !obj::IsA(game, gameCls)) {   // freed or a new map: look again (review r23 B1)
      game = 0;
      if (!last.empty()) { REXLOG_INFO("squad slots: no campaign game info (map unloaded)"); last.clear(); }
      if (now < nextLookup) continue;
      nextLookup = now + 5000;
      const auto games = obj::FindInstancesOf(gameCls);
      if (games.empty()) continue;
      game = games.front();
    }
    const uint32_t data = obj::Read32(game + uint32_t(slots)), count = obj::Read32(game + uint32_t(slots) + 4);
    if (count > 8) continue;   // (being written)
    std::string line;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t e = data + i * uint32_t(stride);
      const uint32_t pawnClass = obj::Read32(e + uint32_t(pawnOff)), owner = obj::Read32(e + uint32_t(ownerOff));
      line += std::format(" [{}] {} / {}", i + 1, pawnClass ? obj::ObjectName(pawnClass) : std::string("None"),
                          owner ? obj::ObjectName(owner) : std::string("None"));
    }
    // review r23 B2: the verdict is the squad-slot check's only (the screen also needs a chapter, and a co-op client
    // has its own rules), under the rule in effect: the game's (TU6), the mods' (mod_chapters) or none (base disc)
    const int rule = kTu6Build ? g_gow3ReloadRule.load() : 2;
    // job 1876: while a checkpoint reload rebuilds the level, the new game info briefly holds its default slots (Marcus /
    // Dom, no controllers) - no player is in any slot then and no menu can be open, so that state gets no verdict
    bool player = false;   // the default slots a reload briefly holds have no controller at all
    for (uint32_t i = 0; i < count && !player; ++i) {
      const uint32_t ow = obj::Read32(data + i * uint32_t(stride) + uint32_t(ownerOff));
      player = ow != 0;   // r36: any controller in any slot = a real state (the player's own slot may be the empty one)
    }
    std::string verdict;
    if (!player && rule != 2) verdict = "not judged (no controller in any slot yet - the level is being set up)";
    else if (rule == 2) verdict = "not affected (no squad-slot check in this game version)";
    else {
      std::string grey;
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t e = data + i * uint32_t(stride);
        const uint32_t pc = obj::Read32(e + uint32_t(pawnOff)), ow = obj::Read32(e + uint32_t(ownerOff));
        if (rule == 1 ? (pc && !ow) : (!pc || !ow)) grey += " " + std::to_string(i + 1);
      }
      verdict = grey.empty() ? std::string("passes the squad-slot check")
                             : "greyed by the squad-slot check (slot" + grey + (rule == 1 ? ", mods' rule)" : ", game's rule)");
    }
    if (line.empty()) line = " (no squad slots)";   // r25 (r23 N4)
    if (line + verdict == last) continue;   // r25: a change of the rule is logged too
    last = line + verdict;
    static int lines = 0;   // a cap, as kismet_trace's
    if (++lines > 2000) continue;
    REXLOG_INFO("squad slots:{} | Reload last checkpoint {}{}", line, verdict,
                lines == 2000 ? " - last line (2000)" : "");
  }
}
}  // namespace

void Gow3InstallCheckpointStreamWait() {
  if (REXCVAR_GET(gow3_squad_slot_log)) std::thread(SquadSlotLog).detach();   // GitHub #309 (read-only)
  if (!REXCVAR_GET(gow3_checkpoint_stream_wait)) return;
#if defined(REX_FILESYSTEM_FILE_OPEN_OBSERVER)
  Ue3ShaderPrebuildChainFileOpened(&OnFileOpened);
  REXLOG_INFO("gow3_checkpoint_stream_wait: on - streamed sections after a checkpoint load block on load (GitHub #226)");
#else
  REXLOG_WARN("gow3_checkpoint_stream_wait: this SDK has no file-open observer (SDK patch 32); off");
#endif
}
