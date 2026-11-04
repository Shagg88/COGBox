// Layer 0 (game-free): what the achievements browser shows (docs/achievements/2026-11-04-achievements-browser-design.md)
// - the left column, the header, the rows and every piece of their text - from part 1's data. No drawing, no SDK:
// the in-game page (achievements_browser.cpp) draws it and part 4's launcher page reuses it. Standard C++17 only.
// Our code (MIT).
#pragma once

#include <xenos/achievements_data.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace xenos::ach {

enum class ShowFilter { kAll, kUnlocked, kLocked };
// The X / Y buttons: Unlock time -> Rarity -> Game order -> Gamerscore -> Name (A-Z) -> Unlock time; All -> Unlocked ->
// Locked -> All.
SortOrder NextSort(SortOrder s);
ShowFilter NextShow(ShowFilter f);
const char* SortName(SortOrder s);   // "Unlock time", "Rarity", "Game order", "Gamerscore", "Name (A-Z)"
const char* ShowName(ShowFilter f);  // "All", "Unlocked", "Locked"
const char* GameLabel(uint32_t title_id);  // "Gears 1", "Gears 2", "Gears 3", "Judgment"; "" for another title

// One entry of the left column.
struct BrowserEntry {
  bool all = false;  // the "All" entry
  int game = -1;     // index into the games vector (= kGames) when !all
  bool this_game = false, loaded = false;
  std::string name, line;  // "Gears of War 2" / "All"; "4 / 79 · 40 / 1750 G" or "Not played yet"
  Totals totals;
};
// `games` is LoadAllGames' result (index i = kGames[i]). The running game first (this_game), the others in kGames order,
// then All. A running title that is not one of the four: the four in order, none tagged.
std::vector<BrowserEntry> BuildSidebar(const std::vector<GameAchievements>& games, uint32_t running_title);

struct BrowserHeader {
  std::string title, counts;  // "Gears of War 2" / "All played games"; "4 / 79 achievements · 40 / 1750 G · 5%"
  uint32_t percent = 0;
};
BrowserHeader HeaderFor(const BrowserEntry& e);

struct BrowserRow {
  const Achievement* a = nullptr;  // into `games`: valid while it is unchanged
  std::string name, text, gp, status, game_label, rarity;
  bool locked = false, hidden_secret = false, secret_tag = false;
};
// The entry's rows (All: every loaded game's), filtered and sorted. utc_offset_minutes(unix seconds) gives the PC's
// offset from UTC at that moment (the page passes localtime's).
std::vector<BrowserRow> BuildRows(const std::vector<GameAchievements>& games, const BrowserEntry& e, SortOrder s,
                                  ShowFilter f, const std::function<int(int64_t)>& utc_offset_minutes);
// "4 Nov 2026, 20:10"; "" for 0 (unknown) or a time before 1970.
std::string FormatUnlockTime(uint64_t filetime, int utc_offset_minutes);
// What the list shows when it has no rows for this entry and filter.
std::string EmptyMessage(const BrowserEntry& e, ShowFilter f);
const char* HintLine(bool ps_prompts);

}  // namespace xenos::ach
