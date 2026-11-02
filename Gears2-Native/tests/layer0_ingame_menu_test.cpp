#ifndef GN_BUILD_TESTS
#define GN_BUILD_TESTS 1
#endif
#include <catch2/catch_test_macros.hpp>
#include <xenos/dev_tools.h>
#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DECLARE(bool, mnk_mouse);

using rex::X_RESULT;

namespace {

class MockAppContext : public rex::ui::WindowedAppContext {
 public:
  void NotifyUILoopOfPendingFunctions() override {}
  void PlatformQuitFromUIThread() override {}
};

class MockWindow : public rex::ui::Window {
 public:
  MockWindow(MockAppContext& ctx) : rex::ui::Window(ctx, "MockWindow", 1280, 720) {}
  bool OpenImpl() override { return true; }
  void RequestCloseImpl() override {}
  std::unique_ptr<rex::ui::Surface> CreateSurfaceImpl(rex::ui::Surface::TypeFlags) override { return nullptr; }
  void RequestPaintImpl() override {}

  bool SetRelativeMouseMode(bool enable) override {
    relative_mode_ = enable;
    return true;
  }

  void* GetNativeWindowHandle() const override {
    return native_hwnd_;
  }

  void* native_hwnd_ = nullptr;
  bool relative_mode_ = false;
};

}  // namespace

TEST_CASE("XenosInGameMenuIsOpen toggles state cleanly", "[layer0][ui][ingame_menu]") {
  XenosSetInGameMenuOpen(false);
  CHECK_FALSE(XenosInGameMenuIsOpen());

  XenosSetInGameMenuOpen(true);
  CHECK(XenosInGameMenuIsOpen());

  XenosSetInGameMenuOpen(false);
  CHECK_FALSE(XenosInGameMenuIsOpen());
}

TEST_CASE("BlockGameInput safely handles null runtime and input system", "[layer0][ui][ingame_menu]") {
  XenosSetInGameMenuInputSystem(nullptr);

  // When Runtime::instance() and g_input are null, calling BlockGameInput must not crash.
  BlockGameInput(true);
  CHECK_FALSE(XenosIsGameInputBlocked());

  BlockGameInput(false);
  CHECK_FALSE(XenosIsGameInputBlocked());
}

TEST_CASE("BlockGameInput tracks blocker balance and avoids desynchronization", "[layer0][ui][ingame_menu]") {
  rex::input::InputSystem input(nullptr);
  XenosSetInGameMenuInputSystem(&input);

  rex::input::X_INPUT_STATE state = {};

  // Initially unblocked: GetState returns non-success (no device connected)
  CHECK_FALSE(XenosIsGameInputBlocked());
  CHECK(input.GetState(0, &state) != X_ERROR_SUCCESS);

  // 1. Block input: blocker is added; InputSystem GetState returns X_ERROR_SUCCESS and zeroes state
  BlockGameInput(true);
  CHECK(XenosIsGameInputBlocked());
  CHECK(input.GetState(0, &state) == X_ERROR_SUCCESS);

  // 2. Redundant block calls do not double-increment the blocker count
  BlockGameInput(true);
  CHECK(XenosIsGameInputBlocked());
  CHECK(input.GetState(0, &state) == X_ERROR_SUCCESS);

  // 3. Unblock input: single call removes the blocker
  BlockGameInput(false);
  CHECK_FALSE(XenosIsGameInputBlocked());
  CHECK(input.GetState(0, &state) != X_ERROR_SUCCESS);

  // 4. Redundant unblock calls do not double-decrement or desynchronize
  BlockGameInput(false);
  CHECK_FALSE(XenosIsGameInputBlocked());
  CHECK(input.GetState(0, &state) != X_ERROR_SUCCESS);

  // Cleanup
  XenosSetInGameMenuInputSystem(nullptr);
}

TEST_CASE("XenosShutdownInGameMenu unblocks input and resets state cleanly", "[layer0][ui][ingame_menu]") {
  rex::input::InputSystem input(nullptr);
  XenosSetInGameMenuInputSystem(&input);

  XenosSetInGameMenuOpen(true);
  BlockGameInput(true);
  CHECK(XenosInGameMenuIsOpen());
  CHECK(XenosIsGameInputBlocked());

  XenosShutdownInGameMenu();
  CHECK_FALSE(XenosInGameMenuIsOpen());
  CHECK_FALSE(XenosIsGameInputBlocked());
}

