// Test of xenos::menu::InputModeTracker (menu4, RC4-1: the menu legend went back to Xbox after every arrow press).
//   g++ -std=c++17 -Wall -Wextra -I../../src/layer0_xenos/include menu_input_test.cpp -o menu_input_test && ./menu_input_test
#include <xenos/menu_input.h>

#include <cstdio>

using xenos::menu::InputModeTracker;
using xenos::menu::PadSample;
static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main() {
  const double f = 1000.0 / 60;   // one frame
  // 1) a connected, idle pad (stick noise under the deadzone, a little trigger rest) never takes the legend
  {
    InputModeTracker t(true);
    double now = 0;
    for (int i = 0; i < 600; ++i, now += f) {
      PadSample p; p.lx = (i % 7) * 0.02f - 0.06f; p.ly = 0.12f; p.lt = 0.05f;
      t.Update(now, p, false, false, false);
    }
    CHECK(t.keys());
  }
  // 2) RC4-1: an arrow key is ALSO the bound left stick in the merged pad state (a frame later, released a frame
  //    after the key): the keys legend stays
  {
    InputModeTracker t(false);
    double now = 0;
    t.Update(now, PadSample{}, false, false, false); now += f;
    for (int press = 0; press < 5; ++press) {
      t.Update(now, PadSample{}, true, true, false); now += f;            // key down
      PadSample s; s.ly = 1.0f;                                           // the bind's stick, full
      for (int i = 0; i < 6; ++i) { t.Update(now, s, true, false, false); now += f; CHECK(!t.pad_event()); CHECK(t.keyboard_recent()); }
      t.Update(now, s, false, false, false); now += f;                    // key up, the stick a frame later
      t.Update(now, PadSample{}, false, false, false); now += f;
      for (int i = 0; i < 30; ++i) { t.Update(now, PadSample{}, false, false, false); now += f; }
      CHECK(t.keys());
    }
    // Enter is bound to Start: a button in the merged state while the key is down - still keys
    t.Update(now, PadSample{}, true, true, false); now += f;
    PadSample st; st.buttons = 0x0010;
    t.Update(now, st, true, false, false); now += f;
    CHECK(t.keys());
  }
  // 2b) the rule before menu4 (InGameMenu::FeedPad: any button or |stick| > 0.35 -> the controller legend) on the same
  //     input shows the RC4-1 flash: keys for the key's frame, then the controller again
  {
    bool keys = false;
    auto old_rule = [&](const PadSample& p, bool key_pressed) {
      if (key_pressed) keys = true;
      if (p.buttons || std::fabs(p.lx) > 0.35f || std::fabs(p.ly) > 0.35f) keys = false;
    };
    old_rule(PadSample{}, true);
    const bool after_key = keys;
    PadSample s; s.ly = 1.0f;
    old_rule(s, false);
    CHECK(after_key && !keys);   // reproduced: back to the controller while the arrow key is still held
  }
  // 3) a real pad event switches to the controller: a button press, a stick crossing the deadzone, a trigger pull
  {
    InputModeTracker t(true);
    double now = 1000;
    t.Update(now, PadSample{}, false, false, false); now += f;
    PadSample a; a.buttons = 0x1000;
    CHECK(t.Update(now, a, false, false, false)); CHECK(!t.keys()); now += f;
    t.Update(now, PadSample{}, false, true, false); now += f; CHECK(t.keys());   // a key: back to keys
    for (int i = 0; i < 30; ++i) { t.Update(now, PadSample{}, false, false, false); now += f; }
    PadSample s; s.lx = 0.8f;
    CHECK(t.Update(now, s, false, false, false)); CHECK(!t.keys()); now += f;
    t.Update(now, s, false, false, true); now += f; CHECK(t.keys());              // the mouse: keys
    CHECK(!t.Update(now, s, false, false, false)); CHECK(t.keys()); now += f;      // a stick HELD is not an event
    PadSample r; r.rt = 0.9f;
    CHECK(t.Update(now, r, false, false, false)); CHECK(!t.keys()); now += f;
  }
  // 4) a pad press right after a key release (inside the grace) is the key's bind; after it, a real one counts
  {
    InputModeTracker t(true);
    double now = 0;
    t.Update(now, PadSample{}, true, true, false); now += f;
    t.Update(now, PadSample{}, false, false, false); now += 100;
    PadSample a; a.buttons = 0x1000;
    CHECK(!t.Update(now, a, false, false, false)); CHECK(t.keys()); now += f;
    t.Update(now, PadSample{}, false, false, false); now += 300;
    CHECK(t.Update(now, a, false, false, false)); CHECK(!t.keys());
  }
  // 5) 0.5.0 review: a key held over kStuckKeyMs (stuck) stops blocking the controller; a short hold still blocks
  {
    InputModeTracker t(true);
    double now = 0;
    t.Update(now, PadSample{}, true, true, false); now += f;
    PadSample a; a.buttons = 0x1000;
    while (now < 1500) { t.Update(now, PadSample{}, true, false, false); now += f; }
    CHECK(!t.Update(now, a, true, false, false)); CHECK(t.keys()); now += f;   // held 1.5 s: still the keyboard's
    while (now < 2400) { t.Update(now, PadSample{}, true, false, false); now += f; }
    CHECK(!t.keyboard_recent());
    CHECK(t.Update(now, a, true, false, false)); CHECK(!t.keys()); now += f;    // held 2.4 s: the pad counts
    t.Update(now, PadSample{}, false, false, false); now += f;                  // released, pressed again: blocks again
    t.Update(now, PadSample{}, true, true, false); now += f;
    CHECK(!t.Update(now, a, true, false, false));
  }
  std::printf("menu_input_test: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
