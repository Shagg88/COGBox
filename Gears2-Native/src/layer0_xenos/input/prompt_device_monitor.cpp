// Layer 0 (game-free): multi-hardware controller identification (Xbox, PlayStation, Switch, Steam Deck,
// Steam Controller), keyboard/mouse vs gamepad activity monitor with mouse deadzone & debounce, and live
// auto-switching of 4x prompt overlays in TextureCache.
#include <xenos/input/prompt_device_monitor.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <Xinput.h>
#endif

REXCVAR_DEFINE_STRING(gow_button_prompts, "auto", "Input",
                      "Button prompt glyph style: auto, xbox, playstation, switch, steamdeck, steamcontroller, keyboard, original");
REXCVAR_DEFINE_STRING(gow2_button_prompts, "auto", "Input",
                      "Button prompt glyph style: auto, xbox, playstation, switch, steamdeck, steamcontroller, keyboard, original");

namespace xenos::input {

namespace {

std::string ToLowerTrim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  std::string out(s.size(), '\0');
  for (size_t i = 0; i < s.size(); ++i) {
    out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
  }
  return out;
}

bool ContainsCI(std::string_view haystack, std::string_view needle_lower) {
  if (haystack.empty() || needle_lower.empty() || needle_lower.size() > haystack.size()) return false;
  return std::search(haystack.begin(), haystack.end(), needle_lower.begin(), needle_lower.end(),
                     [](char a, char b) {
                       return std::tolower(static_cast<unsigned char>(a)) == static_cast<unsigned char>(b);
                     }) != haystack.end();
}

const char* const kKeybindCvars[] = {
    "keybind_right_trigger",
    "keybind_left_trigger",
    "keybind_a",
    "keybind_b",
    "keybind_x",
    "keybind_y",
    "keybind_right_shoulder",
    "keybind_left_shoulder",
    "keybind_dpad_left",
    "keybind_dpad_right",
    "keybind_dpad_up",
    "keybind_dpad_down",
    "keybind_rstick_press",
    "keybind_lstick_press",
    "keybind_start",
    "keybind_back",
};

#ifdef _WIN32
struct SdlGamepadApi {
  using SDL_JoystickID = uint32_t;
  struct SDL_Gamepad;

  using GetGamepadsFn = SDL_JoystickID*(__cdecl*)(int* count);
  using FreeFn = void(__cdecl*)(void* mem);
  using GetGamepadFromIDFn = SDL_Gamepad*(__cdecl*)(SDL_JoystickID instance_id);
  using OpenGamepadFn = SDL_Gamepad*(__cdecl*)(SDL_JoystickID instance_id);
  using GetGamepadTypeForIDFn = int(__cdecl*)(SDL_JoystickID instance_id);
  using GetGamepadVendorForIDFn = uint16_t(__cdecl*)(SDL_JoystickID instance_id);
  using GetGamepadProductForIDFn = uint16_t(__cdecl*)(SDL_JoystickID instance_id);
  using GetGamepadNameForIDFn = const char*(__cdecl*)(SDL_JoystickID instance_id);
  using GetGamepadButtonFn = bool(__cdecl*)(SDL_Gamepad* gamepad, int button);
  using GetGamepadAxisFn = int16_t(__cdecl*)(SDL_Gamepad* gamepad, int axis);

  HMODULE mod = nullptr;
  GetGamepadsFn GetGamepads = nullptr;
  FreeFn Free = nullptr;
  GetGamepadFromIDFn GetGamepadFromID = nullptr;
  OpenGamepadFn OpenGamepad = nullptr;
  GetGamepadTypeForIDFn GetGamepadTypeForID = nullptr;
  GetGamepadVendorForIDFn GetGamepadVendorForID = nullptr;
  GetGamepadProductForIDFn GetGamepadProductForID = nullptr;
  GetGamepadNameForIDFn GetGamepadNameForID = nullptr;
  GetGamepadButtonFn GetGamepadButton = nullptr;
  GetGamepadAxisFn GetGamepadAxis = nullptr;

