// SPDX-License-Identifier: BSD-3-Clause
// gow3 - ReXGlue Recompiled Project
//
// Exit Game on the main menu. The Coalesced edit renames the defunct "Xbox LIVE
// Marketplace" entry; selecting it makes the game look up the "NeedLiveForDownloadableContent"
// error text (sub_8230CF58 in TU6, Localize) before showing that error. That lookup ends the process
// instead - only while the front end is loaded (ue3::InFrontEndForExit: GearStart*.xxx is the last level
// package opened), so the same message in a level can't close the game.

#include "main_menu_exit.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <atomic>
#include <cstring>
#include <filesystem>
#include <string_view>

#include <rex/filesystem/vfs.h>  // REX_FILESYSTEM_FILE_OPEN_OBSERVER
#include <rex/logging.h>
#include <rex/ppc.h>
#include <ue3/main_menu_exit.h>
#include <rex/cvar.h>

#include <chrono>
#include <cstdlib>

void Ue3ShaderPrebuildChainFileOpened(void (*listener)(std::string_view, const std::filesystem::path*));  // shader_prebuild.cpp

namespace rex::glue {
namespace {

constexpr char kNeedLiveKey[] = "NeedLiveForDownloadableContent";

// The whole key, terminator included (wide, as Localize gets it).
bool GuestUtf16Is(const uint8_t* base, uint32_t addr, const char* text) {
  if (!addr || !base) return false;
  for (uint32_t i = 0;; ++i) {
    const uint16_t c = uint16_t((base[addr + 2 * i] << 8) | base[addr + 2 * i + 1]);
    if (c != uint8_t(text[i])) return false;
    if (!c) return true;
  }
}

void OnFileOpened(std::string_view guest_path, const std::filesystem::path*) {
  ue3::NoteFileOpenedForExit(guest_path);
}

}  // namespace

bool MatchesNeedLive(const uint8_t* base, uint32_t addr) {
  if (!addr || !base) return false;
  if (GuestUtf16Is(base, addr, kNeedLiveKey)) return true;
  // An 8-bit key: the whole key, terminator included.
  return std::memcmp(base + addr, kNeedLiveKey, sizeof(kNeedLiveKey)) == 0;
}

void ExitGame() {
  REXLOG_INFO("MAINMENU: Exit Game selected, ending the process");
  rex::FlushLogging();
#ifdef _WIN32
  TerminateProcess(GetCurrentProcess(), 0);
#else
  _exit(0);
#endif
}

}  // namespace rex::glue


// GitHub #215: when SDK patch 67's xam_media_verify_calls counter last changed (checked from the game tick, 4x a second).
static std::atomic<int64_t> g_mv_changed_ms{-1};
void Gow3MediaVerifyTick() {
  static uint32_t ticks = 0;
  static long long last = -1;
  if (++ticks % 15) return;
  const std::string v = rex::cvar::GetFlagByName("xam_media_verify_calls");
  if (v.empty()) return;  // a runtime without patch 67
  const long long n = std::atoll(v.c_str());
  if (n != last) {
    last = n;
    g_mv_changed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }
}
int64_t Gow3MsSinceMediaVerify() {
  const int64_t t = g_mv_changed_ms.load();
  if (t < 0) return -1;
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() - t;
}

void Gow3InstallMainMenuExitIfRequested() {
  if (!REXCVAR_GET(gow_main_menu_exit)) return;
#if defined(REX_FILESYSTEM_FILE_OPEN_OBSERVER)
  Ue3ShaderPrebuildChainFileOpened(&rex::glue::OnFileOpened);
  REXLOG_INFO("main_menu_exit: file-open hook installed (Exit Game while the front end is loaded)");
#else
  REXLOG_WARN("main_menu_exit: this SDK has no file-open observer (SDK patch 32); Exit Game does nothing");
#endif
}

#if defined(GOW3_TU6)
#define GOW3_NEED_LIVE_FN sub_8230CF58
#define GOW3_NEED_LIVE_IMP __imp__sub_8230CF58
#else
#define GOW3_NEED_LIVE_FN sub_822FC8D0
#define GOW3_NEED_LIVE_IMP __imp__sub_822FC8D0
#endif

extern "C" void GOW3_NEED_LIVE_IMP(PPCContext& ctx, uint8_t* base);

extern "C" void GOW3_NEED_LIVE_FN(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(gow_main_menu_exit) && rex::glue::MatchesNeedLive(base, ctx.r5.u32)) {
    // GitHub #215: back on the main menu after playing, Gears 3 opens no file (the front end stays resident), so the
    // file-open tracking still says "in a level". The menu calls XamMediaVerificationVerify twice a second (SDK patch
    // 67 counts it; nothing else does): a call within the last 3 s also means the front end.
    bool front = ue3::InFrontEndForExit();
    if (!front) {
      const int64_t ago = Gow3MsSinceMediaVerify();
      REXLOG_INFO("MAINMENU: #215 file tracking says level; last menu media check {} ms ago", ago);
      front = ago >= 0 && ago < 3000;
    }
    if (front) {
      REXLOG_INFO("MAINMENU: NeedLiveForDownloadableContent on the front end (lr {:08X})",
                  static_cast<uint32_t>(ctx.lr));
      rex::glue::ExitGame();
    }
    REXLOG_INFO("MAINMENU: NeedLiveForDownloadableContent in a level (lr {:08X}) - not Exit Game",
                static_cast<uint32_t>(ctx.lr));
  }
  GOW3_NEED_LIVE_IMP(ctx, base);
}
