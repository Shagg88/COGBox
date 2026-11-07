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
#include <rex/logging/macros.h>

#include <ue3/objects.h>

#include "gow3_addresses.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

REXCVAR_DEFINE_BOOL(gow3_checkpoint_stream_wait, true, "Gears3",
                    "GitHub #226: after a checkpoint load, the streamed sections it opens load completely (block on load) "
                    "before the world goes on");
REXCVAR_DEFINE_INT32(gow3_checkpoint_stream_wait_ms, 3000, "Gears3",
                     "GitHub #226: how long after a checkpoint file open streamed sections are made to block on load");

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

void OnFileOpened(std::string_view guest, const std::filesystem::path*) {
  const std::string l = Lower(guest);
  if (l.find("gearscheckpoint") != std::string::npos) {  // read at a load, written at a save (a save streams nothing)
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
  if (g_new.load(std::memory_order_relaxed) && now >= g_next_scan_ms) {
    g_new = false;
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
  }
  std::string names;
  for (const std::string& n : window) names += " " + n;
  REXLOG_INFO("checkpoint restore: waited {} ms for {} streamed packages ({} set to block on load):{}", last - start,
              window.size(), set, names);
  g_blocked.clear();
  g_last_stream_ms = -1;
}

void Gow3InstallCheckpointStreamWait() {
  if (!REXCVAR_GET(gow3_checkpoint_stream_wait)) return;
#if defined(REX_FILESYSTEM_FILE_OPEN_OBSERVER)
  Ue3ShaderPrebuildChainFileOpened(&OnFileOpened);
  REXLOG_INFO("gow3_checkpoint_stream_wait: on - streamed sections after a checkpoint load block on load (GitHub #226)");
#else
  REXLOG_WARN("gow3_checkpoint_stream_wait: this SDK has no file-open observer (SDK patch 32); off");
#endif
}
