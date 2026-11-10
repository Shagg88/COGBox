// Layer 0 (game-free): which button prompts to show. Identifies the pad in use (Xbox, PlayStation, Switch, Steam Deck,
// Steam Controller), tells keyboard/mouse use from pad use (mouse dead zone, debounce) and switches the 4x prompt
// overlays live (--gow_button_prompts=auto, the default).
// Contributed by Flippage (COGBox PR #117); portions from gow3-recomp: BSD-3-Clause, Copyright (c) Flippage and the
// gow3-recomp contributors (THIRD_PARTY_NOTICES.md). The rest is our code (MIT).
//
// Threads: all hardware polling (XInput / InputSystem pad reads, RawInput and SDL device scans) and all atlas building
// happen on the monitor's own thread (Start / Stop). Window input events arrive on the UI thread. The render thread
// only reads PublishedStyle() - one atomic load.
#pragma once

#include <atomic>
#include <bitset>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <string_view>
#include <thread>

#include <rex/cvar.h>
#include <xenos/renderer/prompt_overlay_compositor.h>

REXCVAR_DECLARE(std::string, gow_button_prompts);
REXCVAR_DECLARE(std::string, gow2_button_prompts);
REXCVAR_DECLARE(bool, gow_button_prompts_keep_pad);

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
  static constexpr uint64_t kDebounceWindowMs = 250;   // notes/79: the cross-device debounce
  static constexpr uint64_t kKbmRecentMs = 300;        // a key / click counts as "in use" this long
  static constexpr uint64_t kPadPollMs = 16;           // monitor thread: pad reads
  static constexpr uint64_t kHardwareScanMs = 2000;    // monitor thread: RawInput / SDL device classification
  // rc2 (keyboard prompts with an Xbox pad): mouse movement switches to key caps only after this much travel within
  // kMouseTravelWindowMs - sensor noise and a gaming mouse's stream of tiny deltas never do.
  static constexpr int32_t kMouseTravelPx = 40;
  static constexpr uint64_t kMouseTravelWindowMs = 200;

  PromptDeviceMonitor();
  ~PromptDeviceMonitor();

  // Classifies a physical or virtual gamepad from its SDL_GamepadType, USB Vendor/Product IDs, device name, and
  // Steam Deck environment flag.
  DeviceStyle ClassifyGamepad(int sdl_type, uint16_t vid, uint16_t pid, std::string_view name,
                              bool steam_deck_env = false) const;

  // Parses a mode string ("auto", "xbox", "playstation", "switch", "steamdeck", "steamcontroller", "keyboard",
  // "original"). Returns std::nullopt for "auto" or unrecognized strings.
  static std::optional<DeviceStyle> ParseModeOption(std::string_view mode);

  // Checks if the process is running in a Steam Deck environment (SteamDeck=1 or STEAM_DECK=1).
  static bool IsSteamDeckEnvironment();

  // Resets activity timestamps, detected hardware state, and debounce state; re-reads the cvars.
  void Reset();
  // 0.4.9 live settings: the in-game menu changed --gow_button_prompts - read the mode again (the monitor thread
  // prepares a forced style at its next scan, within kHardwareScanMs). Detected pads and activity are kept.
  void ReloadMode();

  // Keyboard / mouse activity (UI thread: window events).
  void NotifyKeyboardMouseActivity(uint64_t timestamp_ms = 0);
  void NotifyKeyDown(uint64_t timestamp_ms = 0);
  void NotifyKeyDown(uint32_t vk, uint64_t timestamp_ms = 0);
  void NotifyKeyUp(uint64_t timestamp_ms = 0);
  void NotifyKeyUp(uint32_t vk, uint64_t timestamp_ms = 0);
  void NotifyMouseDown(uint64_t timestamp_ms = 0);
  void NotifyMouseUp(uint64_t timestamp_ms = 0);
  void ResetInputStates();
  // Game: the monitor thread found no glyph folder. Auto still runs: key caps = the launcher's disk overlays
  // (--gow2_texture_overlay_dir), any pad = the game's own prompts (those overlays hidden).
  void SetGlyphsMissing(bool missing);
  // Only counts mouse movement beyond kMouseMoveDeadzonePx.
  void NotifyMouseMove(int32_t dx, int32_t dy, uint64_t timestamp_ms = 0);

  // Is the player pressing keys / mouse buttons, or did so in the last kKbmRecentMs? Held keys and buttons are only
  // checked with GetAsyncKeyState while the game window is in front (a click in another app is not the game's).
  bool IsKeyboardMouseInUse(uint64_t now_ms = 0) const;

  // Pad activity for a classified pad.
  void NotifyGamepadActivity(DeviceStyle pad_style, uint64_t timestamp_ms = 0);
  // The pad that is connected, before any of its buttons is pressed.
  void SetDefaultGamepadStyle(DeviceStyle pad_style);
  DeviceStyle GetDefaultGamepadStyle() const;

  // Decides the style at `now_ms` (cvar override, else auto with the debounce) and applies it to
  // GlobalPromptCompositor() on the calling thread. Tests and tools; the game's monitor thread does this itself.
  DeviceStyle EvaluateActiveStyle(uint64_t now_ms = 0);
  DeviceStyle GetActiveStyle(uint64_t now_ms = 0) { return EvaluateActiveStyle(now_ms); }
  // Re-reads the cvars, then EvaluateActiveStyle (rebuilding the key caps if the keybind cvars changed).
  DeviceStyle Poll(uint64_t now_ms = 0, bool force = false);

  // The style last applied to the compositor. Render thread safe: one relaxed atomic load, nothing else.
  DeviceStyle PublishedStyle() const noexcept { return published_style_.load(std::memory_order_relaxed); }
  // Kept for TextureCache::Bind's call: the same single atomic load (no polling, no locks).
  DeviceStyle PollFromBind() const noexcept { return PublishedStyle(); }

  // Game: starts the monitor thread (UI thread, once the window and input system exist). With
  // --gow_button_prompts=original nothing is started and nothing is ever drawn over the game's prompts.
  void Start(rex::ui::Window* window, rex::input::InputSystem* input_system);
  void Stop();

  void AttachInputSystem(rex::input::InputSystem* input_system);
  void DetachInputSystem(rex::input::InputSystem* input_system = nullptr);
  void AttachWindow(rex::ui::Window* window);
  void DetachWindow(rex::ui::Window* window = nullptr);

  // Win32 RawInput device scan for connected gamepad hardware styles. Monitor thread / tests only (it is slow).
  static std::optional<DeviceStyle> DetectConnectedRawInputPadStyle();
  // The style from the RawInput gamepad device names (upper case). Pure, for tests.
  static std::optional<DeviceStyle> ClassifyRawInputPadNames(const std::vector<std::wstring>& upper_names);
  // RawInput, then SDL3's gamepad list (type / VID / PID / name; no gamepad is opened). Monitor thread / tests only.
  std::optional<DeviceStyle> ScanConnectedPadStyle() const;

  // Steady clock timestamp in milliseconds.
  static uint64_t NowMs();

 private:
  class WindowWatcher;
  enum class Mode : uint8_t { kAuto, kForced, kOriginal };

  void ReadModeLocked();
  void NoteKeyboardMouseLocked(uint64_t timestamp_ms, const char* reason);
  DeviceStyle EvaluateActiveStyleLocked(uint64_t now_ms);
  bool HasAnyKeyDownLocked(bool check_async) const;
  std::string SnapshotKeybindSignature() const;
  void ThreadMain();

  mutable std::mutex mutex_;
  // Mode (from the cvars; read at Start / Reset / Poll, not per frame).
  Mode mode_ = Mode::kAuto;
  DeviceStyle forced_style_ = DeviceStyle::kOriginal;
  bool keyboard_allowed_ = true;  // GitHub #104: false = keep the pad prompts while keyboard and mouse is on

  // Activity.
  uint64_t last_kbm_ms_ = 0;
  uint64_t last_pad_ms_ = 0;
  DeviceStyle last_detected_pad_style_ = DeviceStyle::kXbox;
  bool has_detected_pad_ = false;
  DeviceStyle committed_auto_style_ = DeviceStyle::kXbox;
  bool has_committed_auto_ = false;
  mutable std::bitset<256> keys_down_;
  bool mouse_down_ = false;
  uint64_t mouse_travel_start_ms_ = 0;
  int32_t mouse_travel_px_ = 0;
  std::string pending_reason_ = "start";  // why the auto style last changed (logged with the change)
  bool glyphs_missing_ = false;

  // Applied state.
  DeviceStyle last_applied_style_ = DeviceStyle::kOriginal;
  bool last_applied_hide_ = false;
  bool has_applied_style_ = false;
  std::string last_keybind_sig_;
  std::atomic<DeviceStyle> published_style_{DeviceStyle::kOriginal};

  // Game wiring.
  rex::ui::Window* attached_window_ = nullptr;  // under mutex_
  std::atomic<rex::input::InputSystem*> attached_input_system_{nullptr};
  std::unique_ptr<WindowWatcher> window_watcher_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::condition_variable stop_cv_;
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
