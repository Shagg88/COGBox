// Achievements part 1: the data layer (xenos/achievements_data.h). Our code (MIT).
#include <xenos/achievements_data.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

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
  const auto dir = std::filesystem::temp_directory_path() / "cogbox_ach_test1";
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
  const auto dir = std::filesystem::temp_directory_path() / "cogbox_ach_test2";
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
