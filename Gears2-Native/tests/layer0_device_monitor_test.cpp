// Layer 0 unit tests: multi-hardware gamepad classification (Switch, Steam Deck, Steam Controller, PlayStation, Xbox),
// mouse deadzone (> 5 px) & 250 ms cross-device debounce, live user cvar overrides, auto mode's Xbox = the game's own
// prompts, and GitHub #104 (keep the pad prompts with keyboard and mouse) in PromptDeviceMonitor.
// Contributed by Flippage (COGBox PR #117); portions from gow3-recomp: BSD-3-Clause, Copyright (c) Flippage and the
// gow3-recomp contributors (THIRD_PARTY_NOTICES.md). The rest is our code (MIT).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <xenos/input/prompt_device_monitor.h>
#include <xenos/renderer/prompt_overlay_compositor.h>
#include <xenos/renderer/texture_cache.h>

namespace {

using namespace xenos::input;
using namespace xenos::renderer;
using namespace xenos::renderer::prompt;

struct ScopedPromptCvarReset {
  ScopedPromptCvarReset() {
    REXCVAR_SET(gow_button_prompts, "auto");
    REXCVAR_SET(gow2_button_prompts, "auto");
    REXCVAR_SET(gow_button_prompts_keep_pad, false);
    GlobalPromptDeviceMonitor().Reset();
  }
  ~ScopedPromptCvarReset() {
    REXCVAR_SET(gow_button_prompts, "auto");
    REXCVAR_SET(gow2_button_prompts, "auto");
    REXCVAR_SET(gow_button_prompts_keep_pad, false);
    GlobalPromptDeviceMonitor().Reset();
    ClearDynamicOverlays();
  }
};

}  // namespace

