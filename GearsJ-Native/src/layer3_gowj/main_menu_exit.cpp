// SPDX-License-Identifier: BSD-3-Clause
// gowj - ReXGlue Recompiled Project
//
// Exit Game on the main menu. The Coalesced/TU edit (layer2 main_menu_exit.cpp) renames the
// defunct "Xbox LIVE Marketplace" entry and its sub-menu title (TITLE_DLC) to Exit Game.
//
// Judgment builds and localizes every menu screen once, at load, so no text lookup marks the
// moment the entry is chosen. What does: opening a sub-menu writes its title into the header
// text field ("MAIN MENU" / "<screen title>"), through the text-field setter sub_82D157F0
// (vtable slot 420) called from sub_82D15EF8 at 0x82D160A4. Hovering an entry only redraws the
// list and its preview (the call at 0x82D16114), never that header. So the header being set to
// the Exit Game label ends the process.
//
// Fallbacks: the marketplace UI calls (sub_82DBDA80, sub_82DBF838, sub_829C25E8, sub_829C2680,
// sub_829CAC40) and the "NeedLive..." error text lookup in Localize (sub_82302DE0:
// Localize(FString* Result, Section, Key, Package, LangExt, bOptional), r5 = Key).

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
#include <vector>

#include <rex/logging.h>
#include <rex/ppc.h>
#include <ue3/main_menu_exit.h>

REXCVAR_DEFINE_BOOL(gowj_main_menu_exit, true, "ui",
                    "Add Exit Game to the main menu (replaces Xbox LIVE Marketplace); restart");

namespace rex::glue {
namespace {

// The return address of the header-title call in sub_82D15EF8.
constexpr uint32_t kHeaderTextCallReturn = 0x82D160A4;

bool IsMemoryReadable(const void* ptr, size_t size) {
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

// Any 32-bit guest address can be valid (heap 0x40000000-0x7FFFFFFF, image 0x82000000, physical
// views from 0xA0000000); whether the page is committed decides.
bool IsValidGuestPtr(const uint8_t* base, uint32_t addr, size_t size) {
  if (!base || addr < 0x00010000) return false;
  if (uint64_t(addr) + size > 0x100000000ull) return false;
  return IsMemoryReadable(base + addr, size);
}

bool MatchesKey(const uint8_t* base, uint32_t addr, std::string_view expected) {
  if (!base || !addr) return false;
  if (IsValidGuestPtr(base, addr, expected.size() * 2)) {
    bool wide = true;
    for (size_t i = 0; i < expected.size() && wide; ++i) {
      const uint16_t c = static_cast<uint16_t>((base[addr + 2 * i] << 8) | base[addr + 2 * i + 1]);
      wide = c < 128 && std::tolower(c) == std::tolower(static_cast<unsigned char>(expected[i]));
    }
    if (wide) return true;
  }
  if (!IsValidGuestPtr(base, addr, expected.size())) return false;
  for (size_t i = 0; i < expected.size(); ++i) {
    if (std::tolower(base[addr + i]) != std::tolower(static_cast<unsigned char>(expected[i]))) return false;
  }
  return true;
}

// The Exit Game label in every language the Coalesced edit writes (UTF-8, as Scaleform gets it).
const std::vector<std::string>& ExitLabels() {
  static const std::vector<std::string> labels = [] {
    std::vector<std::string> out;
    for (const char* lang : {"INT", "FRA", "DEU", "ITA", "ESN", "ESM", "PTB", "POL", "RUS", "CZE", "HUN", "KOR",
                             "CHN", "CHT"}) {
      std::string label = ue3::GetExitStringsForLang(lang, ue3::GameId::Judgment).label;
      if (!label.empty()) out.push_back(std::move(label));
    }
    return out;
  }();
  return labels;
}

// Whether the UTF-8 text at addr (HTML tags and surrounding spaces ignored) is an Exit Game label.
bool IsExitLabel(const uint8_t* base, uint32_t addr) {
  constexpr uint32_t kMax = 512;
  std::string text;
  bool in_tag = false;
  for (uint32_t i = 0; i < kMax; ++i) {
    if ((i % 64) == 0 && !IsValidGuestPtr(base, addr + i, 1)) return false;  // one check per 64 bytes
    const char c = static_cast<char>(base[addr + i]);
    if (c == 0) break;
    if (c == '<') in_tag = true;
    else if (c == '>') in_tag = false;
    else if (!in_tag) text += c;
    if (text.size() > 64) return false;  // longer than any label
  }
  const size_t b = text.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return false;
  text = text.substr(b, text.find_last_not_of(" \t\r\n") - b + 1);
  for (const std::string& label : ExitLabels()) {
    if (text == label) return true;
  }
  return false;
}

}  // namespace

bool MatchesNeedLive(const uint8_t* base, uint32_t addr) { return MatchesKey(base, addr, "NeedLive"); }

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

// Text-field setter (r3 = the field, r4 = UTF-8 text).
extern "C" void __imp__sub_82D157F0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82D157F0(PPCContext& ctx, uint8_t* base) {
  if (static_cast<uint32_t>(ctx.lr) == rex::glue::kHeaderTextCallReturn && REXCVAR_GET(gowj_main_menu_exit) &&
      base && ctx.r4.u32 && rex::glue::IsExitLabel(base, ctx.r4.u32)) {
    REXLOG_INFO("MAINMENU: Exit Game sub-menu opened");
    rex::glue::ExitGame();
  }
  __imp__sub_82D157F0(ctx, base);
}

extern "C" void __imp__sub_82302DE0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82302DE0(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(gowj_main_menu_exit) && base && rex::glue::MatchesNeedLive(base, ctx.r5.u32)) {
    rex::glue::ExitGame();
  }
  __imp__sub_82302DE0(ctx, base);
}

#define GOWJ_EXIT_ON_CALL(fn)                                   \
  extern "C" void __imp__##fn(PPCContext& ctx, uint8_t* base); \
  extern "C" void fn(PPCContext& ctx, uint8_t* base) {         \
    if (REXCVAR_GET(gowj_main_menu_exit)) {                    \
      REXLOG_INFO("MAINMENU: marketplace call " #fn);          \
      rex::glue::ExitGame();                                   \
    }                                                          \
    __imp__##fn(ctx, base);                                    \
  }

GOWJ_EXIT_ON_CALL(sub_82DBDA80)
GOWJ_EXIT_ON_CALL(sub_82DBF838)
GOWJ_EXIT_ON_CALL(sub_829C25E8)
GOWJ_EXIT_ON_CALL(sub_829C2680)
GOWJ_EXIT_ON_CALL(sub_829CAC40)
