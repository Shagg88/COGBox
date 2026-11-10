// SPDX-License-Identifier: BSD-3-Clause
// gow2 - ReXGlue Recompiled Project
//
// Exit Game on the main menu. The Coalesced edit renames "Credits" to "Exit Game".
// When selected, Unreal Engine 3 loads the Credits sequence packages (sp_credits_p
// or ui_scenes_credits). Intercepting this file open allows exiting the game cleanly.
// In TU6, sub_8228AF78 is the core Localize() function. When Credits is selected,
// it looks up the credits entries (Section="credits", Package="Credits", or Key="line_00000").
// Intercepting this provides deterministic exit without relying solely on file-open timing.
//
// Both only fire while the front end is loaded (ue3::InFrontEndForExit: GearStart.xxx is the last
// level package opened). Finishing the campaign plays the same credits from a level, and they play.

#include "main_menu_exit.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <string>
#include <cctype>
#include <filesystem>
#include <string_view>

#include <rex/logging.h>
#include <rex/ppc.h>
#include <ue3/main_menu_exit.h>
#include <ue3/shader_prebuild.h>

namespace {

bool ContainsNoCase(std::string_view s, std::string_view needle) {
  if (needle.empty()) return true;
  if (s.size() < needle.size()) return false;
  auto it = std::search(
      s.begin(), s.end(),
      needle.begin(), needle.end(),
      [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
      });
  return it != s.end();
}

void Gow2OnFileOpenedExit(std::string_view guest_path, const std::filesystem::path* host_path) {
  if (!REXCVAR_GET(gow_main_menu_exit)) return;

  // Checked before the open is noted: SP_Credits_P is a level package, and opening it ends the front end.
  if (ue3::InFrontEndForExit()) {
    const bool match = ContainsNoCase(guest_path, "sp_credits_p") ||
                       ContainsNoCase(guest_path, "ui_scenes_credits") ||
                       (host_path && (ContainsNoCase(host_path->string(), "sp_credits_p") ||
                                      ContainsNoCase(host_path->string(), "ui_scenes_credits")));
    if (match) rex::glue::ExitGame();
  }
  ue3::NoteFileOpenedForExit(guest_path);
}

}  // namespace

namespace rex::glue {

void ExitGame() {
  REXLOG_INFO("MAINMENU: Exit Game selected, ending the process");
  rex::FlushLogging();
#ifdef _WIN32
  TerminateProcess(GetCurrentProcess(), 0);
#else
  _exit(0);
#endif
}

bool GuestUtf16EqualsNoCase(const uint8_t* base, uint32_t addr, std::string_view expected) {
  if (!base || !addr) return false;
  for (size_t i = 0; i < expected.size(); ++i) {
    uint16_t c = static_cast<uint16_t>((base[addr + 2 * i] << 8) | base[addr + 2 * i + 1]);
    if (c > 127) return false;
    if (std::tolower(static_cast<unsigned char>(c)) !=
        std::tolower(static_cast<unsigned char>(expected[i]))) {
      return false;
    }
  }
  uint16_t term = static_cast<uint16_t>((base[addr + 2 * expected.size()] << 8) |
                                        base[addr + 2 * expected.size() + 1]);
  return term == 0;
}

bool GuestAsciiEqualsNoCase(const uint8_t* base, uint32_t addr, std::string_view expected) {
  if (!base || !addr) return false;
  for (size_t i = 0; i < expected.size(); ++i) {
    uint8_t c = base[addr + i];
    if (std::tolower(static_cast<unsigned char>(c)) !=
        std::tolower(static_cast<unsigned char>(expected[i]))) {
      return false;
    }
  }
  return base[addr + expected.size()] == '\0';
}

bool MatchesStringNoCase(const uint8_t* base, uint32_t addr, std::string_view text) {
  if (!base || !addr) return false;
  return GuestUtf16EqualsNoCase(base, addr, text) || GuestAsciiEqualsNoCase(base, addr, text);
}

// Shagg 2026-11-10 10:56: in Gears 2 (TU6) Exit Game played the credits instead of quitting. The credits screen is
// part of the front end already in memory (no SP_Credits_P / ui_scenes_credits file opens: none in that day's logs),
// so only this Localize hook can see it, and it matched "credits" exactly. Now any section / package that CONTAINS
// "credit" counts (the Options menu's own label lookup, section OptionsMenu, still does not), and the lookups that
// mention credits are logged (a few) so a miss shows the real names.
namespace {
std::string GuestText(const uint8_t* base, uint32_t addr) {
  std::string out;
  if (!base || !addr) return out;
  for (uint32_t i = 0; i < 64; ++i) {  // UTF-16 BE, ASCII range only
    const uint16_t c = uint16_t((base[addr + 2 * i] << 8) | base[addr + 2 * i + 1]);
    if (!c) return out;
    if (c < 32 || c > 126) { out.clear(); break; }
    out += char(std::tolower(int(c)));
  }
  for (uint32_t i = 0; i < 64; ++i) {  // 8-bit
    const uint8_t c = base[addr + i];
    if (!c) return out;
    if (c < 32 || c > 126) return std::string();
    out += char(std::tolower(int(c)));
  }
  return std::string();
}
std::atomic<int> g_credit_logs{0};
std::atomic<bool> g_hook_seen{false};
}  // namespace

bool MatchesCredits(const uint8_t* base, const PPCContext& ctx) {
  if (!g_hook_seen.exchange(true)) REXLOG_INFO("main_menu_exit: Localize hook active (TU6 sub_8228AF78)");
  if (!base) return false;
  const std::string section = GuestText(base, ctx.r4.u32), key = GuestText(base, ctx.r5.u32),
                    package = GuestText(base, ctx.r6.u32);
  // The Exit Game row's own label (OptionsMenu / Credits) is looked up when the main menu shows - also when the player
  // comes back from a level, where GearStart.xxx is not opened again: that arms Exit Game (a level disarms it).
  if (section == "optionsmenu" && key == "credits") ue3::ArmFrontEndForExit("Exit Game row shown");
  if (!ue3::InFrontEndForExit()) return false;
  const bool mentions = section.find("credit") != std::string::npos || package.find("credit") != std::string::npos ||
                        key.find("credit") != std::string::npos || key == "line_00000";
  if (!mentions) return false;
  const bool label = section == "optionsmenu";  // the menu row itself ("Exit Game" and its tip)
  if (g_credit_logs.fetch_add(1) < 12)
    REXLOG_INFO("main_menu_exit: Localize section '{}' key '{}' package '{}'{}", section, key, package,
                label ? " (the menu's own label)" : " -> Exit Game");
  return !label && (section.find("credit") != std::string::npos || package.find("credit") != std::string::npos ||
                    key == "line_00000");
}

}  // namespace rex::glue

#if defined(GOW2_TU6) && GOW2_TU6
extern "C" void __imp__sub_8228AF78(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8228AF78(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(gow_main_menu_exit) && rex::glue::MatchesCredits(base, ctx)) {
    rex::glue::ExitGame();
  }
  __imp__sub_8228AF78(ctx, base);
}
#endif

void Gow2InstallMainMenuExitIfRequested() {
  if (!REXCVAR_GET(gow_main_menu_exit)) return;
  Ue3ShaderPrebuildChainFileOpened(&Gow2OnFileOpenedExit);
  REXLOG_INFO("main_menu_exit: file-open hook installed (Exit Game while the front end is loaded)");
}