TEST_CASE("PromptDeviceMonitor ClassifyGamepad identifies Switch, Steam Deck, Steam Controller, PlayStation, and Xbox",
          "[layer0][input][device_monitor]") {
  ScopedPromptCvarReset guard;
  PromptDeviceMonitor monitor;

  // 1. Nintendo Switch Pro (Type 7, VID 0x057E, PID 0x2009) and Joy-Cons (Types 8, 9, 10)
  CHECK(monitor.ClassifyGamepad(7, 0x057E, 0x2009, "Pro Controller") == DeviceStyle::kSwitch);
  CHECK(monitor.ClassifyGamepad(8, 0x057E, 0x2006, "Joy-Con (L)") == DeviceStyle::kSwitch);
  CHECK(monitor.ClassifyGamepad(9, 0x057E, 0x2007, "Joy-Con (R)") == DeviceStyle::kSwitch);
  CHECK(monitor.ClassifyGamepad(10, 0x057E, 0x2008, "Joy-Con Pair") == DeviceStyle::kSwitch);
  CHECK(monitor.ClassifyGamepad(1, 0x0000, 0x0000, "Nintendo Switch Pro Controller") == DeviceStyle::kSwitch);

  // 2. Steam Deck (Valve VID 0x28DE, PID 0x1205, name "Steam Deck Controller", or steam_deck_env = true)
  CHECK(monitor.ClassifyGamepad(1, 0x28DE, 0x1205, "Steam Deck Controller") == DeviceStyle::kSteamDeck);
  CHECK(monitor.ClassifyGamepad(1, 0x0000, 0x0000, "Steam Deck") == DeviceStyle::kSteamDeck);
  CHECK(monitor.ClassifyGamepad(1, 0x28DE, 0x11FF, "Steam Virtual Gamepad", true) == DeviceStyle::kSteamDeck);
  CHECK(monitor.ClassifyGamepad(2, 0x045E, 0x028E, "Microsoft X-Box 360 pad", true) == DeviceStyle::kSteamDeck);

  // 3. Steam Controller (Valve VID 0x28DE, PIDs 0x1102, 0x1142, 0x1106, or name "Steam Controller")
  CHECK(monitor.ClassifyGamepad(1, 0x28DE, 0x1102, "Valve Wired Steam Controller") == DeviceStyle::kSteamController);
  CHECK(monitor.ClassifyGamepad(1, 0x28DE, 0x1142, "Wireless Steam Controller") == DeviceStyle::kSteamController);
  CHECK(monitor.ClassifyGamepad(1, 0x28DE, 0x1106, "Steam Controller BLE") == DeviceStyle::kSteamController);
  CHECK(monitor.ClassifyGamepad(1, 0x0000, 0x0000, "Steam Controller") == DeviceStyle::kSteamController);
  // Even on a Steam Deck, an external Steam Controller must still classify as kSteamController.
  CHECK(monitor.ClassifyGamepad(1, 0x28DE, 0x1142, "Steam Controller", true) == DeviceStyle::kSteamController);

  // 4. PlayStation DualShock 3 / DualShock 4 / DualSense (Types 4, 5, 6, Sony VID 0x054C, PID 0x0CE6)
  CHECK(monitor.ClassifyGamepad(4, 0x054C, 0x0268, "PS3 Controller") == DeviceStyle::kPlayStation);
  CHECK(monitor.ClassifyGamepad(5, 0x054C, 0x09CC, "DualShock 4") == DeviceStyle::kPlayStation);
  CHECK(monitor.ClassifyGamepad(6, 0x054C, 0x0CE6, "DualSense Wireless Controller") == DeviceStyle::kPlayStation);
  CHECK(monitor.ClassifyGamepad(1, 0x054C, 0x0DF2, "DualSense Edge") == DeviceStyle::kPlayStation);
  CHECK(monitor.ClassifyGamepad(1, 0x0000, 0x0000, "PS5 Wireless Controller") == DeviceStyle::kPlayStation);
  // External DualSense on Steam Deck must classify as kPlayStation.
  CHECK(monitor.ClassifyGamepad(6, 0x054C, 0x0CE6, "DualSense Wireless Controller", true) == DeviceStyle::kPlayStation);

  // 5. Xbox 360 / One / Series (Types 2, 3, Microsoft VID 0x045E, PID 0x0B12)
  CHECK(monitor.ClassifyGamepad(2, 0x045E, 0x028E, "Xbox 360 Controller") == DeviceStyle::kXbox);
  CHECK(monitor.ClassifyGamepad(3, 0x045E, 0x02EA, "Xbox One S Controller") == DeviceStyle::kXbox);
  CHECK(monitor.ClassifyGamepad(3, 0x045E, 0x0B12, "Xbox Series X Controller") == DeviceStyle::kXbox);
  CHECK(monitor.ClassifyGamepad(1, 0x0000, 0x0000, "Generic XInput Gamepad") == DeviceStyle::kXbox);
}

TEST_CASE("PromptDeviceMonitor: mouse jitter never switches, pad input switches back at once (rc2)",
          "[layer0][input][device_monitor]") {
  ScopedPromptCvarReset guard;
  GlobalPromptCompositor().Initialize("assets/glyphs");

  PromptDeviceMonitor monitor;
  monitor.Reset();

  monitor.NotifyGamepadActivity(DeviceStyle::kPlayStation, 1000);
  CHECK(monitor.EvaluateActiveStyle(1000) == DeviceStyle::kPlayStation);
  CHECK(GlobalPromptCompositor().ActiveStyle() == DeviceStyle::kPlayStation);

  // Sub-deadzone jitter is ignored, and so is a steady stream of small moves (a gaming mouse's sensor noise): each
  // window of kMouseTravelWindowMs stays under kMouseTravelPx.
  monitor.NotifyMouseMove(5, -5, 1500);
  monitor.NotifyMouseMove(0, 3, 1600);
  for (uint64_t t = 2000; t < 4000; t += 250) monitor.NotifyMouseMove(12, 2, t);
  CHECK(monitor.EvaluateActiveStyle(4000) == DeviceStyle::kPlayStation);

  // A real mouse move (40+ px within 200 ms) after the pad has been quiet 250 ms: key caps.
  monitor.NotifyMouseMove(20, 0, 5000);
  monitor.NotifyMouseMove(25, 0, 5016);
  CHECK(monitor.EvaluateActiveStyle(5016) == DeviceStyle::kKeyboard);
  CHECK(GlobalPromptCompositor().ActiveStyle() == DeviceStyle::kKeyboard);

  // A key brushed while the pad is in use (inside 250 ms of pad input) doesn't flicker to key caps.
  monitor.NotifyGamepadActivity(DeviceStyle::kSwitch, 6000);
  CHECK(monitor.EvaluateActiveStyle(6000) == DeviceStyle::kSwitch);
  monitor.NotifyKeyboardMouseActivity(6100);
  CHECK(monitor.EvaluateActiveStyle(6100) == DeviceStyle::kSwitch);
  CHECK(monitor.EvaluateActiveStyle(6400) == DeviceStyle::kKeyboard);

  // Pad input right after keyboard use switches back at once - no debounce on the way back.
  monitor.NotifyGamepadActivity(DeviceStyle::kSteamDeck, 6410);
  CHECK(monitor.EvaluateActiveStyle(6410) == DeviceStyle::kSteamDeck);
  CHECK(GlobalPromptCompositor().ActiveStyle() == DeviceStyle::kSteamDeck);
}