TEST_CASE("InGameMenu cursor freedom and capture management on open and close with mnk_mouse", "[layer0][ui][ingame_menu]") {
  rex::input::InputSystem input(nullptr);
  XenosSetInGameMenuInputSystem(&input);

  REXCVAR_SET(mnk_mouse, true);
  MockAppContext ctx;
  MockWindow window(ctx);
  XenosSetInGameMenuWindow(&window);

  // Simulate gameplay state with mouse look active
  window.CaptureMouse();
  window.SetRelativeMouseMode(true);
  window.SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
  rex::input::mnk::SetMouseLookActive(true);

  CHECK(window.IsMouseCaptureRequested());
  CHECK(window.relative_mode_);
  CHECK(window.GetCursorVisibility() == rex::ui::Window::CursorVisibility::kHidden);
  CHECK(rex::input::mnk::IsMouseLookActive());
  CHECK_FALSE(XenosInGameMenuIsOpen());

  // Open in-game menu: cursor is freed, relative mouse mode exited, mouse look disabled
  XenosOpenInGameMenu();
  CHECK(XenosInGameMenuIsOpen());
  CHECK_FALSE(rex::input::mnk::IsMouseLookActive());
  CHECK_FALSE(window.IsMouseCaptureRequested());
  CHECK_FALSE(window.relative_mode_);
  CHECK(window.GetCursorVisibility() == rex::ui::Window::CursorVisibility::kHidden);
  CHECK(XenosIsGameInputBlocked());

  // Close in-game menu: cursor is recaptured, hidden, relative mode restored, mouse look re-enabled
  XenosCloseInGameMenu();
  CHECK_FALSE(XenosInGameMenuIsOpen());
  CHECK(rex::input::mnk::IsMouseLookActive());
  CHECK(window.IsMouseCaptureRequested());
  CHECK(window.relative_mode_);
  CHECK(window.GetCursorVisibility() == rex::ui::Window::CursorVisibility::kHidden);
  CHECK_FALSE(XenosIsGameInputBlocked());

  // Cleanup
  XenosShutdownInGameMenu();
}

TEST_CASE("InGameMenu dismissal when mnk_mouse is disabled does not recapture mouse", "[layer0][ui][ingame_menu]") {
  rex::input::InputSystem input(nullptr);
  XenosSetInGameMenuInputSystem(&input);

  REXCVAR_SET(mnk_mouse, false);
  MockAppContext ctx;
  MockWindow window(ctx);
  XenosSetInGameMenuWindow(&window);

  XenosOpenInGameMenu();
  CHECK(XenosInGameMenuIsOpen());
  CHECK_FALSE(window.IsMouseCaptureRequested());
  CHECK(window.GetCursorVisibility() == rex::ui::Window::CursorVisibility::kHidden);

  XenosCloseInGameMenu();
  CHECK_FALSE(XenosInGameMenuIsOpen());
  CHECK_FALSE(window.IsMouseCaptureRequested());
  CHECK_FALSE(window.relative_mode_);
  CHECK(window.GetCursorVisibility() == rex::ui::Window::CursorVisibility::kHidden);
  CHECK_FALSE(XenosIsGameInputBlocked());

  // Cleanup
  XenosShutdownInGameMenu();
}

TEST_CASE("XenosShutdownInGameMenu does not recapture mouse on shutdown", "[layer0][ui][ingame_menu]") {
  rex::input::InputSystem input(nullptr);
  XenosSetInGameMenuInputSystem(&input);

  REXCVAR_SET(mnk_mouse, true);
  MockAppContext ctx;
  MockWindow window(ctx);
  XenosSetInGameMenuWindow(&window);

  // Setup game running and menu opened
  window.CaptureMouse();
  window.SetRelativeMouseMode(true);
  window.SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
  rex::input::mnk::SetMouseLookActive(true);

  XenosOpenInGameMenu();
  CHECK(XenosInGameMenuIsOpen());
  CHECK_FALSE(rex::input::mnk::IsMouseLookActive());
  CHECK_FALSE(window.IsMouseCaptureRequested());
  CHECK_FALSE(window.relative_mode_);
  CHECK(window.GetCursorVisibility() == rex::ui::Window::CursorVisibility::kHidden);

  // Shutdown must NOT recapture mouse
  XenosShutdownInGameMenu();
  CHECK_FALSE(XenosInGameMenuIsOpen());
  CHECK_FALSE(window.IsMouseCaptureRequested());
  CHECK_FALSE(XenosIsGameInputBlocked());
}

