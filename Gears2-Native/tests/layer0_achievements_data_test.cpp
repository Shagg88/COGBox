// Achievements part 1: the data layer (xenos/achievements_data.h). Our code (MIT).
#include <xenos/achievements_data.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

// Test files go to a folder in the build tree (GN_TEST_SCRATCH_DIR, set by CMake), never under AppData / %TEMP%.
static std::filesystem::path Scratch(const char* name) {
  return std::filesystem::path(GN_TEST_SCRATCH_DIR) / name;
}

TEST_CASE("ach: test scratch folder is in the build tree", "[ach]") {
  const std::string dir = std::filesystem::path(GN_TEST_SCRATCH_DIR).generic_string();
  CHECK(dir.find("AppData") == std::string::npos);
  CHECK(dir.find("gn-test-scratch") != std::string::npos);
}

TEST_CASE("ach: catalog round trip keeps every field", "[ach]") {
  xenos::ach::Catalog c{0x4D53082D, "Gears of War 2", 1, {
      {1, 10, 0x8, 3, "Line\nBreak \\ back", "Описание", ""},
      {2, 50, 0, 4, "숨겨진", "d", "Keep going"}}};
  auto back = xenos::ach::ParseCatalog(xenos::ach::FormatCatalog(c));
  REQUIRE(back);
  CHECK(back->title_id == 0x4D53082D);
  CHECK(back->game == "Gears of War 2");
  CHECK(back->language == 1);
  REQUIRE(back->achievements.size() == 2);
  CHECK(back->achievements[0].label == "Line\nBreak \\ back");
  CHECK(back->achievements[0].description == "Описание");
  CHECK(back->achievements[0].unachieved.empty());
  CHECK(back->achievements[0].flags == 0x8);
  CHECK(back->achievements[0].image_id == 3);
  CHECK(back->achievements[1].label == "숨겨진");
  CHECK(back->achievements[1].gamerscore == 50);
  CHECK(back->achievements[1].unachieved == "Keep going");
}

TEST_CASE("ach: catalog reads CRLF, skips unknown keys and id-less entries", "[ach]") {
  auto c = xenos::ach::ParseCatalog(
      "# COGBox achievement catalog 1\r\ntitle=4D53082D\r\nfuture=x\r\n"
      "[achievement]\r\nlabel=no id\r\n[achievement]\r\nid=0\r\n"
      "[achievement]\r\nid=7\r\nlabel=Seven\r\nnew_key=1\r\n");
  REQUIRE(c);
  REQUIRE(c->achievements.size() == 1);
  CHECK(c->achievements[0].id == 7);
  CHECK(c->achievements[0].label == "Seven");
}

TEST_CASE("ach: catalog without header or title is not loaded", "[ach]") {
  CHECK_FALSE(xenos::ach::ParseCatalog("title=4D53082D\n"));
  CHECK_FALSE(xenos::ach::ParseCatalog("# COGBox achievement catalog 1\n[achievement]\nid=1\n"));
  CHECK_FALSE(xenos::ach::ParseCatalog(std::string("\0\xff garbage", 10)));
}

TEST_CASE("ach: paths and game table", "[ach]") {
  const std::filesystem::path root = "C:/COGBox";
  CHECK(xenos::ach::CatalogPath(root, 0x4D530A26) == root / "achievements" / "4D530A26" / "catalog.txt");
  CHECK(xenos::ach::IconPath(root, 0x4D530A26, 12) == root / "achievements" / "4D530A26" / "icons" / "12.png");
  REQUIRE(xenos::ach::FindGame(0x4D5308AB) != nullptr);
  CHECK(xenos::ach::UnlockPath(root, *xenos::ach::FindGame(0x4D5308AB)) ==
        root / "Gears3" / "user" / "achievements" / "4D5308AB.toml");
  CHECK(xenos::ach::RevealedPath(root) == root / "achievements" / "revealed.txt");
  CHECK(xenos::ach::FindGame(0x12345678) == nullptr);
  CHECK(xenos::ach::kGames[0].title_id == 0x4D5307D5);
  CHECK(std::string(xenos::ach::kGames[3].folder) == "Judgment");
}

