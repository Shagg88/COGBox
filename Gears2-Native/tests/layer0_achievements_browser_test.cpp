// Achievements part 3: the browser model (xenos/achievements_browser_model.h). Our code (MIT).
#include <xenos/achievements_browser_model.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace xenos::ach;

namespace {
constexpr uint64_t kNov4_2010Utc = 134382966000000000ull;  // 2026-11-04 20:10:00 UTC as a FILETIME
const std::string kDot = "\xC2\xB7";                         // U+00B7

Achievement Make(uint32_t game, uint32_t id, uint32_t gp, bool unlocked, bool secret = false) {
  Achievement a;
  a.title_id = kGames[game].title_id;
  a.id = id;
  a.gamerscore = gp;
  a.label = "Name " + std::to_string(id);
  a.description = "Done " + std::to_string(id);
  a.unachieved = "How to " + std::to_string(id);
  a.unlocked = unlocked;
  a.unlocked_at = unlocked ? kNov4_2010Utc - id * 10'000'000ull * 60 : 0;
  a.secret = secret;
  a.game_index = game;
  a.list_order = id;
  return a;
}

// Gears 1 loaded (3: one unlocked, one locked, one locked secret), Gears 2 loaded (2: one unlocked), Gears 3 and
// Judgment not loaded.
std::vector<GameAchievements> Games() {
  std::vector<GameAchievements> g(4);
  for (size_t i = 0; i < 4; ++i) g[i].game = &kGames[i];
  g[0].loaded = true;
  g[0].achievements = {Make(0, 1, 10, true), Make(0, 2, 20, false), Make(0, 3, 30, false, true)};
  g[0].totals = {1, 3, 10, 60};
  g[1].loaded = true;
  g[1].achievements = {Make(1, 1, 10, true), Make(1, 2, 20, false)};
  g[1].totals = {1, 2, 10, 30};
  return g;
}

int Utc(int64_t) { return 0; }

const BrowserEntry& Find(const std::vector<BrowserEntry>& s, const std::string& name) {
  for (const auto& e : s)
    if (e.name == name) return e;
  FAIL("no entry " << name);
  return s.front();
}
}  // namespace

TEST_CASE("ach: BuildSidebar: running game first and tagged, others in order, All last", "[ach]") {
  const auto s = BuildSidebar(Games(), 0x4D53082D);
  REQUIRE(s.size() == 5);
  CHECK(s[0].name == "Gears of War 2");
  CHECK(s[0].this_game);
  CHECK(s[1].name == "Gears of War");
  CHECK(s[2].name == "Gears of War 3");
  CHECK(s[3].name == "Gears of War: Judgment");
  CHECK(s[4].name == "All");
  CHECK(s[4].all);
  CHECK_FALSE(s[1].this_game);
  CHECK(s[3].line == "Not played yet");
  CHECK_FALSE(s[3].loaded);
  CHECK(s[0].line == "1 / 2 " + kDot + " 10 / 30 G");
  CHECK(s[0].game == 1);
}

TEST_CASE("ach: BuildSidebar: unknown running title", "[ach]") {
  const auto s = BuildSidebar(Games(), 0);
  REQUIRE(s.size() == 5);
  CHECK(s[0].name == "Gears of War");
  CHECK(s[1].name == "Gears of War 2");
  CHECK(s[2].name == "Gears of War 3");
  CHECK(s[3].name == "Gears of War: Judgment");
  CHECK(s[4].name == "All");
  for (const auto& e : s) CHECK_FALSE(e.this_game);
}

TEST_CASE("ach: HeaderFor All uses loaded games only", "[ach]") {
  const auto s = BuildSidebar(Games(), 0x4D53082D);
  const BrowserEntry& all = Find(s, "All");
  CHECK(all.totals.unlocked == 2);
  CHECK(all.totals.total == 5);
  CHECK(all.totals.gp_earned == 20);
  CHECK(all.totals.gp_total == 90);
  CHECK(all.line == "2 / 5 " + kDot + " 20 / 90 G");
  const BrowserHeader h = HeaderFor(all);
  CHECK(h.title == "All played games");
  CHECK(h.percent == 22);
  CHECK(h.counts == "2 / 5 achievements " + kDot + " 20 / 90 G " + kDot + " 22%");
  CHECK(HeaderFor(s[0]).title == "Gears of War 2");
}

