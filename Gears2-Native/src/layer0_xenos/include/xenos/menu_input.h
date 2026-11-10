// Layer 0 (game-free): which legend the in-game menu shows - keys or the controller (menu4, RC4-1). Header only, no
// Windows / ImGui types, so tools/menu_mock/menu_input_test.cpp checks it on Linux. Our code (MIT).
//
// The controller state the menu polls is the SDK's merged player-1 state: with keyboard-and-mouse on, the key binds
// (--keybind_lstick_up=W,Up, --keybind_start=Escape,Return ...) show up in it as stick moves and buttons. So an arrow
// press arrived both as a key and, a frame later, as "pad input" - the legend flashed keys, then went back to the
// controller. Now only a pad EVENT switches to the controller legend: a button press (an edge), or a stick / trigger
// crossing kDeadzone from rest - and none while a keyboard key is down or was released less than kKeyGraceMs ago
// (that pad change is the keyboard's). An idle connected pad never switches anything. Keys and the mouse switch to keys.
// A key held down longer than kStuckKeyMs (a stuck key, a key bound by another program) stops blocking the controller.
#pragma once
#include <cmath>
#include <cstdint>

namespace xenos::menu {

struct PadSample {
  uint16_t buttons = 0;        // XInput bits, the ones held when the menu opened already removed
  float lx = 0, ly = 0, rx = 0, ry = 0;   // -1..1
  float lt = 0, rt = 0;        // 0..1
};

class InputModeTracker {
 public:
  static constexpr float kDeadzone = 0.3f;
  static constexpr double kKeyGraceMs = 250.0;
  static constexpr double kStuckKeyMs = 2000.0;

  explicit InputModeTracker(bool keys = false) : keys_(keys) {}
  bool keys() const { return keys_; }   // true: the keyboard legend
  void SetKeys(bool k) { keys_ = k; }

  // One menu frame. now_ms: a steady clock; key_down: a keyboard key is held; key_pressed: one went down this frame;
  // mouse: the mouse moved / clicked / wheeled. Returns pad_event_ (a real controller event this frame).
  bool Update(double now_ms, const PadSample& p, bool key_down, bool key_pressed, bool mouse) {
    if (!key_down) key_down_since_ms_ = -1;
    else if (key_pressed || key_down_since_ms_ < 0) key_down_since_ms_ = now_ms;
    const bool held = key_down && now_ms - key_down_since_ms_ <= kStuckKeyMs;   // not a stuck key
    if (held || key_pressed) last_key_ms_ = now_ms;
    const bool keyboard_recent = held || key_pressed || (last_key_ms_ >= 0 && now_ms - last_key_ms_ < kKeyGraceMs);
    const uint16_t pressed = uint16_t(p.buttons & ~prev_.buttons);
    auto crossed = [](float now, float before) { return std::fabs(now) > kDeadzone && std::fabs(before) <= kDeadzone; };
    const bool moved = crossed(p.lx, prev_.lx) || crossed(p.ly, prev_.ly) || crossed(p.rx, prev_.rx) || crossed(p.ry, prev_.ry) ||
                       crossed(p.lt, prev_.lt) || crossed(p.rt, prev_.rt);
    prev_ = p;
    pad_event_ = (pressed != 0 || moved) && !keyboard_recent;
    if (key_pressed || mouse) keys_ = true;
    else if (pad_event_) keys_ = false;
    keyboard_recent_ = keyboard_recent;
    return pad_event_;
  }
  bool pad_event() const { return pad_event_; }
  // The pad state this frame is the keyboard's (its binds), not a controller's: the menu then takes its moves from
  // the keys only (no second step from the bound stick).
  bool keyboard_recent() const { return keyboard_recent_; }

 private:
  bool keys_ = false, pad_event_ = false, keyboard_recent_ = false;
  double last_key_ms_ = -1, key_down_since_ms_ = -1;
  PadSample prev_;
};

}  // namespace xenos::menu
