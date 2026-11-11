// SPDX-License-Identifier: BSD-3-Clause
// gow3 - ReXGlue Recompiled Project
//
// Exit Game on the main menu. The Coalesced edit renames the defunct "Xbox LIVE Marketplace" entry, whose script is
// UIScene_FrontEnd.OpenMarketplaceBlade: one check, then the Marketplace, or an error message keyed
// "NeedLiveForDownloadableContent". A worker thread patches that one function so it always takes the error path and
// gives the key a name nothing else uses. When the game looks that name up (sub_8230CF58 in TU6, Localize) the button
// was pressed, wherever the player has been: no menu or level tracking, and other Marketplace screens are untouched.

#include "main_menu_exit.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <chrono>
#include <cstring>
#include <thread>

#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/cvar.h>
#include <rex/kernel/xam/net/ue3_script.h>

namespace rex::glue {
namespace {

constexpr char kOriginalKey[] = "NeedLiveForDownloadableContent";
constexpr char kExitKey[] = "CogboxExitGame_MainMenuButton_";  // the same length: it replaces the key in place
static_assert(sizeof(kOriginalKey) == sizeof(kExitKey));

constexpr uint8_t kJumpIfNot = 0x07, kJump = 0x06, kStringConst = 0x1F;

// The whole key, terminator included (wide, as Localize gets it).
bool GuestUtf16Is(const uint8_t* base, uint32_t addr, const char* text) {
  if (!addr || !base) return false;
  for (uint32_t i = 0;; ++i) {
    const uint16_t c = uint16_t((base[addr + 2 * i] << 8) | base[addr + 2 * i + 1]);
    if (c != uint8_t(text[i])) return false;
    if (!c) return true;
  }
}

bool PatchExitButtonImpl() {
  uint32_t count = 0;
  uint8_t* code = rex::kernel::xam::net::FindUe3Function("OpenMarketplaceBlade", &count);
  if (!code) return false;
  const size_t len = sizeof(kOriginalKey);
  for (uint32_t s = 1; s + len <= count; ++s) {
    if (code[s - 1] != kStringConst || std::memcmp(code + s, kOriginalKey, len) != 0 || code[0] != kJumpIfNot) continue;
    std::memcpy(code + s, kExitKey, len);
    code[0] = kJump;  // the same target: always the error path
    REXLOG_INFO("main_menu_exit: Exit Game hooked to the menu button (OpenMarketplaceBlade, {} bytes)", count);
    return true;
  }
  REXLOG_WARN("main_menu_exit: OpenMarketplaceBlade is not the expected script; Exit Game is off");
  return true;  // looking again will not change it
}

}  // namespace

bool PatchExitButton() { return PatchExitButtonImpl(); }

bool MatchesExitButton(const uint8_t* base, uint32_t addr) {
  if (!addr || !base) return false;
  if (GuestUtf16Is(base, addr, kExitKey)) return true;
  return std::memcmp(base + addr, kExitKey, sizeof(kExitKey)) == 0;  // an 8-bit key
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

void Gow3InstallMainMenuExitIfRequested() {
  if (!REXCVAR_GET(gow_main_menu_exit)) return;
  // Off the game thread: an object scan on it froze the games.
  std::thread([] {
    while (!rex::glue::PatchExitButton()) std::this_thread::sleep_for(std::chrono::seconds(3));
  }).detach();
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
  if (REXCVAR_GET(gow_main_menu_exit) && rex::glue::MatchesExitButton(base, ctx.r5.u32)) {
    rex::glue::ExitGame();
  }
  GOW3_NEED_LIVE_IMP(ctx, base);
}
