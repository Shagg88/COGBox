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
#include <string>
#include <string_view>

#include <rex/logging.h>
#include <rex/ppc.h>
#include <xenos/dev_tools.h>

REXCVAR_DEFINE_BOOL(gowj_main_menu_exit, true, "ui",
                    "Add Exit Game to the main menu (replaces Xbox LIVE Marketplace); restart");

namespace rex::glue {
namespace {

inline bool IsMemoryReadable(const void* ptr, size_t size) {
  if (!ptr || size == 0) return false;
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery(ptr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
  if (mbi.State != MEM_COMMIT) return false;
  if ((mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) == 0) return false;
  if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
  const uint8_t* p = static_cast<const uint8_t*>(ptr);
  const uint8_t* region_end = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
  return (p + size <= region_end);
#else
  return true;
#endif
}

inline bool IsValidGuestPtr(const uint8_t* base, uint32_t addr, size_t size = 1) {
  if (!base || addr < 0x00010000) return false;
  // Any 32-bit guest address can be valid: the game's heap objects (the main menu widget among them)
  // live at 0x40000000-0x7FFFFFFF, the image at 0x82000000, physical memory views from 0xA0000000.
  // Whether the page is committed is what decides (a sparse reservation: reading an uncommitted page
  // faults).
  if (uint64_t(addr) + size > 0x100000000ull) return false;
  return IsMemoryReadable(base + addr, size);
}

inline uint32_t SafeReadU32BE(const uint8_t* base, uint32_t addr) {
  if (!IsValidGuestPtr(base, addr, 4)) return 0;
  return (static_cast<uint32_t>(base[addr + 0]) << 24) |
         (static_cast<uint32_t>(base[addr + 1]) << 16) |
         (static_cast<uint32_t>(base[addr + 2]) << 8)  |
          static_cast<uint32_t>(base[addr + 3]);
}

bool GuestUtf16StartsWithNoCase(const uint8_t* base, uint32_t addr, std::string_view expected) {
  if (!base || !IsValidGuestPtr(base, addr, expected.size() * 2)) return false;
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
  if (!base || !IsValidGuestPtr(base, addr, expected.size())) return false;
  for (size_t i = 0; i < expected.size(); ++i) {
    const uint8_t c = base[addr + i];
    if (std::tolower(static_cast<unsigned char>(c)) !=
        std::tolower(static_cast<unsigned char>(expected[i]))) {
      return false;
    }
  }
  return true;
}

bool MatchesKey(const uint8_t* base, uint32_t addr, std::string_view expected) {
  if (!base || !addr) return false;
  return GuestUtf16StartsWithNoCase(base, addr, expected) ||
         GuestAsciiStartsWithNoCase(base, addr, expected);
}

bool CheckStringCandidate(const uint8_t* base, uint32_t addr) {
  if (!IsValidGuestPtr(base, addr, 4)) return false;
  char buf[64];
  size_t len = 0;
  for (; len < sizeof(buf) - 1; ++len) {
    if (!IsValidGuestPtr(base, addr + static_cast<uint32_t>(len), 1)) break;
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

// The screen name in the widget's currentScreen copy (this + 216: an AS2 value, type byte at +0, 5 =
// string, +4 = the string node whose +0 is the text), or "" when it isn't a readable string.
std::string ScreenName(const uint8_t* base, uint32_t this_ptr) {
  if (!IsValidGuestPtr(base, this_ptr + 216, 8) || base[this_ptr + 216] != 5) return {};
  const uint32_t node = SafeReadU32BE(base, this_ptr + 216 + 4);
  const uint32_t text = node ? SafeReadU32BE(base, node) : 0;
  std::string out;
  for (uint32_t i = 0; text && i < 96 && IsValidGuestPtr(base, text + i, 1); ++i) {
    const char c = static_cast<char>(base[text + i]);
    if (c < 32 || c > 126) break;  // also stops at the terminating NUL
    out += c;
  }
  return out;
}

bool CheckObjectForExitScreen(const uint8_t* base, uint32_t this_ptr) {
  if (!IsValidGuestPtr(base, this_ptr, 230)) return false;

  // UIWidget_MainMenu stores currentScreen (Scaleform GFxValue) at this_ptr + 216.
  const uint32_t screen_state_addr = this_ptr + 216;
  const uint8_t type = base[screen_state_addr];

  // Scaleform VT_String is 5 (or bitfield flags combined, e.g. 0x45)
  if ((type & 0x0F) == 5 || type == 5) {
    const uint32_t node = SafeReadU32BE(base, screen_state_addr + 4);
    if (node) {
      // 1. Scaleform GFxStringNode: offset 0 is pData (pointer to char buffer)
      const uint32_t str_addr = SafeReadU32BE(base, node + 0);
      if (str_addr && CheckStringCandidate(base, str_addr)) return true;
      // 2. Direct string pointer at node
      if (CheckStringCandidate(base, node)) return true;
    }
  }

  // Also check secondary field or raw string pointer at screen_state_addr + 4
  const uint32_t direct_ptr = SafeReadU32BE(base, screen_state_addr + 4);
  if (direct_ptr) {
    if (CheckStringCandidate(base, direct_ptr)) return true;
    const uint32_t deref = SafeReadU32BE(base, direct_ptr + 0);
    if (deref && CheckStringCandidate(base, deref)) return true;
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
    // One line per screen change, so the log shows which screen names the menu reports.
    static std::string last_screen;
    if (std::string screen = rex::glue::ScreenName(base, this_ptr); screen != last_screen) {
      REXLOG_INFO("MAINMENU: screen '{}' (widget {:08X})", screen, this_ptr);
      last_screen = std::move(screen);
    }
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
