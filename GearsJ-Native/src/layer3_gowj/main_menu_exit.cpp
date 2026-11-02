// SPDX-License-Identifier: BSD-3-Clause
// gowj - ReXGlue Recompiled Project
//
// Exit Game on the main menu. The Coalesced edit renames the defunct "Xbox LIVE
// Marketplace" entry; selecting it makes the game look up the "NeedLiveForDownloadableContent"
// error text (sub_82302DE0 in TU4, Localize) or invoke marketplace UI (sub_82DBF838).
// Either trigger ends the process instead.

#include "main_menu_exit.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <cstring>

#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(gowj_main_menu_exit, true, "ui",
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

extern "C" void __imp__sub_82302DE0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82302DE0(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(gowj_main_menu_exit) && rex::glue::MatchesNeedLive(base, ctx.r5.u32)) {
    rex::glue::ExitGame();
  }
  __imp__sub_82302DE0(ctx, base);
}

extern "C" void __imp__sub_82DBF838(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82DBF838(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(gowj_main_menu_exit)) {
    rex::glue::ExitGame();
  }
  __imp__sub_82DBF838(ctx, base);
}
