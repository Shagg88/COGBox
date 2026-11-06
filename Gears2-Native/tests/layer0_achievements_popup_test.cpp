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
  CHECK(PopupDuration(PopupStyle::X360) == 6.1f);
  CHECK(PopupDuration(PopupStyle::Xbox) == 7.3f);
  CHECK(PopupDuration(PopupStyle::Ps5) == 5.7f);
  for (PopupStyle s : {PopupStyle::X360, PopupStyle::Xbox, PopupStyle::Ps5}) {
    const float d = PopupDuration(s);
    CHECK(PopupTimeline(s, 0.f).alpha == 0.f);
    const auto hold = PopupTimeline(s, 2.5f);
    CHECK(hold.alpha == 1.f);
    CHECK(hold.bar == 1.f);
    CHECK(hold.text_alpha == 1.f);
    CHECK(hold.slide == 0.f);
    CHECK(hold.icon_scale == 1.f);
    CHECK(hold.ring == 1.f);
    CHECK(hold.glyph_scale == 1.f);
    CHECK(hold.swap == 1.f);
    CHECK(hold.light == 1.f);
    CHECK(hold.glow == 0.f);
    CHECK(hold.cover == 0.f);
    CHECK_FALSE(hold.finished);
    CHECK_FALSE(PopupTimeline(s, d - 0.01f).finished);
    const auto end = PopupTimeline(s, d);
    CHECK(end.finished);
    CHECK(end.alpha == 0.f);
    for (float t = 0; t < d; t += 0.05f) {
      const auto f = PopupTimeline(s, t);
      CHECK(f.alpha >= 0.f);
      CHECK(f.alpha <= 1.f);
      CHECK(f.bar >= 0.f);
      CHECK(f.bar <= 1.f);
      CHECK(f.glow >= 0.f);
      CHECK(f.cover >= 0.f);
      CHECK(f.cover <= 1.f);
      CHECK(f.glow <= 1.f);
      CHECK(f.slide >= 0.f);
      CHECK(f.slide <= 1.f);
      CHECK(f.icon_scale >= 0.f);
      CHECK(f.icon_scale <= 1.f);
      CHECK(f.ring >= 0.f);
      CHECK(f.ring <= 1.f);
      CHECK(f.swap >= 0.f);
      CHECK(f.swap <= 1.f);
      CHECK(f.page >= 0.f);
      CHECK(f.page <= 1.f);
    }
  }
}

TEST_CASE("ach popup: Xbox 360 motion (from the reference clip, 60 fps)", "[ach]") {
  using namespace xenos::ach;
  const auto s = PopupStyle::X360;
  CHECK(PopupTimeline(s, 0.06f).glow > 0.f);           // a white ghost of the orb flashes first
  CHECK(PopupTimeline(s, 0.3f).glow == 0.f);
  CHECK(PopupTimeline(s, 0.05f).icon_scale == 1.f);    // the orb fades in at full size (it does not grow)
  CHECK(PopupTimeline(s, 0.05f).alpha > 0.f);
  CHECK(PopupTimeline(s, 0.05f).alpha < 1.f);
  CHECK(PopupTimeline(s, 0.25f).bar > 0.f);            // the bar slides out from behind the orb
  CHECK(PopupTimeline(s, 0.25f).bar < 1.f);
  CHECK(PopupTimeline(s, 0.4f).bar == 1.f);            // no overshoot
  CHECK(PopupTimeline(s, 0.3f).text_alpha == 0.f);     // the text comes when the bar is nearly open
  CHECK(PopupTimeline(s, 0.34f).slide > 0.f);          // ... with a small slide from the right
  CHECK(PopupTimeline(s, 0.4f).text_alpha == 1.f);
  CHECK(PopupTimeline(s, 0.5f).slide == 0.f);
  CHECK(PopupTimeline(s, 0.2f).cover == 1.f);          // the sphere fills the whole orb at first
  CHECK(PopupTimeline(s, 0.25f).ring == 0.f);
  CHECK(PopupTimeline(s, 0.31f).ring == 1.f);          // the ring is there at full strength, under the sphere
  CHECK(PopupTimeline(s, 0.33f).cover > 0.f);          // the sphere shrinks and uncovers it (no fade)
  CHECK(PopupTimeline(s, 0.33f).cover < 1.f);
  CHECK(PopupTimeline(s, 0.4f).cover == 0.f);
  CHECK(PopupTimeline(s, 0.36f).light == 0.f);         // the green quarter switches on a moment later
  CHECK(PopupTimeline(s, 0.42f).light == 1.f);
  CHECK(PopupTimeline(s, 0.6f).swap == 0.f);           // still the X
  CHECK(PopupTimeline(s, 0.8f).swap > 0.f);            // X -> achievement icon
  CHECK(PopupTimeline(s, 5.35f).swap < 1.f);           // and back at the end
  CHECK(PopupTimeline(s, 5.7f).text_alpha == 0.f);     // out: text, ring, then the orb shrinks, then the bar fades
  CHECK(PopupTimeline(s, 5.7f).ring == 0.f);
  CHECK(PopupTimeline(s, 5.75f).icon_scale > 0.f);
  CHECK(PopupTimeline(s, 5.75f).icon_scale < 1.f);
  CHECK(PopupTimeline(s, 5.9f).icon_scale == 0.f);
  CHECK(PopupTimeline(s, 5.9f).alpha < 1.f);
  CHECK(PopupTimeline(s, 5.95f).bar < 1.f);
  CHECK(PopupTimeline(s, 5.95f).bar > 0.6f);
}