TEST_CASE("PromptDeviceMonitor user cvar overrides immediately select DeviceStyle and update GlobalPromptCompositor",
          "[layer0][input][device_monitor]") {
  ScopedPromptCvarReset guard;
  REQUIRE(GlobalPromptCompositor().Initialize("assets/glyphs"));

  PromptDeviceMonitor& monitor = GlobalPromptDeviceMonitor();
  monitor.Reset();
  monitor.NotifyKeyboardMouseActivity(1000);
  CHECK(monitor.Poll(1000, true) == DeviceStyle::kKeyboard);
  CHECK(GlobalPromptCompositor().ActiveStyle() == DeviceStyle::kKeyboard);

  const std::pair<const char*, DeviceStyle> kModes[] = {
      {"switch", DeviceStyle::kSwitch},
      {"steamdeck", DeviceStyle::kSteamDeck},
      {"steamcontroller", DeviceStyle::kSteamController},
      {"playstation", DeviceStyle::kPlayStation},
      {"xbox", DeviceStyle::kXbox},
      {"keyboard", DeviceStyle::kKeyboard},
      {"original", DeviceStyle::kOriginal},
  };

  // Test gow_button_prompts overrides.
  for (const auto& [mode_str, expected_style] : kModes) {
    INFO("Testing gow_button_prompts=" << mode_str);
    REXCVAR_SET(gow_button_prompts, mode_str);
    CHECK(monitor.Poll(2000) == expected_style);
    CHECK(GlobalPromptCompositor().ActiveStyle() == expected_style);
  }

  // Returning gow_button_prompts to "auto" restores the live auto-detected style (kKeyboard from t=1000).
  REXCVAR_SET(gow_button_prompts, "auto");
  CHECK(monitor.Poll(2100) == DeviceStyle::kKeyboard);
  CHECK(GlobalPromptCompositor().ActiveStyle() == DeviceStyle::kKeyboard);

  // Test gow2_button_prompts overrides.
  for (const auto& [mode_str, expected_style] : kModes) {
    INFO("Testing gow2_button_prompts=" << mode_str);
    REXCVAR_SET(gow2_button_prompts, mode_str);
    CHECK(monitor.Poll(2200) == expected_style);
    CHECK(GlobalPromptCompositor().ActiveStyle() == expected_style);
  }

  // Returning gow2_button_prompts to "auto" after gamepad activity selects the gamepad style.
  monitor.NotifyGamepadActivity(DeviceStyle::kPlayStation, 2500);
  REXCVAR_SET(gow2_button_prompts, "auto");
  CHECK(monitor.Poll(2500) == DeviceStyle::kPlayStation);
  CHECK(GlobalPromptCompositor().ActiveStyle() == DeviceStyle::kPlayStation);
}

