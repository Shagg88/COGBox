// Layer 0 (game-free): multi-hardware controller identification (Xbox, PlayStation, Switch, Steam Deck,
// Steam Controller), keyboard/mouse vs gamepad activity monitor with mouse deadzone & debounce, and live
// auto-switching of 4x prompt overlays in TextureCache.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <xenos/renderer/prompt_overlay_compositor.h>

REXCVAR_DECLARE(std::string, gow_button_prompts);
REXCVAR_DECLARE(std::string, gow2_button_prompts);

namespace rex::ui {
class Window;
}  // namespace rex::ui

namespace rex::input {
class InputSystem;
}  // namespace rex::input

namespace xenos::input {

using DeviceStyle = ::xenos::renderer::prompt::DeviceStyle;
using ::xenos::renderer::prompt::DeviceStyleName;

class PromptDeviceMonitor {
 public:
  static constexpr int32_t kMouseMoveDeadzonePx = 5;
  static constexpr uint64_t kDebounceWindowMs = 250;
  static constexpr uint64_t kPollIntervalMs = 50;

  PromptDeviceMonitor();
  ~PromptDeviceMonitor();

  // Classifies a physical or virtual gamepad from its SDL_GamepadType, USB Vendor/Product IDs, device name, and
  // Steam Deck environment flag.
  DeviceStyle ClassifyGamepad(int sdl_type, uint16_t vid, uint16_t pid, std::string_view name,
                              bool steam_deck_env = false) const;

  // Parses a user cvar mode string ("auto", "xbox", "playstation", "switch", "steamdeck", "steamcontroller",
  // "keyboard", "original"). Returns std::nullopt for "auto" or unrecognized strings.
  static std::optional<DeviceStyle> ParseModeOption(std::string_view mode);

  // Checks if the process is running in a Steam Deck environment (SteamDeck=1 or STEAM_DECK=1).
  static bool IsSteamDeckEnvironment();

  // Resets activity timestamps, detected hardware state, and debounce state back to defaults.
  void Reset();

  // Records keyboard button or mouse button/wheel activity at `timestamp_ms`.
  void NotifyKeyboardMouseActivity(uint64_t timestamp_ms = 0);

  // Records mouse movement delta `(dx, dy)` at `timestamp_ms`. Only triggers KBM activity when
  // `std::abs(dx) > 5 || std::abs(dy) > 5` (5-pixel deadzone).
  void NotifyMouseMove(int32_t dx, int32_t dy, uint64_t timestamp_ms = 0);

  // Records physical gamepad button/trigger/stick activity for a classified controller profile at `timestamp_ms`.
  void NotifyGamepadActivity(DeviceStyle pad_style, uint64_t timestamp_ms = 0);

  // Updates the default controller profile when a gamepad is enumerated before any input event is pressed.
  void SetDefaultGamepadStyle(DeviceStyle pad_style);

  // Returns the current or detected gamepad style (PlayStation, Switch, Steam Deck, Steam Controller, Xbox).
  DeviceStyle GetDefaultGamepadStyle() const;

  // Evaluates the target DeviceStyle at `now_ms`, honoring cvar overrides ("gow_button_prompts" /
  // "gow2_button_prompts") and the 250 ms cross-device debounce window. Also syncs GlobalPromptCompositor().
  DeviceStyle EvaluateActiveStyle(uint64_t now_ms = 0) const;

  DeviceStyle GetActiveStyle(uint64_t now_ms = 0) const { return EvaluateActiveStyle(now_ms); }

  // Polls connected SDL3 / XInput hardware, checks cvars & keybinds, and updates GlobalPromptCompositor().
  // Throttled internally to run hardware queries at most once every 50 ms unless `force` is true or a cvar / input
  // notification arrived since the last poll.
  DeviceStyle Poll(uint64_t now_ms = 0, bool force = false);

  // Lightweight entry point called from TextureCache::Bind(). Applies cvar overrides and live input switches
  // without clobbering direct PromptOverlayCompositor::SetActiveStyle() calls in headless unit tests.
  void PollFromBind();

  void SetHardwarePollingEnabled(bool enabled) { hardware_polling_enabled_.store(enabled, std::memory_order_relaxed); }
  bool IsHardwarePollingEnabled() const { return hardware_polling_enabled_.load(std::memory_order_relaxed); }

  // Attaches/detaches rex::input::InputSystem to poll live controllers (DualSense, DualShock, Switch, Steam Deck, Xbox).
  void AttachInputSystem(rex::input::InputSystem* input_system);
  void DetachInputSystem(rex::input::InputSystem* input_system = nullptr);

  // Win32 RawInput device scan for connected gamepad hardware styles (PlayStation, Switch, Steam Deck, Steam Controller, Xbox).
  static std::optional<DeviceStyle> DetectConnectedRawInputPadStyle();

  // Attaches/detaches a WindowInputListener on the game window to track KBM events.
  void AttachWindow(rex::ui::Window* window);
  void DetachWindow(rex::ui::Window* window = nullptr);

 private:
  class WindowWatcher;

  static uint64_t NowMs();
  DeviceStyle DefaultGamepadStyleLocked() const;
  std::optional<DeviceStyle> ResolveForcedStyleLocked() const;
  DeviceStyle EvaluateActiveStyleLocked(uint64_t now_ms) const;
  void PollHardwareLocked(uint64_t now_ms);
  std::string SnapshotKeybindSignature() const;

  mutable std::mutex mutex_;
  uint64_t last_kbm_ms_ = 0;
  uint64_t last_pad_ms_ = 0;
  DeviceStyle last_detected_pad_style_ = DeviceStyle::kXbox;
  bool has_detected_pad_ = false;
  mutable DeviceStyle committed_auto_style_ = DeviceStyle::kXbox;
  mutable bool has_committed_auto_ = false;

  mutable std::string last_gow_cvar_ = "auto";
  mutable std::string last_gow2_cvar_ = "auto";
  mutable std::optional<DeviceStyle> sticky_forced_style_;
  mutable DeviceStyle last_applied_style_ = DeviceStyle::kOriginal;
  mutable bool has_applied_style_ = false;

  uint64_t last_poll_ms_ = 0;
  std::string last_keybind_sig_;
  std::atomic<bool> dirty_{false};
  std::atomic<bool> hardware_polling_enabled_{true};

  rex::ui::Window* attached_window_ = nullptr;
  rex::input::InputSystem* attached_input_system_ = nullptr;
  std::unique_ptr<WindowWatcher> window_watcher_;
};

PromptDeviceMonitor& GlobalPromptDeviceMonitor();

}  // namespace xenos::input

namespace xenos::renderer::prompt {

using ::xenos::input::GlobalPromptDeviceMonitor;
using ::xenos::input::PromptDeviceMonitor;

}  // namespace xenos::renderer::prompt

namespace xenos::renderer {

using ::xenos::input::GlobalPromptDeviceMonitor;
using ::xenos::input::PromptDeviceMonitor;

}  // namespace xenos::renderer
