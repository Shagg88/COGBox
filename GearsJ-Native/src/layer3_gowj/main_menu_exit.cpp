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

inline uint32_t SafeReadU32BE(const uint8_t* base, uint32_t addr) {
  if (!base || !IsValidGuestPtr(addr) || !IsValidGuestPtr(addr + 3)) return 0;
  return (static_cast<uint32_t>(base[addr + 0]) << 24) |
         (static_cast<uint32_t>(base[addr + 1]) << 16) |
         (static_cast<uint32_t>(base[addr + 2]) << 8)  |
          static_cast<uint32_t>(base[addr + 3]);
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

bool CheckStringCandidate(const uint8_t* base, uint32_t addr) {
  if (!base || !IsValidGuestPtr(addr) || !IsValidGuestPtr(addr + 3)) return false;
  char buf[64];
  size_t len = 0;
  for (; len < sizeof(buf) - 1; ++len) {
    if (!IsValidGuestPtr(addr + static_cast<uint32_t>(len))) break;
    const char c = static_cast<char>(base[addr + len]);
    if (c == '\0') break;
    if (c < 32 || c > 126) return false;
    buf[len] = c;
  }
  buf[len] = '\0';
  if (len < 3) return false;
  if (strstr(buf, "DLC_Categories") != nullptr ||
      strstr(buf, "MAINMENUOP_DLC") != nullptr ||
      strstr(buf, "TITLE_DLC") != nullptr) {
    REXLOG_INFO("MAINMENU: Exit screen string matched ('{}' at {:08X})", buf, addr);
    return true;
  }
  return false;
}

bool CheckObjectForExitScreen(const uint8_t* base, uint32_t this_ptr) {
  if (!base || !IsValidGuestPtr(this_ptr)) return false;
  // UIWidget_MainMenu stores currentScreen at this_ptr + 216 and related screen state
  // across this_ptr + 200..260. Check all pointer fields.
  for (uint32_t offset = 200; offset <= 260; offset += 4) {
    const uint32_t ptr = SafeReadU32BE(base, this_ptr + offset);
    if (!IsValidGuestPtr(ptr)) continue;

    // 1. Direct ASCII string at ptr
    if (CheckStringCandidate(base, ptr)) return true;
    // 2. Scaleform GFxStringNode (text starts at offset 12)
    if (CheckStringCandidate(base, ptr + 12)) return true;

    // 3. Indirect string pointer
    const uint32_t deref = SafeReadU32BE(base, ptr);
    if (IsValidGuestPtr(deref)) {
      if (CheckStringCandidate(base, deref)) return true;
      if (CheckStringCandidate(base, deref + 12)) return true;
    }

    // 4. Scaleform GFxValue secondary field at ptr + 4
    const uint32_t deref4 = SafeReadU32BE(base, ptr + 4);
    if (IsValidGuestPtr(deref4)) {
      if (CheckStringCandidate(base, deref4)) return true;
      if (CheckStringCandidate(base, deref4 + 12)) return true;
    }
  }
  return false;
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
    const uint64_t swaps = XenosSwapCount();
    if (swaps > 100 && rex::glue::MatchesKey(base, ctx.r5.u32, "TITLE_DLC")) {
      REXLOG_INFO("MAINMENU: TITLE_DLC queried after boot (swaps={}), terminating process immediately", swaps);
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
      if (rex::glue::CheckObjectForExitScreen(base, this_ptr)) {
        REXLOG_INFO("MAINMENU: Exit Game transition detected on main menu widget (this={:08X}, swaps={}), terminating process immediately",
                    this_ptr, swaps);
        rex::glue::ExitGame();
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