TEST_CASE("ach: BuildRows All: only loaded games, each with its label", "[ach]") {
  const auto games = Games();
  const auto s = BuildSidebar(games, 0x4D53082D);
  const auto rows = BuildRows(games, Find(s, "All"), SortOrder::kGameOrder, ShowFilter::kAll, Utc);
  REQUIRE(rows.size() == 5);
  CHECK(rows[0].game_label == "Gears 1");
  CHECK(rows[3].game_label == "Gears 2");
  for (const auto& r : rows) CHECK_FALSE(r.game_label.empty());
  const auto one = BuildRows(games, s[0], SortOrder::kGameOrder, ShowFilter::kAll, Utc);
  for (const auto& r : one) CHECK(r.game_label.empty());  // a single game: no labels
}

TEST_CASE("ach: BuildRows: hidden secret", "[ach]") {
  const auto games = Games();
  const auto s = BuildSidebar(games, 0x4D5307D5);
  const auto rows = BuildRows(games, s[0], SortOrder::kGameOrder, ShowFilter::kAll, Utc);
  REQUIRE(rows.size() == 3);
  const BrowserRow& r = rows[2];
  CHECK(r.hidden_secret);
  CHECK(r.locked);
  CHECK(r.name == "Secret achievement");
  CHECK(r.text == "Select to reveal");
  CHECK(r.gp == "30 G");
  CHECK(r.status == "Locked");
  CHECK_FALSE(r.secret_tag);
  CHECK(r.a->id == 3);
}

TEST_CASE("ach: BuildRows: unlocked or revealed secret is not hidden", "[ach]") {
  for (int which = 0; which < 2; ++which) {
    auto games = Games();
    Achievement& a = games[0].achievements[2];
    if (which == 0) {
      a.unlocked = true;
      a.unlocked_at = kNov4_2010Utc;
    } else {
      a.revealed = true;
    }
    const auto rows = BuildRows(games, BuildSidebar(games, 0x4D5307D5)[0], SortOrder::kGameOrder, ShowFilter::kAll, Utc);
    const BrowserRow& r = rows[2];
    CHECK_FALSE(r.hidden_secret);
    CHECK(r.secret_tag);
    CHECK(r.name == "Name 3");
    CHECK(r.text == (which == 0 ? "Done 3" : "How to 3"));
  }
}

TEST_CASE("ach: BuildRows: locked rows use the unachieved text", "[ach]") {
  const auto games = Games();
  const auto rows = BuildRows(games, BuildSidebar(games, 0x4D5307D5)[0], SortOrder::kGameOrder, ShowFilter::kAll, Utc);
  CHECK(rows[0].text == "Done 1");
  CHECK_FALSE(rows[0].locked);
  CHECK(rows[0].status == "Unlocked 4 Nov 2026, 20:09");
  CHECK(rows[0].gp == "10 G");
  CHECK(rows[1].text == "How to 2");
  CHECK(rows[1].locked);
  CHECK(rows[1].status == "Locked");
}

TEST_CASE("ach: BuildRows: Show filter", "[ach]") {
  const auto games = Games();
  const auto all = Find(BuildSidebar(games, 0), "All");
  const auto u = BuildRows(games, all, SortOrder::kGameOrder, ShowFilter::kUnlocked, Utc);
  REQUIRE(u.size() == 2);
  for (const auto& r : u) CHECK_FALSE(r.locked);
  const auto l = BuildRows(games, all, SortOrder::kGameOrder, ShowFilter::kLocked, Utc);
  REQUIRE(l.size() == 3);
  for (const auto& r : l) CHECK(r.locked);
}

