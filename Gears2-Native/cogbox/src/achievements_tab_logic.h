// CogBox - the ACHIEVEMENTS tab's small helpers (achievements part 4, docs/achievements/2026-11-04-achievements-launcher-
// design.md). No SDL, no Dear ImGui: test/achievements_tab_test.cpp checks them. The tab itself is achievements_tab.cpp.
#pragma once

#include <xenos/achievements_browser_model.h>

#include <filesystem>
#include <string>

namespace cogbox {

// A UTF-8 path (the launcher keeps every path as UTF-8) as a std::filesystem::path. Never from the narrow string
// directly: MinGW would read it in the ANSI code page and a non-ASCII folder name would not be found.
std::filesystem::path cogbox_path(const std::string &utf8);
// "<unlocked> / <total> achievements | <earned> / <total> G | <percent>%"
std::string status_line(const xenos::ach::Totals &t);
// A game button's label: "Gears of War", "Gears of War 2", "Gears of War 3", "Judgment" (short, so the row fits), "All".
const char *button_label(const xenos::ach::BrowserEntry &e);

}  // namespace cogbox