  bool Resolve() {
    HMODULE current = GetModuleHandleW(L"SDL3.dll");
    if (!current) return false;
    if (mod == current && GetGamepads) return true;
    mod = current;
    GetGamepads = reinterpret_cast<GetGamepadsFn>(GetProcAddress(mod, "SDL_GetGamepads"));
    Free = reinterpret_cast<FreeFn>(GetProcAddress(mod, "SDL_free"));
    GetGamepadFromID = reinterpret_cast<GetGamepadFromIDFn>(GetProcAddress(mod, "SDL_GetGamepadFromID"));
    OpenGamepad = reinterpret_cast<OpenGamepadFn>(GetProcAddress(mod, "SDL_OpenGamepad"));
    GetGamepadTypeForID = reinterpret_cast<GetGamepadTypeForIDFn>(GetProcAddress(mod, "SDL_GetGamepadTypeForID"));
    GetGamepadVendorForID = reinterpret_cast<GetGamepadVendorForIDFn>(GetProcAddress(mod, "SDL_GetGamepadVendorForID"));
    GetGamepadProductForID =
        reinterpret_cast<GetGamepadProductForIDFn>(GetProcAddress(mod, "SDL_GetGamepadProductForID"));
    GetGamepadNameForID = reinterpret_cast<GetGamepadNameForIDFn>(GetProcAddress(mod, "SDL_GetGamepadNameForID"));
    GetGamepadButton = reinterpret_cast<GetGamepadButtonFn>(GetProcAddress(mod, "SDL_GetGamepadButton"));
    GetGamepadAxis = reinterpret_cast<GetGamepadAxisFn>(GetProcAddress(mod, "SDL_GetGamepadAxis"));
    return GetGamepads != nullptr && Free != nullptr;
  }
};

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

XInputGetStateFn ResolveXInputGetState() {
  static bool resolved = false;
  static XInputGetStateFn fn = nullptr;
  if (!resolved) {
    resolved = true;
    HMODULE xi = GetModuleHandleW(L"xinput1_4.dll");
    if (!xi) xi = GetModuleHandleW(L"xinput9_1_0.dll");
    if (!xi) xi = LoadLibraryW(L"xinput1_4.dll");
    if (!xi) xi = LoadLibraryW(L"xinput9_1_0.dll");
    if (xi) {
      fn = reinterpret_cast<XInputGetStateFn>(GetProcAddress(xi, "XInputGetState"));
    }
  }
  return fn;
}
#endif

}  // namespace

class PromptDeviceMonitor::WindowWatcher final : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
 public:
  explicit WindowWatcher(PromptDeviceMonitor& monitor) : monitor_(monitor) {}

  void OnKeyDown(rex::ui::KeyEvent&) override { monitor_.NotifyKeyboardMouseActivity(PromptDeviceMonitor::NowMs()); }

  void OnMouseDown(rex::ui::MouseEvent&) override {
    monitor_.NotifyKeyboardMouseActivity(PromptDeviceMonitor::NowMs());
  }

  void OnMouseWheel(rex::ui::MouseEvent&) override {
    monitor_.NotifyKeyboardMouseActivity(PromptDeviceMonitor::NowMs());
  }

  void OnMouseMove(rex::ui::MouseEvent& e) override {
    int32_t dx = 0;
    int32_t dy = 0;
    if (e.dx() != 0.0f || e.dy() != 0.0f) {
      dx = static_cast<int32_t>(std::lround(e.dx()));
      dy = static_cast<int32_t>(std::lround(e.dy()));
    } else if (has_last_pos_) {
      dx = e.x() - last_x_;
      dy = e.y() - last_y_;
    }
    last_x_ = e.x();
    last_y_ = e.y();
    has_last_pos_ = true;
    monitor_.NotifyMouseMove(dx, dy, PromptDeviceMonitor::NowMs());
  }

  void OnClosing(rex::ui::UIEvent& e) override { monitor_.DetachWindow(e.target()); }

 private:
  PromptDeviceMonitor& monitor_;
  int32_t last_x_ = 0;
  int32_t last_y_ = 0;
  bool has_last_pos_ = false;
};

PromptDeviceMonitor::PromptDeviceMonitor() {
  last_detected_pad_style_ = DefaultGamepadStyleLocked();
  committed_auto_style_ = last_detected_pad_style_;
}

PromptDeviceMonitor::~PromptDeviceMonitor() { DetachWindow(nullptr); }

