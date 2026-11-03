// SPDX-License-Identifier: BSD-3-Clause
// gowj - ReXGlue Recompiled Project
//
// Exit Game on the main menu. The Coalesced/TU edit renames the defunct "Xbox LIVE
// Marketplace" entry; selecting it triggers marketplace UI (sub_82DBDA80, sub_82DBF838,
// sub_829C25E8, sub_829C2680, sub_829CAC40) or looks up the "NeedLive..." error text (sub_82302DE0, Localize).
// Any of these triggers ends the process immediately.

#include "main_menu_exit.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <cctype>
#include <cstring>
#include <string_view>

#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(gowj_main_menu_exit, true, "ui",
                    "Add Exit Game to the main menu (replaces Xbox LIVE Marketplace); restart");

namespace rex::glue {
namespace {

bool GuestUtf16StartsWithNoCase(const uint8_t* base, uint32_t addr, std::string_view expected) {
  if (!addr || !base) return false;
  for (size_t i = 0; i < expected.size(); ++i) {
    const uint16_t c = static_cast<uint16_t>((base[addr + 2 * i] << 8) | base[addr + 2 * i + 1]);
    if (c > 127) return false;
    if (std::tolower(static_cast<unsigned char>(c)) !=
        std::tolower(static_cast<unsigned char>(expected[i]))) {
      return false;
    }
  }
  return true;
}

bool GuestAsciiStartsWithNoCase(const uint8_t* base, uint32_t addr, std::string_view expected) {
  if (!addr || !base) return false;
  for (size_t i = 0; i < expected.size(); ++i) {
    const uint8_t c = base[addr + i];
    if (std::tolower(static_cast<unsigned char>(c)) !=
        std::tolower(static_cast<unsigned char>(expected[i]))) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool MatchesKey(const uint8_t* base, uint32_t addr, std::string_view expected) {
  if (!base || !addr) return false;
  return GuestUtf16StartsWithNoCase(base, addr, expected) ||
         GuestAsciiStartsWithNoCase(base, addr, expected);
}

bool MatchesNeedLive(const uint8_t* base, uint32_t addr) {
  return MatchesKey(base, addr, "NeedLive");
}

bool MatchesExitTrigger(const uint8_t* base, const PPCContext& ctx) {
  if (!base) return false;
  return MatchesKey(base, ctx.r5.u32, "TITLE_DLC") || MatchesKey(base, ctx.r5.u32, "NeedLive");
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
  if (REXCVAR_GET(gowj_main_menu_exit) && rex::glue::MatchesExitTrigger(base, ctx)) {
    rex::glue::ExitGame();
  }
  __imp__sub_82302DE0(ctx, base);
}

extern "C" void __imp__sub_82DBDA80(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82DBDA80(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(gowj_main_menu_exit)) {
    rex::glue::ExitGame();
  }
  __imp__sub_82DBDA80(ctx, base);
}

extern "C" void __imp__sub_82DBF838(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82DBF838(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(gowj_main_menu_exit)) {
    rex::glue::ExitGame();
  }
  __imp__sub_82DBF838(ctx, base);
}

extern "C" void __imp__sub_829C25E8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_829C25E8(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(gowj_main_menu_exit)) {
    rex::glue::ExitGame();
  }
  __imp__sub_829C25E8(ctx, base);
}

extern "C" void __imp__sub_829C2680(PPCContext& ctx, uint8_t* base);
extern "C" void sub_829C2680(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(gowj_main_menu_exit)) {
    rex::glue::ExitGame();
  }
  __imp__sub_829C2680(ctx, base);
}

extern "C" void __imp__sub_829CAC40(PPCContext& ctx, uint8_t* base);
extern "C" void sub_829CAC40(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(gowj_main_menu_exit)) {
    rex::glue::ExitGame();
  }
  __imp__sub_829CAC40(ctx, base);
}