TEST_CASE("ach popup: modern Xbox motion (from the reference clip)", "[ach]") {
  using namespace xenos::ach;
  const auto s = PopupStyle::Xbox;
  CHECK(PopupTimeline(s, 0.2f).icon_scale < 1.f);      // the ring grows
  CHECK(PopupTimeline(s, 0.2f).ring == 0.f);           // hollow until it fills
  CHECK(PopupTimeline(s, 0.6f).glyph_scale > 0.f);     // the icon grows from a dot
  CHECK(PopupTimeline(s, 0.6f).glyph_scale < 1.f);
  CHECK(PopupTimeline(s, 1.0f).bar == 0.f);            // still only the badge
  CHECK(PopupTimeline(s, 1.3f).swap > 0.f);            // the coin flip to the trophy
  CHECK(PopupTimeline(s, 1.3f).swap < 1.f);
  CHECK(PopupTimeline(s, 1.4f).bar > 0.f);             // the pill opens from the centre
  CHECK(PopupTimeline(s, 3.0f).page == 0.f);           // title page
  CHECK(PopupTimeline(s, 5.0f).page == 1.f);           // description page
  CHECK(PopupTimeline(s, 6.4f).bar < 1.f);             // closing
  CHECK(PopupTimeline(s, 6.6f).swap < 1.f);            // flips back
  CHECK(PopupTimeline(s, 7.1f).icon_scale < 1.f);      // the ring shrinks away
}

TEST_CASE("ach popup: PS5 motion (from the reference clip)", "[ach]") {
  using namespace xenos::ach;
  const auto s = PopupStyle::Ps5;
  CHECK(PopupTimeline(s, 0.05f).slide > 0.f);          // a short slide from the right while it fades in
  CHECK(PopupTimeline(s, 0.05f).alpha > 0.f);
  CHECK(PopupTimeline(s, 0.2f).slide == 0.f);
  CHECK(PopupTimeline(s, 5.6f).slide > 0.f);
  CHECK(PopupTimeline(s, 5.6f).alpha < 1.f);
  CHECK(kPs5SlidePixels == 10.f);
}