uint64_t PromptDeviceMonitor::NowMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool PromptDeviceMonitor::IsSteamDeckEnvironment() {
#ifdef _WIN32
  char buf[16] = {};
  if (GetEnvironmentVariableA("SteamDeck", buf, sizeof(buf)) == 1 && buf[0] == '1') return true;
  if (GetEnvironmentVariableA("STEAM_DECK", buf, sizeof(buf)) == 1 && buf[0] == '1') return true;
  return false;
#else
  if (const char* sd = std::getenv("SteamDeck")) {
    if (std::strcmp(sd, "1") == 0) return true;
  }
  if (const char* sd2 = std::getenv("STEAM_DECK")) {
    if (std::strcmp(sd2, "1") == 0) return true;
  }
  return false;
#endif
}

DeviceStyle PromptDeviceMonitor::ClassifyGamepad(int sdl_type, uint16_t vid, uint16_t pid, std::string_view name,
                                                 bool steam_deck_env) const {
  const bool on_deck = steam_deck_env || IsSteamDeckEnvironment();

  // 1. Steam Deck: Valve VID 0x28DE + PID 0x1205, or "Steam Deck" in name, or "Steam Virtual Gamepad" on Deck.
  if ((vid == 0x28DE && pid == 0x1205) || ContainsCI(name, "steam deck") ||
      (on_deck && ContainsCI(name, "steam virtual gamepad"))) {
    return DeviceStyle::kSteamDeck;
  }

  // 2. Steam Controller: Valve VID 0x28DE + PID 0x1102 / 0x1142 / 0x1106, or "Steam Controller" in name.
  if ((vid == 0x28DE && (pid == 0x1102 || pid == 0x1142 || pid == 0x1106)) || ContainsCI(name, "steam controller")) {
    return DeviceStyle::kSteamController;
  }

  // 3. Nintendo Switch: SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO (7), JOYCON_LEFT (8), JOYCON_RIGHT (9), JOYCON_PAIR (10),
  // Nintendo VID 0x057E, or Switch/Joy-Con/Pro Controller name (when not Microsoft/Sony VID).
  if ((sdl_type >= 7 && sdl_type <= 10) || vid == 0x057E ||
      (vid != 0x045E && vid != 0x054C &&
       (ContainsCI(name, "switch") || ContainsCI(name, "joy-con") || ContainsCI(name, "joycon") ||
        ContainsCI(name, "pro controller")))) {
    return DeviceStyle::kSwitch;
  }

  // 4. PlayStation: SDL_GAMEPAD_TYPE_PS3 (4), PS4 (5), PS5 (6), Sony VID 0x054C, or DualShock/DualSense/PS name.
  if ((sdl_type >= 4 && sdl_type <= 6) || vid == 0x054C || ContainsCI(name, "dualshock") ||
      ContainsCI(name, "dualsense") || ContainsCI(name, "playstation") || ContainsCI(name, "ps4") ||
      ContainsCI(name, "ps5")) {
    return DeviceStyle::kPlayStation;
  }

  // 5. Steam Deck Environment Fallback: when running on Steam Deck and controller didn't match Switch/PS/SteamController.
  if (on_deck && (vid != 0x045E || steam_deck_env)) {
    return DeviceStyle::kSteamDeck;
  }

  // 6. Default controller profile: Xbox.
  return DeviceStyle::kXbox;
}

std::optional<DeviceStyle> PromptDeviceMonitor::ParseModeOption(std::string_view mode) {
  const std::string m = ToLowerTrim(mode);
  if (m.empty() || m == "auto") return std::nullopt;
  if (m == "xbox" || m == "xinput") return DeviceStyle::kXbox;
  if (m == "playstation" || m == "ps" || m == "ps4" || m == "ps5" || m == "dualsense" || m == "dualshock") {
    return DeviceStyle::kPlayStation;
  }
  if (m == "switch" || m == "nintendo" || m == "procontroller" || m == "joycon") return DeviceStyle::kSwitch;
  if (m == "steamdeck" || m == "deck" || m == "steam_deck") return DeviceStyle::kSteamDeck;
  if (m == "steamcontroller" || m == "steam" || m == "steam_controller") return DeviceStyle::kSteamController;
  if (m == "keyboard" || m == "kbm" || m == "mnk" || m == "mouse") return DeviceStyle::kKeyboard;
  if (m == "original" || m == "xbox360" || m == "off" || m == "none" || m == "default") return DeviceStyle::kOriginal;
  return std::nullopt;
}