TEST_CASE("PromptDeviceMonitor DetectConnectedRawInputPadStyle identifies connected controller hardware",
          "[layer0][input][device_monitor]") {
  ScopedPromptCvarReset guard;
  PromptDeviceMonitor monitor;
  const auto style = PromptDeviceMonitor::DetectConnectedRawInputPadStyle();
  if (style) {
    CHECK((*style == DeviceStyle::kPlayStation || *style == DeviceStyle::kXbox ||
           *style == DeviceStyle::kSwitch || *style == DeviceStyle::kSteamDeck ||
           *style == DeviceStyle::kSteamController));
  }
}

TEST_CASE("PromptDeviceMonitor IsKeyboardMouseInUse handles autorepeat and switching back to gamepad",
          "[layer0][input][device_monitor]") {
  ScopedPromptCvarReset guard;
  REQUIRE(GlobalPromptCompositor().Initialize("assets/glyphs"));

  PromptDeviceMonitor& monitor = GlobalPromptDeviceMonitor();
  monitor.Reset();

  // 1. Initially controller active
  monitor.NotifyGamepadActivity(DeviceStyle::kPlayStation, 1000);
  CHECK(monitor.EvaluateActiveStyle(1000) == DeviceStyle::kPlayStation);
  CHECK_FALSE(monitor.IsKeyboardMouseInUse(1000));

  // 2. Player presses 'W' to walk forward (VK 0x57)
  monitor.NotifyKeyDown(0x57, 2000);
  CHECK(monitor.IsKeyboardMouseInUse(2000));

  // Windows sends repeated WM_KEYDOWN events at ~30Hz while 'W' is held
  for (uint64_t t = 2030; t <= 3000; t += 30) {
    monitor.NotifyKeyDown(0x57, t);
    CHECK(monitor.IsKeyboardMouseInUse(t));
  }

  // Monitor evaluated at t=3000 (after debounce) has switched to kKeyboard
  CHECK(monitor.EvaluateActiveStyle(3000) == DeviceStyle::kKeyboard);

  // Player releases 'W' -> single WM_KEYUP event
  monitor.NotifyKeyUp(0x57, 3050);

  // Within the debounce window after key release
  CHECK(monitor.IsKeyboardMouseInUse(3050));
  CHECK(monitor.IsKeyboardMouseInUse(3200));

  // After 300ms since last key activity (last key down was 2990ms, so 3290ms+ is inactive)
  CHECK_FALSE(monitor.IsKeyboardMouseInUse(3350));

  // 3. Player uses DualSense controller at t=3400ms:
  // Controller input must register cleanly and switch back to PlayStation!
  monitor.NotifyGamepadActivity(DeviceStyle::kPlayStation, 3400);
  CHECK(monitor.EvaluateActiveStyle(3400) == DeviceStyle::kPlayStation);
  CHECK(GlobalPromptCompositor().ActiveStyle() == DeviceStyle::kPlayStation);
}

TEST_CASE("Auto mode shows the game's own prompts for an Xbox pad and switches instantly both ways",
          "[layer0][input][device_monitor]") {
  ScopedPromptCvarReset guard;
  REQUIRE(GlobalPromptCompositor().Initialize("assets/glyphs"));
  PromptDeviceMonitor monitor;
  monitor.Reset();

  // Xbox pad: no overlay at all (the game's Xbox 360 art), and nothing was built for it.
  monitor.NotifyGamepadActivity(DeviceStyle::kXbox, 1000);
  CHECK(monitor.EvaluateActiveStyle(1000) == DeviceStyle::kOriginal);
  CHECK(GlobalPromptCompositor().ActiveStyle() == DeviceStyle::kOriginal);
  CHECK(monitor.PublishedStyle() == DeviceStyle::kOriginal);
  CHECK(monitor.PollFromBind() == DeviceStyle::kOriginal);

  // A key after the debounce: key caps; a pad button after the debounce: back to the game's own.
  monitor.NotifyKeyDown(0x57, 1500);
  CHECK(monitor.EvaluateActiveStyle(1500) == DeviceStyle::kKeyboard);
  CHECK(monitor.PublishedStyle() == DeviceStyle::kKeyboard);
  monitor.NotifyKeyUp(0x57, 1510);
  monitor.NotifyGamepadActivity(DeviceStyle::kXbox, 2000);
  CHECK(monitor.EvaluateActiveStyle(2000) == DeviceStyle::kOriginal);

  // Other pads get their own glyphs.
  monitor.NotifyGamepadActivity(DeviceStyle::kPlayStation, 2100);
  CHECK(monitor.EvaluateActiveStyle(2100) == DeviceStyle::kPlayStation);

  // An explicit "xbox" still draws the Kenney Xbox glyphs; "original" never draws anything.
  REXCVAR_SET(gow_button_prompts, "xbox");
  CHECK(monitor.Poll(2200) == DeviceStyle::kXbox);
  REXCVAR_SET(gow_button_prompts, "original");
  CHECK(monitor.Poll(2300) == DeviceStyle::kOriginal);
}