TEST_CASE("ach: BuildRows: sort order is applied", "[ach]") {
  const auto games = Games();
  const auto all = Find(BuildSidebar(games, 0), "All");
  const auto g = BuildRows(games, all, SortOrder::kGamerscore, ShowFilter::kAll, Utc);
  REQUIRE(g.size() == 5);
  CHECK(g[0].a->gamerscore == 30);
  const auto t = BuildRows(games, all, SortOrder::kUnlockTime, ShowFilter::kAll, Utc);
  CHECK_FALSE(t[0].locked);
  CHECK_FALSE(t[1].locked);
  CHECK(t[2].locked);
}

TEST_CASE("ach: FormatUnlockTime", "[ach]") {
  CHECK(FormatUnlockTime(kNov4_2010Utc, 0) == "4 Nov 2026, 20:10");
  CHECK(FormatUnlockTime(kNov4_2010Utc, 60) == "4 Nov 2026, 21:10");
  CHECK(FormatUnlockTime(kNov4_2010Utc, -1290) == "3 Nov 2026, 22:40");
  CHECK(FormatUnlockTime(0, 0).empty());
  CHECK(FormatUnlockTime(130464000000000ull, 0).empty());  // 1601-06-01
  CHECK(FormatUnlockTime(116444736000000000ull + 10'000'000ull * 86400 * 59, 0) == "1 Mar 1970, 00:00");
}

TEST_CASE("ach: BuildRows: unknown unlock time", "[ach]") {
  auto games = Games();
  games[0].achievements[0].unlocked_at = 0;
  const auto rows = BuildRows(games, BuildSidebar(games, 0x4D5307D5)[0], SortOrder::kGameOrder, ShowFilter::kAll, Utc);
  CHECK(rows[0].status == "Unlocked");
}

TEST_CASE("ach: BuildRows: rarity text only when rare", "[ach]") {
  auto games = Games();
  games[0].achievements[0].rarity = {true, 4.26f};
  games[0].achievements[1].rarity = {true, 30.f};
  const auto rows = BuildRows(games, BuildSidebar(games, 0x4D5307D5)[0], SortOrder::kGameOrder, ShowFilter::kAll, Utc);
  CHECK(rows[0].rarity == "Rare " + kDot + " 4.3%");
  CHECK(rows[1].rarity.empty());
  CHECK(rows[2].rarity.empty());
}

TEST_CASE("ach: EmptyMessage", "[ach]") {
  const auto s = BuildSidebar(Games(), 0x4D53082D);
  CHECK(EmptyMessage(Find(s, "Gears of War: Judgment"), ShowFilter::kAll) ==
        "Start Gears of War: Judgment once from COGBox to load its achievements.");
  CHECK(EmptyMessage(s[0], ShowFilter::kUnlocked) == "No unlocked achievements yet");
  CHECK(EmptyMessage(s[0], ShowFilter::kLocked) == "Every achievement is unlocked!");
}

TEST_CASE("ach: NextSort / NextShow cycle", "[ach]") {
  SortOrder s = SortOrder::kUnlockTime;
  const SortOrder want[] = {SortOrder::kRarity, SortOrder::kGameOrder, SortOrder::kGamerscore, SortOrder::kName,
                            SortOrder::kUnlockTime};
  for (SortOrder w : want) CHECK((s = NextSort(s)) == w);
  CHECK(std::string(SortName(SortOrder::kName)) == "Name (A-Z)");
  CHECK(std::string(SortName(SortOrder::kUnlockTime)) == "Unlock time");
  CHECK(NextShow(ShowFilter::kAll) == ShowFilter::kUnlocked);
  CHECK(NextShow(ShowFilter::kUnlocked) == ShowFilter::kLocked);
  CHECK(NextShow(ShowFilter::kLocked) == ShowFilter::kAll);
  CHECK(std::string(ShowName(ShowFilter::kLocked)) == "Locked");
  CHECK(std::string(GameLabel(0x4D530A26)) == "Judgment");
  CHECK(std::string(GameLabel(0x12345678)).empty());
}

TEST_CASE("ach: HintLine", "[ach]") {
  CHECK(std::string(HintLine(false)) == "LB / RB: game   A: select   X: sort   Y: show   B: back");
  CHECK(std::string(HintLine(true)) == "L1 / R1: game   Cross: select   Square: sort   Triangle: show   Circle: back");
}