DeviceStyle PromptDeviceMonitor::DefaultGamepadStyleLocked() const {
  if (IsSteamDeckEnvironment()) return DeviceStyle::kSteamDeck;
  if (rex::cvar::Query<bool>("cogbox_menu_ps")) return DeviceStyle::kPlayStation;
  return DeviceStyle::kXbox;
}

void PromptDeviceMonitor::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  last_kbm_ms_ = 0;
  last_pad_ms_ = 0;
  has_detected_pad_ = false;
  last_detected_pad_style_ = DefaultGamepadStyleLocked();
  committed_auto_style_ = last_detected_pad_style_;
  has_committed_auto_ = false;
  last_gow_cvar_ = REXCVAR_GET(gow_button_prompts);
  last_gow2_cvar_ = REXCVAR_GET(gow2_button_prompts);
  sticky_forced_style_ = ParseModeOption(last_gow_cvar_);
  if (!sticky_forced_style_) {
    sticky_forced_style_ = ParseModeOption(last_gow2_cvar_);
  }
  has_applied_style_ = false;
  last_poll_ms_ = 0;
  dirty_.store(false, std::memory_order_release);
}

void PromptDeviceMonitor::NotifyKeyboardMouseActivity(uint64_t timestamp_ms) {
  if (timestamp_ms == 0) timestamp_ms = NowMs();
  std::lock_guard<std::mutex> lock(mutex_);
  last_kbm_ms_ = timestamp_ms;
  if (!has_committed_auto_) {
    committed_auto_style_ = DeviceStyle::kKeyboard;
    has_committed_auto_ = true;
  } else if (committed_auto_style_ != DeviceStyle::kKeyboard) {
    if (last_pad_ms_ == 0 || timestamp_ms >= last_pad_ms_ + kDebounceWindowMs) {
      committed_auto_style_ = DeviceStyle::kKeyboard;
    }
  }
  dirty_.store(true, std::memory_order_release);
}

void PromptDeviceMonitor::NotifyMouseMove(int32_t dx, int32_t dy, uint64_t timestamp_ms) {
  if (std::abs(dx) <= kMouseMoveDeadzonePx && std::abs(dy) <= kMouseMoveDeadzonePx) {
    return;
  }
  NotifyKeyboardMouseActivity(timestamp_ms);
}

void PromptDeviceMonitor::NotifyGamepadActivity(DeviceStyle pad_style, uint64_t timestamp_ms) {
  if (timestamp_ms == 0) timestamp_ms = NowMs();
  std::lock_guard<std::mutex> lock(mutex_);
  last_detected_pad_style_ = pad_style;
  has_detected_pad_ = true;
  last_pad_ms_ = timestamp_ms;
  if (!has_committed_auto_) {
    committed_auto_style_ = pad_style;
    has_committed_auto_ = true;
  } else if (committed_auto_style_ != DeviceStyle::kKeyboard) {
    committed_auto_style_ = pad_style;
  } else {
    if (last_kbm_ms_ == 0 || timestamp_ms >= last_kbm_ms_ + kDebounceWindowMs) {
      committed_auto_style_ = pad_style;
    }
  }
  dirty_.store(true, std::memory_order_release);
}

void PromptDeviceMonitor::SetDefaultGamepadStyle(DeviceStyle pad_style) {
  std::lock_guard<std::mutex> lock(mutex_);
  has_detected_pad_ = true;
  if (last_pad_ms_ == 0) {
    last_detected_pad_style_ = pad_style;
    if (!has_committed_auto_ || committed_auto_style_ != DeviceStyle::kKeyboard) {
      committed_auto_style_ = pad_style;
    }
    dirty_.store(true, std::memory_order_release);
  }
}