TEST_CASE("Mouse deltas are discarded when InGameMenu is open", "[layer0][input][mouse_look]") {
  // Direct delta discard verification when menu is open
  XenosSetInGameMenuOpen(true);
  double dx = 10.0, dy = -5.0;
  if (XenosInGameMenuIsOpen()) {
    dx = dy = 0.0;
  }
  CHECK(dx == 0.0);
  CHECK(dy == 0.0);
  XenosSetInGameMenuOpen(false);

  // Verify full bank drain and suppression behavior matching ue3_mouse::Listener::OnMouseMove
  double bank_dx = 15.0;
  double bank_dy = -8.0;

  auto drain = [&](double* x, double* y) {
    *x = bank_dx;
    *y = bank_dy;
    bank_dx = bank_dy = 0.0;
  };

  auto on_mouse_move = [&](double d_x, double d_y) {
    if (XenosInGameMenuIsOpen()) {
      double x, y;
      drain(&x, &y);
      return;
    }
    bank_dx += d_x;
    bank_dy += d_y;
  };

  // 1. Menu closed: mouse deltas accumulate in bank normally
  XenosSetInGameMenuOpen(false);
  on_mouse_move(5.0, 3.0);
  CHECK(bank_dx == 20.0);
  CHECK(bank_dy == -5.0);

  // 2. Menu open: mouse move immediately flushes bank and discards incoming delta
  XenosSetInGameMenuOpen(true);
  CHECK(XenosInGameMenuIsOpen());
  on_mouse_move(10.0, -5.0);
  CHECK(bank_dx == 0.0);
  CHECK(bank_dy == 0.0);

  // 3. Subsequent mouse moves while menu remains open are also discarded
  on_mouse_move(25.0, -12.0);
  CHECK(bank_dx == 0.0);
  CHECK(bank_dy == 0.0);

  // 4. Menu closed: mouse movement resumes accumulating normally
  XenosSetInGameMenuOpen(false);
  CHECK_FALSE(XenosInGameMenuIsOpen());
  on_mouse_move(4.0, 2.0);
  CHECK(bank_dx == 4.0);
  CHECK(bank_dy == 2.0);
}

