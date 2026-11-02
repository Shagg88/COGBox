// SPDX-License-Identifier: BSD-3-Clause
// gow3 - ReXGlue Recompiled Project
//
// Exit Game on the main menu. The Coalesced edit renames the defunct "Xbox LIVE
// Marketplace" entry; selecting it makes the game look up the "NeedLiveForDownloadableContent"
// error text (sub_8230CF58 in TU6, Localize) before showing that error. That lookup ends the process
// instead.

#include "main_menu_exit.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <cstring>

#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(gow3_main_menu_exit, true, "ui",
                    "Add Exit Game to the main menu (replaces Xbox LIVE Marketplace); restart");

namespace rex::glue {
namespace {

bool GuestUtf16Is(const uint8_t* base, uint32_t addr, const char* text) {
  if (!addr || !base) return false;
  for (uint32_t i = 0;; ++i) {
    const uint16_t c = uint16_t((base[addr + 2 * i] << 8) | base[addr + 2 * i + 1]);
    if (c != uint8_t(text[i])) return false;
    if (!c) return true;
  }
}

}  // namespace

bool MatchesNeedLive(const uint8_t* base, uint32_t addr) {
  if (!addr || !base) return false;
  if (GuestUtf16Is(base, addr, "NeedLiveForDownloadableContent")) return true;
  const char* str = reinterpret_cast<const char*>(base + addr);
  return std::strncmp(str, "NeedLiveForDownloadableContent", 30) == 0;
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

#if defined(GOW3_TU6)
#define GOW3_NEED_LIVE_FN sub_8230CF58
#define GOW3_NEED_LIVE_IMP __imp__sub_8230CF58
#else
#define GOW3_NEED_LIVE_FN sub_822FC8D0
#define GOW3_NEED_LIVE_IMP __imp__sub_822FC8D0
#endif

extern "C" void GOW3_NEED_LIVE_IMP(PPCContext& ctx, uint8_t* base);

extern "C" void GOW3_NEED_LIVE_FN(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(gow3_main_menu_exit) && rex::glue::MatchesNeedLive(base, ctx.r5.u32)) {
    rex::glue::ExitGame();
  }
  GOW3_NEED_LIVE_IMP(ctx, base);
}