std::optional<DeviceStyle> PromptDeviceMonitor::ResolveForcedStyleLocked() const {
  const std::string& gow_val = REXCVAR_GET(gow_button_prompts);
  const std::string& gow2_val = REXCVAR_GET(gow2_button_prompts);

  const bool gow_changed = (gow_val != last_gow_cvar_);
  const bool gow2_changed = (gow2_val != last_gow2_cvar_);

  if (gow_changed || gow2_changed) {
    if (gow_changed && !gow2_changed) {
      last_gow_cvar_ = gow_val;
      sticky_forced_style_ = ParseModeOption(gow_val);
      if (!sticky_forced_style_ && ToLowerTrim(gow_val) == "auto" && ToLowerTrim(gow2_val) != "auto") {
        REXCVAR_SET(gow2_button_prompts, "auto");
        last_gow2_cvar_ = "auto";
      }
    } else if (gow2_changed && !gow_changed) {
      last_gow2_cvar_ = gow2_val;
      sticky_forced_style_ = ParseModeOption(gow2_val);
      if (!sticky_forced_style_ && ToLowerTrim(gow2_val) == "auto" && ToLowerTrim(gow_val) != "auto") {
        REXCVAR_SET(gow_button_prompts, "auto");
        last_gow_cvar_ = "auto";
      }
    } else {
      last_gow_cvar_ = gow_val;
      last_gow2_cvar_ = gow2_val;
      sticky_forced_style_ = ParseModeOption(gow_val);
      if (!sticky_forced_style_) {
        sticky_forced_style_ = ParseModeOption(gow2_val);
      }
    }
  } else if (!sticky_forced_style_) {
    if (auto p1 = ParseModeOption(gow_val)) {
      sticky_forced_style_ = p1;
    } else if (auto p2 = ParseModeOption(gow2_val)) {
      sticky_forced_style_ = p2;
    }
  }

  if (sticky_forced_style_) {
    return sticky_forced_style_;
  }

  const std::string gow3_val = rex::cvar::GetFlagByName("gow3_button_prompts");
  if (auto p3 = ParseModeOption(gow3_val)) return p3;

  const std::string gowj_val = rex::cvar::GetFlagByName("gowj_button_prompts");
  if (auto pj = ParseModeOption(gowj_val)) return pj;

  return std::nullopt;
}

DeviceStyle PromptDeviceMonitor::EvaluateActiveStyleLocked(uint64_t now_ms) const {
  if (const auto forced = ResolveForcedStyleLocked()) {
    return *forced;
  }

  if (last_kbm_ms_ == 0 && last_pad_ms_ == 0) {
    if (!has_detected_pad_ && rex::cvar::Query<bool>("mnk_mode")) {
      return DeviceStyle::kKeyboard;
    }
    return has_detected_pad_ ? last_detected_pad_style_ : DefaultGamepadStyleLocked();
  }

  const uint64_t eval_ms = (now_ms != 0) ? now_ms : std::max(last_kbm_ms_, last_pad_ms_);
  if (!has_committed_auto_) {
    committed_auto_style_ = (last_kbm_ms_ > last_pad_ms_) ? DeviceStyle::kKeyboard : last_detected_pad_style_;
    has_committed_auto_ = true;
  } else if (committed_auto_style_ == DeviceStyle::kKeyboard && last_pad_ms_ > last_kbm_ms_) {
    if (last_kbm_ms_ == 0 || eval_ms >= last_kbm_ms_ + kDebounceWindowMs) {
      committed_auto_style_ = last_detected_pad_style_;
    }
  } else if (committed_auto_style_ != DeviceStyle::kKeyboard && last_kbm_ms_ > last_pad_ms_) {
    if (last_pad_ms_ == 0 || eval_ms >= last_pad_ms_ + kDebounceWindowMs) {
      committed_auto_style_ = DeviceStyle::kKeyboard;
    }
  } else if (committed_auto_style_ != DeviceStyle::kKeyboard) {
    committed_auto_style_ = last_detected_pad_style_;
  }

  return committed_auto_style_;
}

DeviceStyle PromptDeviceMonitor::EvaluateActiveStyle(uint64_t now_ms) const {
  DeviceStyle target = DeviceStyle::kXbox;
  bool should_apply = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    target = EvaluateActiveStyleLocked(now_ms);
    if (!has_applied_style_ || last_applied_style_ != target ||
        ::xenos::renderer::prompt::GlobalPromptCompositor().ActiveStyle() != target) {
      last_applied_style_ = target;
      has_applied_style_ = true;
      should_apply = true;
    }
  }
  if (should_apply) {
    ::xenos::renderer::prompt::GlobalPromptCompositor().SetActiveStyle(target);
  }
  return target;
}