TEST_CASE("GitHub #104: with keep-pad on, keyboard and mouse never bring the key caps in auto mode",
          "[layer0][input][device_monitor]") {
  ScopedPromptCvarReset guard;
  REQUIRE(GlobalPromptCompositor().Initialize("assets/glyphs"));
  REXCVAR_SET(gow_button_prompts_keep_pad, true);
  PromptDeviceMonitor monitor;
  monitor.Reset();

  monitor.NotifyGamepadActivity(DeviceStyle::kPlayStation, 1000);
  CHECK(monitor.EvaluateActiveStyle(1000) == DeviceStyle::kPlayStation);
  monitor.NotifyKeyboardMouseActivity(2000);
  monitor.NotifyMouseMove(40, 0, 2100);
  CHECK(monitor.EvaluateActiveStyle(2500) == DeviceStyle::kPlayStation);

  // An Xbox pad with keyboard use: still the game's own prompts, never key caps.
  monitor.NotifyGamepadActivity(DeviceStyle::kXbox, 3000);
  monitor.NotifyKeyboardMouseActivity(4000);
  CHECK(monitor.EvaluateActiveStyle(4500) == DeviceStyle::kOriginal);

  // An explicit "keyboard" is the player's own choice and still wins.
  REXCVAR_SET(gow_button_prompts, "keyboard");
  CHECK(monitor.Poll(5000) == DeviceStyle::kKeyboard);
}

// 0.4.9: DS4Windows (ViGEm Xbox 360) / Steam Input virtual pads beside a real Sony pad keep the PlayStation prompts;
// a real Xbox One / Series pad still wins over a Sony device (GitHub #249).
TEST_CASE("RawInput: virtual XInput pads don't hide a real PlayStation pad", "[device_monitor]") {
  const std::wstring ds4 = L"\\\\?\\HID#VID_054C&PID_09CC#7&1";
  const std::wstring vigem360 = L"\\\\?\\HID#VID_045E&PID_028E&IG_00#1&2";
  const std::wstring steamvirt = L"\\\\?\\HID#VID_28DE&PID_11FF&IG_00#3&4";
  const std::wstring series = L"\\\\?\\HID#VID_045E&PID_0B12&IG_00#5&6";
  using V = std::vector<std::wstring>;
  CHECK(PromptDeviceMonitor::ClassifyRawInputPadNames(V{vigem360, ds4}) == DeviceStyle::kPlayStation);
  CHECK(PromptDeviceMonitor::ClassifyRawInputPadNames(V{steamvirt, ds4}) == DeviceStyle::kPlayStation);
  CHECK(PromptDeviceMonitor::ClassifyRawInputPadNames(V{series, ds4}) == DeviceStyle::kXbox);
  CHECK(PromptDeviceMonitor::ClassifyRawInputPadNames(V{vigem360}) == DeviceStyle::kXbox);
  CHECK(PromptDeviceMonitor::ClassifyRawInputPadNames(V{ds4}) == DeviceStyle::kPlayStation);
  CHECK_FALSE(PromptDeviceMonitor::ClassifyRawInputPadNames(V{}).has_value());
}