TEST_CASE("ach: atomic write then load", "[ach]") {
  const auto dir = Scratch("ach1");
  std::filesystem::remove_all(dir);
  xenos::ach::Catalog c{0x4D5307D5, "Gears of War", 1, {{5, 20, 0x8, 1, "L", "D", "U"}}};
  REQUIRE(xenos::ach::WriteFileAtomic(xenos::ach::CatalogPath(dir, c.title_id), xenos::ach::FormatCatalog(c)));
  CHECK_FALSE(std::filesystem::exists(xenos::ach::CatalogPath(dir, c.title_id).string() + ".tmp"));
  auto back = xenos::ach::LoadCatalog(dir, 0x4D5307D5);
  REQUIRE(back);
  CHECK(back->achievements[0].unachieved == "U");
  CHECK_FALSE(xenos::ach::LoadCatalog(dir, 0x4D53082D));
  std::filesystem::remove_all(dir);
}

TEST_CASE("ach: unlocks in the SDK's current format", "[ach]") {
  auto u = xenos::ach::ParseUnlocks(
      "# Achievement unlock state - managed by ReXGlue runtime\n\n"
      "[unlocked.\"3\"]\nfiletime = 133500000000000000\n\n"
      "[unlocked.\"12\"]\r\nfiletime = 133600000000000000\r\n");
  REQUIRE(u.size() == 2);
  CHECK(u.at(3) == 133500000000000000ull);
  CHECK(u.at(12) == 133600000000000000ull);
}

TEST_CASE("ach: unlocks in the old array format have no time", "[ach]") {
  auto u = xenos::ach::ParseUnlocks("unlocked = [1, 2, 9]\n");
  REQUIRE(u.size() == 3);
  CHECK(u.at(9) == 0);
}

TEST_CASE("ach: a damaged unlock file gives no unlocks and no crash", "[ach]") {
  CHECK(xenos::ach::ParseUnlocks("[unlocked.\"abc\"]\nfiletime = x\n\xff\xfe").empty());
  auto u = xenos::ach::ParseUnlocks("[unlocked.\"4\"]\n");  // header only: unlocked, time unknown
  REQUIRE(u.size() == 1);
  CHECK(u.at(4) == 0);
}

TEST_CASE("ach: revealed list round trip and junk lines", "[ach]") {
  const auto dir = Scratch("ach2");
  std::filesystem::remove_all(dir);
  REQUIRE(xenos::ach::AddRevealed(dir, 0x4D53082D, 15));
  REQUIRE(xenos::ach::AddRevealed(dir, 0x4D53082D, 15));  // twice: still one entry
  auto r = xenos::ach::LoadRevealed(dir);
  CHECK(r.size() == 1);
  CHECK(r.count({0x4D53082D, 15}) == 1);
  auto p = xenos::ach::ParseRevealed("\n4d53082d:7\njunk\n4D5308AB:\n:5\n4D5308AB:2\r\n");
  CHECK(p.size() == 2);
  CHECK(p.count({0x4D53082D, 7}) == 1);
  CHECK(p.count({0x4D5308AB, 2}) == 1);
  std::filesystem::remove_all(dir);
}

namespace {
xenos::ach::Catalog ThreeAchievements() {
  return {0x4D53082D, "Gears of War 2", 1,
          {{1, 10, 0x8, 1, "A", "", ""}, {2, 30, 0, 2, "B", "", ""}, {3, 60, 0x8, 3, "C", "", ""}}};
}
struct FixedRarity : xenos::ach::RarityProvider {
  xenos::ach::Rarity Get(uint32_t, uint32_t id) override {
    if (id == 3) return {true, 2.5f};
    if (id == 1) return {true, 40.f};
    return {};
  }
};
}  // namespace

TEST_CASE("ach: join, secrets and totals", "[ach]") {
  xenos::ach::NoRarity none;
  const auto& g2 = *xenos::ach::FindGame(0x4D53082D);
  auto g = xenos::ach::BuildGame(g2, ThreeAchievements(), {{2, 100}, {99, 5}}, {{0x4D53082D, 3}}, none, "C:/none");
  REQUIRE(g.loaded);
  REQUIRE(g.achievements.size() == 3);  // the unlock of unknown id 99 is ignored
  CHECK(g.achievements[1].unlocked);
  CHECK(g.achievements[1].secret);  // flags 0: secret
  CHECK_FALSE(g.achievements[0].secret);
  CHECK(g.achievements[2].revealed);
  CHECK_FALSE(g.achievements[0].revealed);
  CHECK(g.achievements[1].unlocked_at == 100);
  CHECK(g.achievements[2].list_order == 2);
  CHECK(g.achievements[2].game_index == 1);
  CHECK(g.achievements[0].icon.empty());  // no PNG on disk
  CHECK(g.totals.unlocked == 1);
  CHECK(g.totals.total == 3);
  CHECK(g.totals.gp_earned == 30);
  CHECK(g.totals.gp_total == 100);
  CHECK(xenos::ach::PercentByGp(g.totals) == 30);
  CHECK(xenos::ach::PercentByGp({}) == 0);
}

