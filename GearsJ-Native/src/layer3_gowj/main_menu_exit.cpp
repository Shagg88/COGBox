// SPDX-License-Identifier: BSD-3-Clause
// gowj - ReXGlue Recompiled Project
//
// Exit Game on the main menu. The Coalesced/TU edit renames the defunct "Xbox LIVE
// Marketplace" entry; selecting it triggers marketplace UI (sub_82DBDA80, sub_82DBF838,
// sub_829C25E8, sub_829C2680, sub_829CAC40) or looks up the "NeedLive..." error text (sub_82302DE0, Localize).
// When MAINMENUOP_DLC_Categories is navigated to, its title (TITLE_DLC) is requested; after the
// boot-time pre-cache pass, querying TITLE_DLC terminates the process immediately.

#include "main_menu_exit.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <string_view>

#include <rex/logging.h>
#include <rex/ppc.h>
#include <xenos/dev_tools.h>

REXCVAR_DEFINE_BOOL(gowj_main_menu_exit, true, "ui",
                    "Add Exit Game to the main menu (replaces Xbox LIVE Marketplace); restart");

namespace rex::glue {
namespace {

const auto g_start_time = std::chrono::steady_clock::now();
std::atomic<uint32_t> g_title_dlc_count{0};
std::atomic<bool> g_saw_tooltip_dlc{false};

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
  if (REXCVAR_GET(gowj_main_menu_exit) && base) {
    if (rex::glue::MatchesKey(base, ctx.r5.u32, "TOOLTIP_DLC")) {
      rex::glue::g_saw_tooltip_dlc.store(true, std::memory_order_relaxed);
    }

    if (rex::glue::MatchesNeedLive(base, ctx.r5.u32)) {
      rex::glue::ExitGame();
    }

    if (rex::glue::MatchesKey(base, ctx.r5.u32, "TITLE_DLC")) {
      const uint32_t count = rex::glue::g_title_dlc_count.fetch_add(1, std::memory_order_relaxed);
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - rex::glue::g_start_time).count();
      const uint64_t swaps = XenosSwapCount();

      // UIWidget_MainMenu pre-caches screen titles from AllOptions during initial boot (count == 0).
      // Only subsequent queries (count > 0, when the player actually selects Exit Game and navigates
      // to MAINMENUOP_DLC_Categories), or queries after the tooltip was shown / gameplay frames swapped,
      // trigger immediate process exit.
      if (count > 0 && (rex::glue::g_saw_tooltip_dlc.load(std::memory_order_relaxed) ||
                        swaps > 30 || elapsed > 3)) {
        rex::glue::ExitGame();
      } else {
        REXLOG_INFO("MAINMENU: TITLE_DLC queried during initial pre-cache (count: {}, swaps: {}, elapsed: {}s), skipping exit",
                    count, swaps, elapsed);
      }
    }
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
