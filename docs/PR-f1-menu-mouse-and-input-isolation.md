## Summary

This PR adds full mouse navigation, software cursor rendering, and complete input isolation to the in-game options menu (F1 / `Back` + `Start`) across all COGBox titles (**Gears of War 2** Disc & TU6, **Gears of War 3** TU6, and **Gears of War: Judgment**).

### Problems Addressed
1. **No Mouse Navigation:** Opening the F1 menu kept the OS cursor locked in relative mode; mouse motion continued driving the UE3 camera behind the menu instead of pointing/clicking ImGui controls.
2. **Invisible Mouse Cursor:** Direct3D 12 swapchains and borderless fullscreen mode suppress the Win32 hardware cursor, leaving the mouse pointer invisible even while hovering over ImGui elements.
3. **Input Bleed-Through:** Keyboard keys, mouse clicks, and gamepad buttons leaked into Unreal Engine 3 while navigating the menu, causing the character to fire weapons, roll, or move during menu interaction.
4. **Dismissal Weapon Fire / Action Bleed:** Clicking "Resume" with the Left Mouse Button or pressing `Esc` / `B` to close the menu immediately fired equipped weapons or rolled the player upon exiting.

---

## Key Changes

### 1. Cursor Lifecycle & Software Rendering (`layer0_xenos/ingame_menu.cpp`)
- **Software Cursor Rendering (`io.MouseDrawCursor = true`):**
  - Engages Dear ImGui's built-in software cursor (`io.MouseDrawCursor = true`), dynamically scaled with the UI layout (`ImGui::GetStyle().MouseCursorScale = scale`).
  - Renders directly into the game's DirectX 12 render pass, guaranteeing a crisp, visible mouse pointer across all display modes (fullscreen, borderless, windowed, high-DPI).
  - Automatically hides the mouse cursor when gamepad controls (sticks/D-pad) are used for navigation, and restores it as soon as the mouse is moved or clicked.
- **On Menu Open (`OpenNow`):**
  - Disables mouse look via `rex::input::mnk::SetMouseLookActive(false)`.
  - Exits relative mouse mode (`SetRelativeMouseMode(false)`).
  - Releases mouse capture (`ReleaseMouse()`).
  - Explicitly hides the OS hardware cursor (`SetCursorVisibility(kHidden)`) to eliminate double-cursor artifacts.
- **On Menu Close (`ScheduleDrop`):**
  - Disables software cursor drawing (`io.MouseDrawCursor = false`).
  - If keyboard/mouse mode is active (`REXCVAR_GET(mnk_mouse)`), re-engages mouse capture, re-enters relative mode (`SetRelativeMouseMode(true)`), and re-enables mouse look (`SetMouseLookActive(true)`).

### 2. Full Input Isolation & Dynamic `InputSystem` Acquisition (`layer0_xenos/ingame_menu.cpp`, `xenos/dev_tools.h`)
- Dynamically retrieves `rex::input::InputSystem` from `rex::Runtime::instance()` if not available at early initialization, guaranteeing `AddUIInputBlocker()` is always invoked.
- Manages UI input blocking with atomic exchange (`g_blocking.exchange(on) == on`) to prevent duplicate blocker registrations.
- Exposes clean Layer 0 APIs:
  - `bool XenosInGameMenuIsOpen() noexcept`
  - `void XenosSetInGameMenuOpen(bool open) noexcept`
  - `void XenosBlockGameInput(bool on)`
  - `bool XenosIsGameInputBlocked() noexcept`
  - `bool XenosInGameMenuIsMouseCursorDrawn() noexcept`

### 3. UE3 Mouse Look Suppression (`layer3_gow2`, `layer3_gow3`, `layer3_gowj`)
- Updated `Listener::OnMouseMove` in `ue3_mouse_look.h` across Gears 2, Gears 3, and Judgment.
- When `XenosInGameMenuIsOpen()` returns true, all incoming mouse motion packets are drained and discarded before reaching the viewport camera.

### 4. Anti-Bleed-Through Dismissal Barrier (`layer0_xenos/ingame_menu.cpp`)
- When the user dismisses the menu (clicks "Resume", presses `Esc`, or presses gamepad `B`/`Start`), the visual ImGui dialog and software cursor hide immediately to maintain crisp UI responsiveness.
- The underlying `UIInputBlocker` remains active until all closing inputs (`VK_LBUTTON`, `VK_RBUTTON`, `VK_MBUTTON`, `VK_ESCAPE`, `VK_RETURN`, controller triggers, and face buttons) are physically released, with a 250ms fallback timeout.
- Win32 `GetAsyncKeyState` checks are strictly guarded by `GetForegroundWindow() == hwnd` to prevent interference when alt-tabbed.

### 5. Unit Tests (`Gears2-Native/tests/layer0_ingame_menu_test.cpp`, `CMakeLists.txt`)
- Added comprehensive Catch2 unit tests verifying:
  - Menu open/close state transitions.
  - Software mouse cursor state tracking via `XenosInGameMenuIsMouseCursorDrawn()`.
  - Cursor freedom, visibility, and relative mode toggling.
  - UI input blocking acquisition and teardown.
  - Mouse look deactivation during menu visibility.
  - Dismissal barrier evaluation with held keys and timeout fallbacks.

---

## Verification

1. **Unit Tests:**
   - Ran `gn_tests.exe`: all 144 test cases (1,064,090 assertions) passed cleanly with 0 failures.
2. **Compilation & Multi-Game Build:**
   - Compiled all 4 Release configurations with 0 errors:
     - `Gears2\port-disc\gow2.exe`
     - `Gears2\port-tu6\gow2.exe`
     - `Gears3\port-tu6\gow3.exe`
     - `Judgment\port\gowj.exe`
3. **In-Game Testing:**
   - Verified visible software mouse cursor on F1 open.
   - Verified widgets (tabs, sliders, dropdowns, buttons) respond smoothly to mouse clicks, dragging, and scrolling.
   - Verified camera remains completely still while moving the mouse inside the menu.
   - Verified clicking "Resume" with LMB does not fire weapon upon returning to gameplay.