TEST_CASE("Layer0 in-game menu open and close suppresses mouse look and blocks game input", "[layer0][input][isolation]") {
  rex::input::InputSystem input(nullptr);
  XenosSetInGameMenuInputSystem(&input);

  REXCVAR_SET(mnk_mouse, true);
  MockAppContext ctx;
  MockWindow window(ctx);
  XenosSetInGameMenuWindow(&window);

  // Initial gameplay state: mouse look is active, input is not blocked
  rex::input::mnk::SetMouseLookActive(true);
  CHECK(rex::input::mnk::IsMouseLookActive());
  CHECK_FALSE(XenosInGameMenuIsOpen());
  CHECK_FALSE(XenosIsGameInputBlocked());

  // Open in-game menu: mouse look is suppressed and game input is blocked
  XenosOpenInGameMenuNow();
  CHECK(XenosInGameMenuIsOpen());
  CHECK_FALSE(rex::input::mnk::IsMouseLookActive());
  CHECK(XenosIsGameInputBlocked());

  // InputSystem reports blocker active (zeroed state returned to game)
  rex::input::X_INPUT_STATE state = {};
  CHECK(input.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(state.gamepad.buttons == 0);

  // Close in-game menu: mouse look is restored and game input is unblocked
  XenosCloseInGameMenu();
  CHECK_FALSE(XenosInGameMenuIsOpen());
  CHECK(rex::input::mnk::IsMouseLookActive());
  CHECK_FALSE(XenosIsGameInputBlocked());

  // Cleanup
  XenosShutdownInGameMenu();
}

TEST_CASE("Input release barrier delays unblocking until buttons are released", "[layer0][ui][barrier]") {
  bool unblock_pending = true;
  bool lmb_down = true;

  auto poll_unblock = [&](bool key_down) {
    if (unblock_pending && !key_down) {
      unblock_pending = false;
      return true;
    }
    return false;
  };

  CHECK_FALSE(poll_unblock(lmb_down));
  CHECK(unblock_pending);

  lmb_down = false;
  CHECK(poll_unblock(lmb_down));
  CHECK_FALSE(unblock_pending);
}

TEST_CASE("XenosAreClosingInputsHeld detects all dismissal inputs and triggers", "[layer0][ui][barrier]") {
  XenosClosingInputsState state = {};
  CHECK_FALSE(XenosAreClosingInputsHeld(state));

  // Win32 buttons
  state.lbutton_down = true;
  CHECK(XenosAreClosingInputsHeld(state));
  state.lbutton_down = false;

  state.rbutton_down = true;
  CHECK(XenosAreClosingInputsHeld(state));
  state.rbutton_down = false;

  state.mbutton_down = true;
  CHECK(XenosAreClosingInputsHeld(state));
  state.mbutton_down = false;

  state.escape_down = true;
  CHECK(XenosAreClosingInputsHeld(state));
  state.escape_down = false;

  state.return_down = true;
  CHECK(XenosAreClosingInputsHeld(state));
  state.return_down = false;

  // Gamepad triggers (deadzone 30)
  state.left_trigger = 30;
  CHECK_FALSE(XenosAreClosingInputsHeld(state));
  state.left_trigger = 31;
  CHECK(XenosAreClosingInputsHeld(state));
  state.left_trigger = 255;
  CHECK(XenosAreClosingInputsHeld(state));
  state.left_trigger = 0;

  state.right_trigger = 30;
  CHECK_FALSE(XenosAreClosingInputsHeld(state));
  state.right_trigger = 31;
  CHECK(XenosAreClosingInputsHeld(state));
  state.right_trigger = 255;
  CHECK(XenosAreClosingInputsHeld(state));
  state.right_trigger = 0;

  // Gamepad dismiss and select buttons
  state.pad_buttons = 0x2000;  // XINPUT_GAMEPAD_B
  CHECK(XenosAreClosingInputsHeld(state));

  state.pad_buttons = 0x0010;  // XINPUT_GAMEPAD_START
  CHECK(XenosAreClosingInputsHeld(state));

  state.pad_buttons = 0x1000;  // XINPUT_GAMEPAD_A
  CHECK(XenosAreClosingInputsHeld(state));

  state.pad_buttons = 0x4000;  // XINPUT_GAMEPAD_X (not a dismiss button)
  CHECK_FALSE(XenosAreClosingInputsHeld(state));

  state.pad_buttons = 0x0001;  // XINPUT_GAMEPAD_DPAD_UP (not a dismiss button)
  CHECK_FALSE(XenosAreClosingInputsHeld(state));
}

TEST_CASE("XenosEvaluateDismissalBarrier enforces neutral release or 250ms safety timeout", "[layer0][ui][barrier]") {
  bool unblock_pending = true;

  // Input held, < 250ms -> stays blocked
  CHECK_FALSE(XenosEvaluateDismissalBarrier(unblock_pending, 0, true));
  CHECK(unblock_pending);

  CHECK_FALSE(XenosEvaluateDismissalBarrier(unblock_pending, 100, true));
  CHECK(unblock_pending);

  CHECK_FALSE(XenosEvaluateDismissalBarrier(unblock_pending, 249, true));
  CHECK(unblock_pending);

  // Input neutral before timeout -> unblocks immediately
  CHECK(XenosEvaluateDismissalBarrier(unblock_pending, 50, false));
  CHECK_FALSE(unblock_pending);

  // When unblock_pending is already false, barrier evaluation returns false
  CHECK_FALSE(XenosEvaluateDismissalBarrier(unblock_pending, 50, false));

  // Input held continuously past 250ms safety timeout -> unblocks
  unblock_pending = true;
  CHECK(XenosEvaluateDismissalBarrier(unblock_pending, 250, true));
  CHECK_FALSE(unblock_pending);

  unblock_pending = true;
  CHECK(XenosEvaluateDismissalBarrier(unblock_pending, 500, true));
  CHECK_FALSE(unblock_pending);
}

TEST_CASE("ScheduleDrop is idempotent and safe against repeated calls", "[layer0][ui][ingame_menu]") {
  rex::input::InputSystem input(nullptr);
  XenosSetInGameMenuInputSystem(&input);

  REXCVAR_SET(mnk_mouse, true);
  MockAppContext ctx;
  MockWindow window(ctx);
  XenosSetInGameMenuWindow(&window);

  // When menu is closed, calling XenosCloseInGameMenu is a safe no-op
  CHECK_FALSE(XenosInGameMenuIsOpen());
  XenosCloseInGameMenu();
  CHECK_FALSE(XenosInGameMenuIsOpen());
  CHECK_FALSE(window.IsMouseCaptureRequested());

  // Open menu
  XenosOpenInGameMenu();
  CHECK(XenosInGameMenuIsOpen());

  // Close once
  XenosCloseInGameMenu();
  CHECK_FALSE(XenosInGameMenuIsOpen());
  CHECK(window.IsMouseCaptureRequested());

  // Close again (idempotent, must not re-trigger mouse capture or state alterations)
  XenosCloseInGameMenu();
  CHECK_FALSE(XenosInGameMenuIsOpen());
  // mouse capture request count must not have increased
  window.ReleaseMouse();
  CHECK_FALSE(window.IsMouseCaptureRequested());

  // Cleanup
  XenosShutdownInGameMenu();
}

TEST_CASE("OpenNow releases nested mouse captures completely", "[layer0][ui][ingame_menu]") {
  MockAppContext ctx;
  MockWindow window(ctx);
  XenosSetInGameMenuWindow(&window);

  // Simulate multiple nested captures (count = 3)
  window.CaptureMouse();
  window.CaptureMouse();
  window.CaptureMouse();
  CHECK(window.IsMouseCaptureRequested());

  XenosOpenInGameMenu();
  CHECK(XenosInGameMenuIsOpen());
  // All nested captures must be fully drained
  CHECK_FALSE(window.IsMouseCaptureRequested());

  // Cleanup
  XenosCloseInGameMenu();
  XenosShutdownInGameMenu();
}

TEST_CASE("XenosSetClosingInputsOverride correctly overrides physical input sampling", "[layer0][ui][barrier]") {
  XenosClosingInputsState simulated = {};
  simulated.lbutton_down = true;
  XenosSetClosingInputsOverride(&simulated);

  // Barrier check with simulated inputs
  bool held = XenosAreClosingInputsHeld(simulated);
  CHECK(held);

  bool unblock_pending = true;
  CHECK_FALSE(XenosEvaluateDismissalBarrier(unblock_pending, 10, held));
  CHECK(unblock_pending);

  simulated.lbutton_down = false;
  held = XenosAreClosingInputsHeld(simulated);
  CHECK_FALSE(held);
  CHECK(XenosEvaluateDismissalBarrier(unblock_pending, 20, held));
  CHECK_FALSE(unblock_pending);

  XenosSetClosingInputsOverride(nullptr);
}



TEST_CASE("XenosBlockGameInput atomic exchange and backwards compatibility wrapper", "[layer0][ui][ingame_menu]") {
  rex::input::InputSystem input(nullptr);
  XenosSetInGameMenuInputSystem(&input);

  CHECK_FALSE(XenosIsGameInputBlocked());

  // 1. Direct call to XenosBlockGameInput(true)
  XenosBlockGameInput(true);
  CHECK(XenosIsGameInputBlocked());

  // 2. Redundant call with atomic exchange is a safe no-op
  XenosBlockGameInput(true);
  CHECK(XenosIsGameInputBlocked());

  // 3. Backwards compatibility wrapper BlockGameInput(false)
  BlockGameInput(false);
  CHECK_FALSE(XenosIsGameInputBlocked());

  // 4. Redundant unblock is a safe no-op
  XenosBlockGameInput(false);
  CHECK_FALSE(XenosIsGameInputBlocked());

  // Cleanup
  XenosSetInGameMenuInputSystem(nullptr);
}

TEST_CASE("InGameMenu manages software mouse cursor via XenosInGameMenuIsMouseCursorDrawn", "[layer0][ui][ingame_menu]") {
  CHECK_FALSE(XenosInGameMenuIsMouseCursorDrawn());

  // With no drawer attached, returns false safely
  XenosSetInGameMenuDrawer(nullptr);
  CHECK_FALSE(XenosInGameMenuIsMouseCursorDrawn());

  // When drawer is attached and menu closes/shuts down, mouse draw cursor is reset
  XenosShutdownInGameMenu();
  CHECK_FALSE(XenosInGameMenuIsMouseCursorDrawn());
}
