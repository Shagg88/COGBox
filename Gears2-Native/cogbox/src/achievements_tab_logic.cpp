// CogBox - the ACHIEVEMENTS tab's small helpers - see achievements_tab_logic.h.
#include "achievements_tab_logic.h"

#include <windows.h>

namespace cogbox {

std::filesystem::path cogbox_path(const std::string &utf8)
{
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &w[0], n);
    return std::filesystem::path(w);
}

std::string status_line(const xenos::ach::Totals &t)
{
    return std::to_string(t.unlocked) + " / " + std::to_string(t.total) + " achievements | " + std::to_string(t.gp_earned) +
           " / " + std::to_string(t.gp_total) + " G | " + std::to_string(xenos::ach::PercentByGp(t)) + "%";
}

const char *button_label(const xenos::ach::BrowserEntry &e)
{
    if (e.all) return "All";
    if (e.game == 3) return "Judgment";   // kGames[3], "Gears of War: Judgment" in full
    return e.game >= 0 && e.game < (int)xenos::ach::kGames.size() ? xenos::ach::kGames[(size_t)e.game].name : "";
}

}  // namespace cogbox
