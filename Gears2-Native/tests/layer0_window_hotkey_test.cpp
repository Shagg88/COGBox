// Layer 0 unit tests: the window-mode hotkey's key text (GitHub #316, xenos/window_hotkey.h): "Alt+Return", "F11",
// "Ctrl+Shift+F11", "None", and the exact-modifier match. Our code (MIT).
#include <catch2/catch_test_macros.hpp>

#include <xenos/window_hotkey.h>

namespace {
using namespace xenos::window_hotkey;
using VK = rex::ui::VirtualKey;
}  // namespace

TEST_CASE("window hotkey: the default Alt+Return and other key texts parse", "[window_hotkey]") {
  Chord c;
  REQUIRE(ParseChord("Alt+Return", c));
  CHECK(c.key == VK::kReturn);
  CHECK(c.alt);
  CHECK_FALSE(c.ctrl);
  CHECK_FALSE(c.shift);

  REQUIRE(ParseChord(" alt + return ", c));   // spaces and lower case are fine
  CHECK(c.key == VK::kReturn);
  CHECK(c.alt);

  REQUIRE(ParseChord("Alt+Enter", c));   // the name on the key works too (r40)
  CHECK(c.key == VK::kReturn);
  CHECK(c.alt);
  REQUIRE(ParseChord("alt+ENTER", c));
  CHECK(c.key == VK::kReturn);
  CHECK_FALSE(ParseChord("Alt+Enterr", c));

  REQUIRE(ParseChord("F11", c));
  CHECK(c.key == VK::kF11);
  CHECK_FALSE(c.alt);

  REQUIRE(ParseChord("Ctrl+Shift+F11", c));
  CHECK(c.key == VK::kF11);
  CHECK(c.ctrl);
  CHECK(c.shift);
  CHECK_FALSE(c.alt);
}

TEST_CASE("window hotkey: None, empty and unusable texts leave the hotkey off", "[window_hotkey]") {
  Chord c;
  CHECK_FALSE(ParseChord("", c));
  CHECK_FALSE(ParseChord("None", c));
  CHECK_FALSE(ParseChord("none", c));
  CHECK_FALSE(ParseChord("Alt", c));            // a modifier alone is not a key
  CHECK_FALSE(ParseChord("Alt+", c));
  CHECK_FALSE(ParseChord("Alt+Shift", c));
  CHECK_FALSE(ParseChord("Foo+F11", c));        // not a modifier
  CHECK_FALSE(ParseChord("Alt+NoSuchKey", c));
  CHECK_FALSE(ParseChord("LMB", c));            // mouse buttons are not window-hotkey keys
  CHECK_FALSE(c.valid());
}

TEST_CASE("window hotkey: the modifiers must match exactly", "[window_hotkey]") {
  Chord c;
  REQUIRE(ParseChord("Alt+Return", c));
  CHECK(Matches(c, VK::kReturn, true, false, false));
  CHECK_FALSE(Matches(c, VK::kReturn, false, false, false));   // plain Enter is the game's Start
  CHECK_FALSE(Matches(c, VK::kReturn, true, true, false));
  CHECK_FALSE(Matches(c, VK::kF11, true, false, false));

  REQUIRE(ParseChord("F11", c));
  CHECK(Matches(c, VK::kF11, false, false, false));
  CHECK_FALSE(Matches(c, VK::kF11, false, true, false));

  CHECK_FALSE(Matches(Chord{}, VK::kNone, false, false, false));
}