void PromptDeviceMonitor::PollHardwareLocked(uint64_t now_ms) {
#ifdef _WIN32
  bool any_sdl_gamepad = false;
  static SdlGamepadApi sdl;
  if (sdl.Resolve()) {
    int count = 0;
    SdlGamepadApi::SDL_JoystickID* ids = sdl.GetGamepads(&count);
    if (ids && count > 0) {
      any_sdl_gamepad = true;
      for (int i = 0; i < count; ++i) {
        const SdlGamepadApi::SDL_JoystickID id = ids[i];
        const int sdl_type = sdl.GetGamepadTypeForID ? sdl.GetGamepadTypeForID(id) : 0;
        const uint16_t vid = sdl.GetGamepadVendorForID ? sdl.GetGamepadVendorForID(id) : 0;
        const uint16_t pid = sdl.GetGamepadProductForID ? sdl.GetGamepadProductForID(id) : 0;
        const char* raw_name = sdl.GetGamepadNameForID ? sdl.GetGamepadNameForID(id) : "";
        const DeviceStyle pad_style = ClassifyGamepad(sdl_type, vid, pid, raw_name ? raw_name : "");

        if (!has_detected_pad_) {
          has_detected_pad_ = true;
          last_detected_pad_style_ = pad_style;
          if (!has_committed_auto_) {
            committed_auto_style_ = pad_style;
          }
        }

        SdlGamepadApi::SDL_Gamepad* pad = sdl.GetGamepadFromID ? sdl.GetGamepadFromID(id) : nullptr;
        if (!pad && sdl.OpenGamepad) {
          pad = sdl.OpenGamepad(id);
        }
        if (!pad) continue;

        bool active = false;
        if (sdl.GetGamepadButton) {
          for (int b = 0; b <= 20; ++b) {
            if (sdl.GetGamepadButton(pad, b)) {
              active = true;
              break;
            }
          }
        }
        if (!active && sdl.GetGamepadAxis) {
          for (int a = 0; a < 4; ++a) {
            if (std::abs(static_cast<int>(sdl.GetGamepadAxis(pad, a))) > 10000) {
              active = true;
              break;
            }
          }
          if (!active) {
            if (sdl.GetGamepadAxis(pad, 4) > 4000 || sdl.GetGamepadAxis(pad, 5) > 4000) {
              active = true;
            }
          }
        }

        if (active) {
          last_detected_pad_style_ = pad_style;
          has_detected_pad_ = true;
          last_pad_ms_ = now_ms;
          if (!has_committed_auto_ || committed_auto_style_ != DeviceStyle::kKeyboard || last_kbm_ms_ == 0 ||
              now_ms >= last_kbm_ms_ + kDebounceWindowMs) {
            committed_auto_style_ = pad_style;
            has_committed_auto_ = true;
          }
          break;
        }
      }
    }
    if (ids) {
      sdl.Free(ids);
    }
  }

  if (!any_sdl_gamepad) {
    if (XInputGetStateFn xinput_get = ResolveXInputGetState()) {
      for (DWORD i = 0; i < 4; ++i) {
        XINPUT_STATE st = {};
        if (xinput_get(i, &st) == ERROR_SUCCESS) {
          const DeviceStyle pad_style = DefaultGamepadStyleLocked();
          if (!has_detected_pad_) {
            has_detected_pad_ = true;
            last_detected_pad_style_ = pad_style;
          }
          const XINPUT_GAMEPAD& gp = st.Gamepad;
          const bool active = gp.wButtons != 0 || gp.bLeftTrigger > 30 || gp.bRightTrigger > 30 ||
                              std::abs(static_cast<int>(gp.sThumbLX)) > 10000 ||
                              std::abs(static_cast<int>(gp.sThumbLY)) > 10000 ||
                              std::abs(static_cast<int>(gp.sThumbRX)) > 10000 ||
                              std::abs(static_cast<int>(gp.sThumbRY)) > 10000;
          if (active) {
            last_detected_pad_style_ = pad_style;
            last_pad_ms_ = now_ms;
            if (!has_committed_auto_ || committed_auto_style_ != DeviceStyle::kKeyboard || last_kbm_ms_ == 0 ||
                now_ms >= last_kbm_ms_ + kDebounceWindowMs) {
              committed_auto_style_ = pad_style;
              has_committed_auto_ = true;
            }
            break;
          }
        }
      }
    }
  }
#else
  (void)now_ms;
#endif
}

std::string PromptDeviceMonitor::SnapshotKeybindSignature() const {
  std::string sig;
  sig.reserve(256);
  for (const char* k : kKeybindCvars) {
    sig += rex::cvar::GetFlagByName(k);
    sig.push_back('|');
  }
  return sig;
}

