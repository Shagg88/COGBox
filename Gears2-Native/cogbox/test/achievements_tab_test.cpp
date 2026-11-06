// CogBox - test of the ACHIEVEMENTS tab's helpers (src/achievements_tab_logic.h). Plain checks, like settings_test.
// Build (llvm-mingw):
//   x86_64-w64-mingw32-g++ -std=c++17 -DUNICODE -D_UNICODE -DNOMINMAX -Isrc -I../src/layer0_xenos/include
//     test/achievements_tab_test.cpp src/achievements_tab_logic.cpp ../src/layer0_xenos/achievements_data.cpp
//     ../src/layer0_xenos/achievements_browser_model.cpp ../src/layer0_xenos/achievements_popup_logic.cpp
// Run it from any folder: it writes only under test-scratch\ next to the exe.
#include "../src/achievements_tab_logic.h"

#include <xenos/achievements_browser_model.h>

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace ach = xenos::ach;

static int fails = 0;
#define CHECK(x) do { if (!(x)) { std::cout << "FAIL " << __LINE__ << ": " #x "\n"; ++fails; } } while (0)

int main() {
    CHECK(cogbox::status_line({2, 5, 20, 90}) == "2 / 5 achievements | 20 / 90 G | 22%");
    CHECK(cogbox::status_line({0, 0, 0, 0}) == "0 / 0 achievements | 0 / 0 G | 0%");

    std::vector<ach::GameAchievements> games(4);
    for (size_t i = 0; i < 4; ++i) games[i].game = &ach::kGames[i];
    const auto side = ach::BuildSidebar(games, 0x4D53082D);
    CHECK(std::string(cogbox::button_label(side[0])) == "Gears of War 2");
    CHECK(std::string(cogbox::button_label(side[1])) == "Gears of War");
    CHECK(std::string(cogbox::button_label(side[3])) == "Judgment");
    CHECK(std::string(cogbox::button_label(side[4])) == "All");

    // a COGBox folder with a non-ASCII name ("Usér") still loads its files
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::filesystem::path scratch = std::filesystem::path(exe).parent_path() / L"test-scratch";
    std::filesystem::remove_all(scratch);
    const std::string utf8 = scratch.u8string() + "\\Us\xC3\xA9r\\COGBox";
    const std::filesystem::path dir = cogbox::cogbox_path(utf8);
    CHECK(dir.u8string() == utf8);
    ach::Catalog c;
    c.title_id = 0x4D53082D;
    c.game = "Gears of War 2";
    c.achievements.push_back({1, 10, 0x8, 3, "Green as Grass", "Showed Carmine the ropes", "Train the rook"});
    CHECK(ach::WriteFileAtomic(ach::CatalogPath(dir, c.title_id), ach::FormatCatalog(c)));
    const auto loaded = ach::LoadAllGames(dir, ach::CurrentRarityProvider());
    CHECK(loaded.size() == 4 && loaded[1].loaded && loaded[1].achievements.size() == 1);
    CHECK(ach::AddRevealed(dir, 0x4D53082D, 1));
    CHECK(ach::LoadRevealed(dir).count({0x4D53082D, 1u}) == 1);
    std::filesystem::remove_all(scratch);

    std::cout << (fails ? "TESTS FAILED\n" : "all ok\n");
    return fails ? 1 : 0;
}
