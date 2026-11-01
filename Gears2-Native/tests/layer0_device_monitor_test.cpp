// Layer 0 unit tests: multi-hardware gamepad classification (Switch, Steam Deck, Steam Controller, PlayStation, Xbox),
// mouse deadzone (> 5 px) & 250 ms cross-device debounce, and live user cvar overrides in PromptDeviceMonitor.
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
    GlobalPromptDeviceMonitor().Reset();
  }
  ~ScopedPromptCvarReset() {
    REXCVAR_SET(gow_button_prompts, "auto");
    REXCVAR_SET(gow2_button_prompts, "auto");
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

TEST_CASE("PromptDeviceMonitor enforces 5-pixel mouse deadzone and 250 ms cross-device debounce",
          "[layer0][input][device_monitor]") {
  ScopedPromptCvarReset guard;
  GlobalPromptCompositor().Initialize("assets/glyphs");

  PromptDeviceMonitor monitor;
  monitor.Reset();

  // Initial gamepad input on PlayStation controller at t = 1000 ms selects kPlayStation immediately.
  monitor.NotifyGamepadActivity(DeviceStyle::kPlayStation, 1000);
  CHECK(monitor.EvaluateActiveStyle(1000) == DeviceStyle::kPlayStation);
  CHECK(GlobalPromptCompositor().ActiveStyle() == DeviceStyle::kPlayStation);

  // Sub-deadzone mouse jitter (|dx| <= 5 && |dy| <= 5) must NOT trigger KBM activity even well after 250 ms.
  monitor.NotifyMouseMove(5, -5, 1500);
  monitor.NotifyMouseMove(0, 3, 1600);
  CHECK(monitor.EvaluateActiveStyle(1600) == DeviceStyle::kPlayStation);

  // Gamepad activity at t = 2000 ms keeps kPlayStation.
  monitor.NotifyGamepadActivity(DeviceStyle::kPlayStation, 2000);
  CHECK(monitor.EvaluateActiveStyle(2000) == DeviceStyle::kPlayStation);

  // Mouse movement exceeding 5-pixel deadzone (dx = 12) at t = 2100 ms (100 ms after last pad input, inside the
  // 250 ms debounce window) must NOT flicker to kKeyboard yet.
  monitor.NotifyMouseMove(12, 2, 2100);
  CHECK(monitor.EvaluateActiveStyle(2100) == DeviceStyle::kPlayStation);
  CHECK(monitor.EvaluateActiveStyle(2249) == DeviceStyle::kPlayStation);

  // Once the 250 ms debounce window from the last gamepad input (2000 + 250 = 2250 ms) expires, it switches cleanly
  // to kKeyboard.
  CHECK(monitor.EvaluateActiveStyle(2250) == DeviceStyle::kKeyboard);
  CHECK(GlobalPromptCompositor().ActiveStyle() == DeviceStyle::kKeyboard);

  // While on kKeyboard (last KBM at 2100 ms), a brief gamepad jitter at t = 2280 ms (180 ms after 2100 ms, < 250 ms)
  // must stay debounced on kKeyboard until 2100 + 250 = 2350 ms.
  monitor.NotifyGamepadActivity(DeviceStyle::kSwitch, 2280);
  CHECK(monitor.EvaluateActiveStyle(2280) == DeviceStyle::kKeyboard);
  CHECK(monitor.EvaluateActiveStyle(2349) == DeviceStyle::kKeyboard);
  CHECK(monitor.EvaluateActiveStyle(2350) == DeviceStyle::kSwitch);
  CHECK(GlobalPromptCompositor().ActiveStyle() == DeviceStyle::kSwitch);

  // Decisive KBM input after >= 250 ms of gamepad silence (t = 3000 ms >= 2280 + 250) switches immediately at t = 3000.
  monitor.NotifyKeyboardMouseActivity(3000);
  CHECK(monitor.EvaluateActiveStyle(3000) == DeviceStyle::kKeyboard);
  CHECK(GlobalPromptCompositor().ActiveStyle() == DeviceStyle::kKeyboard);

  // Decisive Steam Deck input after >= 250 ms of KBM silence (t = 3500 ms >= 3000 + 250) switches immediately.
  monitor.NotifyGamepadActivity(DeviceStyle::kSteamDeck, 3500);
  CHECK(monitor.EvaluateActiveStyle(3500) == DeviceStyle::kSteamDeck);
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