// Part 4 player check: the in-game menu applies the popup settings at once (the popups read their cvars on each unlock)
TEST_CASE("ach: LiveCvarFor maps the launcher's popup options to game cvars", "[ach]") {
  using xenos::ach::LiveCvarFor;
  auto c = LiveCvarFor("AchievementStyle", 2);
  REQUIRE(c);
  CHECK(c->first == "cogbox_ach_style");
  CHECK(c->second == "ps5");
  CHECK(LiveCvarFor("AchievementStyle", 0)->second == "x360");
  CHECK(LiveCvarFor("AchievementColour", 7)->second == "teal");
  CHECK(LiveCvarFor("AchievementColour", 7)->first == "cogbox_ach_colour");
  CHECK(LiveCvarFor("AchievementPosition", 3)->second == "top_right");
  CHECK(LiveCvarFor("AchievementPosition", 0)->second == "default");
  CHECK(LiveCvarFor("AchievementSound", 0)->second == "false");
  CHECK(LiveCvarFor("AchievementSound", 1)->first == "cogbox_ach_sound");
  CHECK(LiveCvarFor("HideAchievementPopups", 1)->second == "true");
  CHECK(LiveCvarFor("HideAchievementPopups", 1)->first == "cogbox_ach_hide");
  CHECK(LiveCvarFor("AchievementColour", 42)->second == "dark");  // out of range: the first, as the launcher does
  CHECK_FALSE(LiveCvarFor("Fov", 90));                             // not a live option: needs the restart as before
  // every value maps to a name the popups read back
  for (int i = 0; i < 10; ++i)
    CHECK(xenos::ach::ParseColour(LiveCvarFor("AchievementColour", i)->second).name ==
          xenos::ach::kColourPresets[size_t(i)].name);
}

// Part 4 player check: F7's descriptions wrap to two lines (then "...")
TEST_CASE("ach: WrapText wraps at spaces into at most max_lines, the last cut with an ellipsis", "[ach]") {
  using xenos::ach::WrapText;
  auto w = [](const std::string& s) { return static_cast<float>(s.size()); };  // 1 px per byte
  CHECK(WrapText("", 10.f, w, 2).empty());
  CHECK(WrapText("short", 10.f, w, 2) == std::vector<std::string>{"short"});
  CHECK(WrapText("aaaa bbbb cccc", 10.f, w, 2) == std::vector<std::string>{"aaaa bbbb", "cccc"});
  // too long for two lines: the second is cut and ends with the ellipsis, within max_width
  const auto cut = WrapText("aaaa bbbb cccc dddd eeee", 10.f, w, 2);
  REQUIRE(cut.size() == 2);
  CHECK(cut[0] == "aaaa bbbb");
  CHECK(cut[1].size() <= 10);
  CHECK(cut[1].substr(cut[1].size() - 3) == "\xE2\x80\xA6");
  // a word longer than a line breaks inside it, never mid UTF-8 character
  const auto longw = WrapText("abcdefghijklmnop", 10.f, w, 3);
  REQUIRE(longw.size() == 2);
  CHECK(longw[0] == "abcdefghij");
  CHECK(longw[1] == "klmnop");
  const auto cjk = WrapText("\xED\x95\x9C\xED\x95\x9C\xED\x95\x9C\xED\x95\x9C", 7.f, w, 3);  // 4 x 3-byte characters
  REQUIRE(cjk.size() == 2);
  CHECK(cjk[0] == "\xED\x95\x9C\xED\x95\x9C");
  CHECK(cjk[1] == "\xED\x95\x9C\xED\x95\x9C");
  CHECK(WrapText("one two", 100.f, w, 1) == std::vector<std::string>{"one two"});
}

// Part 4 final review: the changes file written on a live change holds only the live options, so a restart-only edit
// the player put back is never saved by a crash / Alt-F4 after a popup change
TEST_CASE("ach: MenuChangesText keeps restart-only edits out of the live write", "[ach]") {
  using xenos::ach::MenuChange;
  using xenos::ach::MenuChangesText;
  const std::vector<MenuChange> opts = {{"Fov", 110, 90}, {"AchievementStyle", 2, 0}, {"HDR", 0, 0}};
  CHECK(MenuChangesText(opts, false) == "Fov=110\r\nAchievementStyle=2\r\n");  // Quit / Restart: every change
  CHECK(MenuChangesText(opts, true) == "AchievementStyle=2\r\n");              // a live change: live options only
  const std::vector<MenuChange> reverted = {{"Fov", 90, 90}, {"AchievementStyle", 2, 0}};
  CHECK(MenuChangesText(reverted, false) == "AchievementStyle=2\r\n");
  CHECK(MenuChangesText({{"AchievementSound", 0, 1}}, true) == "AchievementSound=0\r\n");
  CHECK(MenuChangesText({{"Sens", 1.7, 1}}, false) == "Sens=1.7\r\n");  // "%.6g", as the menu always wrote
}
