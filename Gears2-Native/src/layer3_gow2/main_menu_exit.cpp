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
// The options menu has no script for this entry, so the hook is the entry itself: the game looks up its label
// (section "optionsmenu", key "credits" or "credits_desc") when that menu is built. Credits only end the game if that
// happened since the last level was opened, so finishing the campaign plays them, wherever the player has been before.

#include "main_menu_exit.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
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

std::atomic<bool> g_exit_entry_shown{false};  // the options menu with Exit Game was built, and no level opened since

// A level's persistent package: *_P.xxx (campaign) or MP_*.xxx (versus and Horde maps, not their _LOC_ files).
bool IsLevelPackage(std::string_view path) {
  const size_t slash = path.find_last_of("\\/");
  const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
  if (!ContainsNoCase(name, ".xxx") || ContainsNoCase(name, "_loc_") || ContainsNoCase(name, "geargame_p")) return false;
  if (ContainsNoCase(name, "_p.xxx")) return true;
  return name.size() > 3 && std::tolower(static_cast<unsigned char>(name[0])) == 'm' &&
         std::tolower(static_cast<unsigned char>(name[1])) == 'p' && name[2] == '_';
}

void Gow2OnFileOpenedExit(std::string_view guest_path, const std::filesystem::path* host_path) {
  if (!REXCVAR_GET(gow_main_menu_exit)) return;

  // Checked before the level is noted: SP_Credits_P is a level package.
  if (g_exit_entry_shown.load()) {
    const bool match = ContainsNoCase(guest_path, "sp_credits_p") ||
                       ContainsNoCase(guest_path, "ui_scenes_credits") ||
                       (host_path && (ContainsNoCase(host_path->string(), "sp_credits_p") ||
                                      ContainsNoCase(host_path->string(), "ui_scenes_credits")));
    if (match) rex::glue::ExitGame();
  }
  if (IsLevelPackage(guest_path) && g_exit_entry_shown.exchange(false)) {
    REXLOG_INFO("main_menu_exit: level {} - Exit Game entry no longer on screen", guest_path);
  }
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

bool MatchesCredits(const uint8_t* base, const PPCContext& ctx) {
  if (!base) {
    return false;
  }
  if (MatchesStringNoCase(base, ctx.r4.u32, "optionsmenu")) {
    if ((MatchesStringNoCase(base, ctx.r5.u32, "credits") || MatchesStringNoCase(base, ctx.r5.u32, "credits_desc")) &&
        !g_exit_entry_shown.exchange(true)) {
      REXLOG_INFO("main_menu_exit: the options menu's Exit Game entry was built");
    }
    return false;
  }
  if (!g_exit_entry_shown.load()) {
    return false;
  }
  if (MatchesStringNoCase(base, ctx.r4.u32, "credits") ||
      MatchesStringNoCase(base, ctx.r6.u32, "credits") ||
      MatchesStringNoCase(base, ctx.r5.u32, "line_00000")) {
    return true;
  }
  return false;
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
