// SPDX-License-Identifier: BSD-3-Clause
// gow2 - ReXGlue Recompiled Project
//
// Exit Game on the main menu. The Coalesced edit renames "Credits" to "Exit Game".
// When selected, Unreal Engine 3 loads the Credits sequence packages (sp_credits_p
// or ui_scenes_credits). Intercepting this file open allows exiting the game cleanly.

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
#include <ue3/shader_prebuild.h>

REXCVAR_DEFINE_BOOL(gow2_main_menu_exit, true, "ui",
                    "Add Exit Game to the main menu (replaces Credits); restart");

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

}  // namespace rex::glue

namespace {

std::atomic<bool> g_ready_to_exit{false};

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
  if (!REXCVAR_GET(gow2_main_menu_exit)) return;
  if (!g_ready_to_exit.load(std::memory_order_relaxed)) return;

  const bool match = ContainsNoCase(guest_path, "sp_credits_p") ||
                     ContainsNoCase(guest_path, "ui_scenes_credits") ||
                     (host_path && (ContainsNoCase(host_path->string(), "sp_credits_p") ||
                                    ContainsNoCase(host_path->string(), "ui_scenes_credits")));
  if (match) {
    rex::glue::ExitGame();
  }
}

}  // namespace

void Gow2NotifyReadyToExit() {
  g_ready_to_exit.store(true, std::memory_order_relaxed);
}

void Gow2InstallMainMenuExitIfRequested() {
  if (!REXCVAR_GET(gow2_main_menu_exit)) return;
  Ue3ShaderPrebuildChainFileOpened(&Gow2OnFileOpenedExit);
  REXLOG_INFO("gow2_main_menu_exit: installed file-open hook for main menu Exit Game");
}