TEST_CASE("ach: all-games totals count loaded games only", "[ach]") {
  xenos::ach::NoRarity none;
  std::vector<xenos::ach::GameAchievements> games{
      xenos::ach::BuildGame(xenos::ach::kGames[1], ThreeAchievements(), {{3, 1}}, {}, none, "C:/none"),
      xenos::ach::BuildGame(xenos::ach::kGames[2], std::nullopt, {{1, 1}}, {}, none, "C:/none")};
  CHECK_FALSE(games[1].loaded);
  CHECK(games[1].achievements.empty());
  auto t = xenos::ach::SumTotals(games);
  CHECK(t.total == 3);
  CHECK(t.unlocked == 1);
  CHECK(t.gp_earned == 60);
  CHECK(t.gp_total == 100);
}

TEST_CASE("ach: sort by unlock time", "[ach]") {
  xenos::ach::NoRarity none;
  auto g = xenos::ach::BuildGame(xenos::ach::kGames[1], ThreeAchievements(), {{1, 0}, {3, 500}, {2, 500}}, {}, none,
                                 "C:/x");
  xenos::ach::Achievement locked = g.achievements[0];
  locked.unlocked = false;
  locked.id = 4;
  locked.list_order = 3;
  g.achievements.insert(g.achievements.begin(), locked);
  xenos::ach::SortAchievements(g.achievements, xenos::ach::SortOrder::kUnlockTime);
  // same time: list order; unknown time (0) after timed; locked last
  CHECK(g.achievements[0].id == 2);
  CHECK(g.achievements[1].id == 3);
  CHECK(g.achievements[2].id == 1);
  CHECK(g.achievements[3].id == 4);
}

TEST_CASE("ach: sort by rarity", "[ach]") {
  xenos::ach::NoRarity none;
  FixedRarity fixed;
  auto a = xenos::ach::BuildGame(xenos::ach::kGames[1], ThreeAchievements(), {{1, 300}, {2, 200}, {3, 100}}, {},
                                 none, "C:/x");
  auto b = a.achievements;
  xenos::ach::SortAchievements(a.achievements, xenos::ach::SortOrder::kRarity);
  xenos::ach::SortAchievements(b, xenos::ach::SortOrder::kUnlockTime);
  for (size_t i = 0; i < b.size(); ++i) CHECK(a.achievements[i].id == b[i].id);  // NoRarity: same as time
  auto r = xenos::ach::BuildGame(xenos::ach::kGames[1], ThreeAchievements(), {{1, 300}, {2, 200}, {3, 100}}, {},
                                 fixed, "C:/x");
  xenos::ach::SortAchievements(r.achievements, xenos::ach::SortOrder::kRarity);
  CHECK(r.achievements[0].id == 3);
  CHECK(r.achievements[1].id == 1);
  CHECK(r.achievements[2].id == 2);
}

TEST_CASE("ach: rarity provider defaults to none", "[ach]") {
  CHECK_FALSE(xenos::ach::CurrentRarityProvider().Get(1, 1).known);
  FixedRarity fixed;
  xenos::ach::SetRarityProvider(&fixed);
  CHECK(xenos::ach::CurrentRarityProvider().Get(1, 3).known);
  xenos::ach::SetRarityProvider(nullptr);
  CHECK_FALSE(xenos::ach::CurrentRarityProvider().Get(1, 3).known);
}

TEST_CASE("ach: unlocks as the SDK writes them (unquoted ids)", "[ach]") {
  auto u = xenos::ach::ParseUnlocks("[unlocked.3]\nfiletime = 133500000000000000\n[unlocked.12]\nfiletime = 7\n");
  REQUIRE(u.size() == 2);
  CHECK(u.at(3) == 133500000000000000ull);
  CHECK(u.at(12) == 7);
}

TEST_CASE("ach: an atomic write replaces an existing file and leaves no temp file", "[ach]") {
  const auto dir = Scratch("ach3");
  std::filesystem::remove_all(dir);
  const auto file = dir / "revealed.txt";
  REQUIRE(xenos::ach::WriteFileAtomic(file, "old\n"));
  REQUIRE(xenos::ach::WriteFileAtomic(file, "new\n"));
  std::string text;
  REQUIRE(xenos::ach::ReadFile(file, &text));
  CHECK(text == "new\n");
  CHECK_FALSE(std::filesystem::exists(file.string() + ".tmp"));
  std::filesystem::remove_all(dir);
}