DeviceStyle PromptDeviceMonitor::Poll(uint64_t now_ms, bool force) {
  if (now_ms == 0) now_ms = NowMs();

  DeviceStyle target = DeviceStyle::kXbox;
  bool should_apply = false;
  bool should_rebuild_keycaps = false;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool cvar_changed =
        (REXCVAR_GET(gow_button_prompts) != last_gow_cvar_) || (REXCVAR_GET(gow2_button_prompts) != last_gow2_cvar_);
    const bool was_dirty = dirty_.exchange(false, std::memory_order_acq_rel);
    const bool interval_elapsed = (last_poll_ms_ == 0) || (now_ms >= last_poll_ms_ + kPollIntervalMs);

    if (interval_elapsed || force) {
      last_poll_ms_ = now_ms;
      if (attached_window_ != nullptr && hardware_polling_enabled_.load(std::memory_order_relaxed)) {
        PollHardwareLocked(now_ms);
      }
    }

    target = EvaluateActiveStyleLocked(now_ms);

    if (target == DeviceStyle::kKeyboard && (interval_elapsed || force || cvar_changed || was_dirty || last_keybind_sig_.empty())) {
      std::string sig = SnapshotKeybindSignature();
      if (!last_keybind_sig_.empty() && sig != last_keybind_sig_) {
        should_rebuild_keycaps = true;
      }
      last_keybind_sig_ = std::move(sig);
    }

    if (!has_applied_style_ || last_applied_style_ != target ||
        ::xenos::renderer::prompt::GlobalPromptCompositor().ActiveStyle() != target) {
      last_applied_style_ = target;
      has_applied_style_ = true;
      should_apply = true;
    }
  }

  auto& compositor = ::xenos::renderer::prompt::GlobalPromptCompositor();
  if (target != DeviceStyle::kOriginal && !compositor.IsInitialized()) {
    compositor.Initialize();
  }
  if (should_rebuild_keycaps) {
    compositor.RebuildKeyboardOverlays();
  }
  if (should_apply) {
    compositor.SetActiveStyle(target);
  }
  return target;
}

void PromptDeviceMonitor::PollFromBind() {
  const bool cvar_changed =
      (REXCVAR_GET(gow_button_prompts) != last_gow_cvar_) || (REXCVAR_GET(gow2_button_prompts) != last_gow2_cvar_);
  const bool has_cvar_override =
      (REXCVAR_GET(gow_button_prompts) != "auto") || (REXCVAR_GET(gow2_button_prompts) != "auto");
  const bool is_dirty = dirty_.load(std::memory_order_acquire);

  if (!cvar_changed && !has_cvar_override && !is_dirty && attached_window_ == nullptr) {
    // Headless context with no cvar overrides and no monitor input notifications:
    // if GlobalPromptCompositor has not been initialized yet, leave it as-is so unit tests
    // calling PromptOverlayCompositor::SetActiveStyle() directly are not clobbered.
    return;
  }

  const uint64_t now_ms = NowMs();
  if (!cvar_changed && !is_dirty && last_poll_ms_ != 0 && now_ms < last_poll_ms_ + kPollIntervalMs) {
    return;
  }
  Poll(now_ms, cvar_changed || is_dirty);
}

void PromptDeviceMonitor::AttachWindow(rex::ui::Window* window) {
  if (!window) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (attached_window_ == window) return;
  if (attached_window_ && window_watcher_) {
    attached_window_->RemoveInputListener(window_watcher_.get());
    attached_window_->RemoveListener(window_watcher_.get());
  }
  if (!window_watcher_) {
    window_watcher_ = std::make_unique<WindowWatcher>(*this);
  }
  attached_window_ = window;
  attached_window_->AddInputListener(window_watcher_.get(), 0);
  attached_window_->AddListener(window_watcher_.get());
  dirty_.store(true, std::memory_order_release);
}

void PromptDeviceMonitor::DetachWindow(rex::ui::Window* window) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!attached_window_) return;
  if (window != nullptr && window != attached_window_) return;
  if (window_watcher_) {
    attached_window_->RemoveInputListener(window_watcher_.get());
    attached_window_->RemoveListener(window_watcher_.get());
  }
  attached_window_ = nullptr;
}

PromptDeviceMonitor& GlobalPromptDeviceMonitor() {
  static PromptDeviceMonitor instance;
  return instance;
}

}  // namespace xenos::input
