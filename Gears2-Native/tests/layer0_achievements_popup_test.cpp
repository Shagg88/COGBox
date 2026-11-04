// Achievements part 2: the popup rules and timeline (xenos/achievements_popup_logic.h). Our code (MIT).
#include <xenos/achievements_popup_logic.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("ach popup: grade from GP and the Gears 1 exceptions", "[ach]") {
  using xenos::ach::GradeFor;
  using G = xenos::ach::TrophyGrade;
  for (uint32_t gp : {0u, 10u, 20u, 30u}) CHECK(GradeFor(0x4D53082D, 1, gp) == G::Bronze);
  for (uint32_t gp : {31u, 40u, 50u, 74u}) CHECK(GradeFor(0x4D53082D, 1, gp) == G::Silver);
  for (uint32_t gp : {75u, 100u, 150u}) CHECK(GradeFor(0x4D53082D, 1, gp) == G::Gold);
  CHECK(GradeFor(0x4D5307D5, 13, 20) == G::Gold);
  CHECK(GradeFor(0x4D5307D5, 19, 30) == G::Gold);
  CHECK(GradeFor(0x4D5308AB, 19, 30) == G::Bronze);
}

TEST_CASE("ach popup: rare below ten percent only", "[ach]") {
  CHECK_FALSE(xenos::ach::IsRare({}));
  CHECK(xenos::ach::IsRare({true, 9.9f}));
  CHECK_FALSE(xenos::ach::IsRare({true, 10.f}));
  CHECK_FALSE(xenos::ach::IsRare({true, 40.f}));
}

TEST_CASE("ach popup: completes the game", "[ach]") {
  using xenos::ach::CompletesGame;
  CHECK(CompletesGame({{1, true}, {2, true}, {3, true}}, 3));
  CHECK_FALSE(CompletesGame({{1, true}, {2, false}, {3, true}}, 3));
  CHECK_FALSE(CompletesGame({{1, true}, {2, true}}, 9));
  CHECK_FALSE(CompletesGame({}, 1));
}

TEST_CASE("ach popup: settings names", "[ach]") {
  using namespace xenos::ach;
  CHECK(ParseStyle("ps5") == PopupStyle::Ps5);
  CHECK(ParseStyle("xbox") == PopupStyle::Xbox);
  CHECK(ParseStyle("nonsense") == PopupStyle::X360);
  CHECK(std::string(ParseColour("TEAL").name) == "Teal");
  CHECK(std::string(ParseColour("??").name) == "Dark");
  CHECK(kColourPresets[1].fill.r == 0x9E);
  CHECK(kColourPresets[0].border.r == 0x3C);
  CHECK(ParsePosition("top_right") == PopupPosition::TopRight);
  CHECK(ParsePosition("x") == PopupPosition::Default);
  CHECK(ResolvePosition(PopupStyle::Ps5, PopupPosition::Default) == PopupPosition::TopRight);
  CHECK(ResolvePosition(PopupStyle::X360, PopupPosition::Default) == PopupPosition::BottomCentre);
  CHECK(ResolvePosition(PopupStyle::Xbox, PopupPosition::Default) == PopupPosition::BottomCentre);
  CHECK(ResolvePosition(PopupStyle::Ps5, PopupPosition::BottomLeft) == PopupPosition::BottomLeft);
}

TEST_CASE("ach popup: text fitting", "[ach]") {
  auto measure = [](const std::string& s) { return float(s.size()) * 10.f; };  // 10 px per byte
  float scale = 0;
  CHECK(xenos::ach::FitText("Short", 100, measure, &scale) == "Short");
  CHECK(scale == 1.f);
  CHECK(xenos::ach::FitText("0123456789A", 100, measure, &scale) == "0123456789A");  // 110 * 0.9 fits
  CHECK(scale == 0.9f);
  std::string cut = xenos::ach::FitText("A very long achievement name", 100, measure, &scale);
  REQUIRE(cut.size() >= 3);
  CHECK(cut.substr(cut.size() - 3) == "\xE2\x80\xA6");  // ends with the ellipsis
  CHECK(measure(cut) * scale <= 100.f);
  std::string ru = xenos::ach::FitText(
      "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\xD1\x81\xD1\x82\xD0\xB2\xD0\xB8\xD0\xB5", 60, measure, &scale);
  CHECK(ru.size() > 3);
  CHECK((ru.size() - 3) % 2 == 0);  // whole 2-byte Cyrillic letters kept before the ellipsis
}

TEST_CASE("ach popup: timeline starts and ends invisible, holds fully shown", "[ach]") {
  using namespace xenos::ach;
  for (PopupStyle s : {PopupStyle::X360, PopupStyle::Xbox, PopupStyle::Ps5}) {
    CHECK(PopupTimeline(s, 0.f).alpha == 0.f);
    const auto hold = PopupTimeline(s, 2.5f);
    CHECK(hold.alpha == 1.f);
    CHECK(hold.bar == 1.f);
    CHECK(hold.text_alpha == 1.f);
    CHECK(hold.slide == 0.f);
    CHECK(hold.icon_scale == 1.f);
    CHECK_FALSE(hold.finished);
    const auto end = PopupTimeline(s, 5.f);
    CHECK(end.finished);
    CHECK(end.alpha == 0.f);
    for (float t = 0; t < 5.f; t += 0.05f) {
      const auto f = PopupTimeline(s, t);
      CHECK(f.alpha >= 0.f);
      CHECK(f.alpha <= 1.f);
      CHECK(f.bar >= 0.f);
      CHECK(f.bar <= 1.f);
      CHECK(f.slide >= 0.f);
      CHECK(f.slide <= 1.f);
      CHECK(f.icon_scale <= 1.12f + 1e-4f);
    }
  }
}

TEST_CASE("ach popup: each style's own motion", "[ach]") {
  using namespace xenos::ach;
  CHECK(PopupTimeline(PopupStyle::X360, 0.3f).bar > 0.f);
  CHECK(PopupTimeline(PopupStyle::X360, 0.3f).bar < 1.f);
  CHECK(PopupTimeline(PopupStyle::X360, 0.1f).text_alpha == 0.f);
  CHECK(PopupTimeline(PopupStyle::Xbox, 0.18f).icon_scale > 1.f);  // overshoot
  CHECK(PopupTimeline(PopupStyle::Xbox, 0.9f).shine > 0.f);
  CHECK(PopupTimeline(PopupStyle::Xbox, 2.0f).shine == -1.f);
  CHECK(PopupTimeline(PopupStyle::Ps5, 0.2f).slide > 0.f);
  CHECK(PopupTimeline(PopupStyle::Ps5, 4.8f).slide > 0.f);
}
