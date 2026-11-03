// SPDX-License-Identifier: BSD-3-Clause
// gowj - ReXGlue Recompiled Project
//
// Exit Game on the main menu. The Coalesced/TU edit renames the defunct "Xbox LIVE
// Marketplace" entry; selecting it triggers marketplace UI (sub_82DBDA80, sub_82DBF838,
// sub_829C25E8, sub_829C2680, sub_829CAC40), looks up "NeedLive..." error text (sub_82302DE0),
// or transitions to MAINMENUOP_DLC_Categories (screen state setter sub_82D0EE70).
// Any of these events after boot-time initialization terminates the process immediately.

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
#include <xenos/dev_tools.h>

REXCVAR_DEFINE_BOOL(gowj_main_menu_exit, true, "ui",
                    "Add Exit Game to the main menu (replaces Xbox LIVE Marketplace); restart");

namespace rex::glue {
namespace {

inline bool IsValidGuestPtr(uint32_t addr) {
  // Physical RAM: 512 MB
  if (addr >= 0x00010000 && addr < 0x20000000) return true;
  // Game XEX image (code and data sections)
  if (addr >= 0x82000000 && addr < 0x83700000) return true;
  return false;
}

bool GuestUtf16StartsWithNoCase(const uint8_t* base, uint32_t addr, std::string_view expected) {
  if (!addr || !base || !IsValidGuestPtr(addr)) return false;
  for (size_t i = 0; i < expected.size(); ++i) {
    if (!IsValidGuestPtr(addr + static_cast<uint32_t>(2 * i + 1))) return false;
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
  if (!addr || !base || !IsValidGuestPtr(addr)) return false;
  for (size_t i = 0; i < expected.size(); ++i) {
    if (!IsValidGuestPtr(addr + static_cast<uint32_t>(i))) return false;
    const uint8_t c = base[addr + i];
    if (std::tolower(static_cast<unsigned char>(c)) !=
        std::tolower(static_cast<unsigned char>(expected[i]))) {
      return false;
    }
  }
  return true;
}

bool MatchesKey(const uint8_t* base, uint32_t addr, std::string_view expected) {
  if (!base || !addr || !IsValidGuestPtr(addr)) return false;
  return GuestUtf16StartsWithNoCase(base, addr, expected) ||
         GuestAsciiStartsWithNoCase(base, addr, expected);
}

}  // namespace

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
    if (rex::glue::MatchesNeedLive(base, ctx.r5.u32)) {
      rex::glue::ExitGame();
    }
  }
  __imp__sub_82302DE0(ctx, base);
}

extern "C" void __imp__sub_82D0EE70(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82D0EE70(PPCContext& ctx, uint8_t* base) {
  const uint32_t this_ptr = ctx.r3.u32;

  __imp__sub_82D0EE70(ctx, base);

  if (REXCVAR_GET(gowj_main_menu_exit) && base) {
    const uint64_t swaps = XenosSwapCount();
    // Gate to ensure boot-time initialization is complete
    if (swaps > 100) {
      if (rex::glue::IsValidGuestPtr(this_ptr) && rex::glue::IsValidGuestPtr(this_ptr + 216 + 7)) {
        const uint32_t screen_state_addr = this_ptr + 216;
        const uint8_t type = base[screen_state_addr];
        if (type == 5) { // Scaleform VT_String
          const uint32_t node = (static_cast<uint32_t>(base[screen_state_addr + 4]) << 24) |
                                (static_cast<uint32_t>(base[screen_state_addr + 5]) << 16) |
                                (static_cast<uint32_t>(base[screen_state_addr + 6]) << 8) |
                                static_cast<uint32_t>(base[screen_state_addr + 7]);
          if (rex::glue::IsValidGuestPtr(node) && rex::glue::IsValidGuestPtr(node + 3)) {
            const uint32_t str_addr = (static_cast<uint32_t>(base[node + 0]) << 24) |
                                      (static_cast<uint32_t>(base[node + 1]) << 16) |
                                      (static_cast<uint32_t>(base[node + 2]) << 8) |
                                      static_cast<uint32_t>(base[node + 3]);
            if (rex::glue::IsValidGuestPtr(str_addr)) {
              const char* s = reinterpret_cast<const char*>(base + str_addr);
              if (strstr(s, "DLC_Categories") != nullptr || strstr(s, "TITLE_DLC") != nullptr) {
                REXLOG_INFO("MAINMENU: Exit Game selected (screen={}), terminating process immediately", s);
                rex::glue::ExitGame();
              }
            }
          }
        }
      }
    }
  }
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
