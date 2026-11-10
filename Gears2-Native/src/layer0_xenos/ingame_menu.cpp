// Layer 0 (game-free): COGBox's in-game menu. Back + Start on a controller (or F1) opens it over the game; the game
// In-game menu mouse control and input isolation: based on PR #134 by Flippage (https://github.com/Flippage).
// keeps running underneath, but while the menu is open the controller, keyboard and mouse only work the menu (the SDK's
// input "active" callback says no to the game). The menu shows COGBox's settings for this game, read from the file
// COGBox names in --cogbox_menu (written just before it started the game), with Resume, Restart to apply and Quit game.
// Changes are written to the file COGBox named for them; "Restart to apply" also leaves a restart flag; then both close
// the window the normal way - COGBox applies the changes to its settings and, with the flag, starts the game again.
// Without --cogbox_menu (started by hand) the menu has only Resume and Quit game.
// The controller is read with XInput (loaded at run time, nothing to link) on a small polling thread; while the menu is
// open its buttons are fed to Dear ImGui's gamepad navigation. Our code (MIT).
#include <xenos/dev_tools.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/immediate_drawer.h>
#include <xenos/achievements_data.h>   // ach::ReadFile: friends' profile pictures
#include <xenos/achievements_browser.h>   // achievements part 3: the Achievements page
#include <xenos/achievements_popup_logic.h>   // achievements part 4: popup settings applied at once (LiveCvarFor)
#include <xenos/input/prompt_device_monitor.h>
#include <xenos/menu_input.h>
#include <xenos/menu_look.h>   // 0.4.9: the full-screen look (Noto Sans, the launcher's style)
#include <xenos/renderer/prompt_overlay_compositor.h>   // GitHub #223: XenosSetPromptGame
#include <xenos/achievement_popup_assets.h>   // the in-game text's font (CJK / Cyrillic) when translated
#include <xenos/ui_lang.h>
#include <xenos/achievement_popup.h>
#include <xenos/guide_button.h>   // social step 5
#include <xenos/social_ingame.h>
#include <xenos/social_page.h>
#include <xenos/social_popup.h>
#include <rex/kernel/xam/net/social_ui.h>   // social step 5: the game's own Invite button   // F-8: XenosAchievementSettingsMutex   // --cogbox_ui_lang: XenosTr
#include <rex/cvar.h>
#include <rex/filesystem.h>   // 0.4.9: rex::path_to_utf8
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>   // PR #134: mouse look off while the menu is open
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>
#include <imgui.h>
#include <imgui_internal.h>   // 0.2.0: ShadeVertsLinearColorGradientKeepAlpha

#include <windows.h>
#include <Xinput.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

REXCVAR_DEFINE_STRING(cogbox_menu, "", "Gears2",
                      "COGBox's in-game menu: the file COGBox writes with this game's settings (Back + Start or F1 opens "
                      "the menu; without a file it offers Resume and Quit game only)");
REXCVAR_DEFINE_BOOL(cogbox_menu_enabled, true, "Gears2", "COGBox's in-game menu (Back + Start or F1) on or off");
REXCVAR_DECLARE(bool, guide_button);   // the SDK's input setting (Guide passed to the game)
REXCVAR_DEFINE_BOOL(cogbox_social_guide, true, "Gears2",
                    "Social step 5: the controller's Guide / PS button opens the menu's Social page (when the game runs online)");
REXCVAR_DEFINE_BOOL(cogbox_menu_ps, false, "Gears2", "Job 523: show PlayStation button names in the in-game menu (COGBox sets it for a PlayStation pad)");
REXCVAR_DEFINE_BOOL(cogbox_menu_open_at_start, false, "Gears2",
                    "Test switch: open the in-game menu as soon as the window exists");

REXCVAR_DEFINE_BOOL(cogbox_pad_merge, true, "Gears2",
                    "GitHub #108: with one local player, every controller drives player 1 (off: controller N = player N)");

namespace {

// ---- GitHub #108: which controller is player 1 ----------------------------------------------------------------------
// The SDK's default assignment gives player N the controller that connected Nth. On a Steam Deck the game can see more
// than one pad (the Deck's own controls and Steam Input's virtual pad, or an extra pad), and the first one is not
// always the one that sends input - so player 1 got a pad that stays silent while the launcher (which reads every pad)
// worked. With one local player (all single-player games, and co-op off) every controller now drives player 1, merged
// as the SDK merges keyboard / mouse with a pad. Split-screen (--local_players 2-4) and the second scripted pad keep
// one controller per player. Every change of the device set is logged, so a problem report shows what the game saw.
std::atomic<int> g_real_pads{-1};   // controllers (not keyboard / mouse) the input system knows; -1 = not counted
std::atomic<bool> g_pump_stop{false};
std::thread g_pump;

class SinglePlayerPads final : public rex::input::DeviceAssignment {
 public:
  void OnDevicesChanged(const std::vector<rex::input::DeviceInfo>& devices) override {
    user0_.clear();
    int real = 0;
    for (const auto& d : devices) {
      user0_.push_back(d.id);
      if (!d.synthetic) ++real;
    }
    g_real_pads.store(real);
    std::string list;
    for (const auto& d : devices)
      list += (list.empty() ? "" : " | ") + std::string(d.synthetic ? "[kbm/stand-in] " : "") + "#" +
              std::to_string(d.ordinal) + " \"" + d.name + "\" " + d.guid;
    if (list != last_) {
      last_ = list;
      REXLOG_INFO("cogbox_pads: {} device(s), all drive player 1: {}", devices.size(), list.empty() ? "none" : list);
    }
  }
  void DevicesForUser(uint32_t user_index, std::vector<rex::input::DeviceId>& out) const override {
    out.clear();
    if (user_index == 0) out = user0_;
  }

 private:
  std::vector<rex::input::DeviceId> user0_;
  std::string last_ = "\x01";   // so the first list (even an empty one) is logged
};

// 0.4.9 split-screen: with --local_players 2-4 the SDK's own rule (SlotAssignment: device ordinal N drives player N+1,
// keyboard / mouse and stand-ins drive player 1) - the same rule, plus the device -> slot list in the log on every change,
// so a report shows which pad went to which player (a Steam Input or DS4Windows duplicate shows as two devices).
class SplitScreenPads final : public rex::input::DeviceAssignment {
 public:
  explicit SplitScreenPads(uint32_t players) : players_(players) {}
  void OnDevicesChanged(const std::vector<rex::input::DeviceInfo>& devices) override {
    for (auto& u : users_) u.clear();
    int real = 0;
    std::string list;
    for (const auto& d : devices) {
      int slot = -1;   // 0-based player; -1 = no player (ordinal past 4)
      if (d.synthetic) slot = 0;
      else if (d.ordinal < rex::input::kMaxGuestUsers) slot = static_cast<int>(d.ordinal);
      if (slot >= 0) users_[size_t(slot)].push_back(d.id);
      if (!d.synthetic) ++real;
      list += (list.empty() ? "" : " | ") + std::string(slot < 0 ? "no slot" : "slot " + std::to_string(slot + 1)) +
              (slot >= int(players_) ? " (no player)" : "") + ": " + std::string(d.synthetic ? "[kbm/stand-in] " : "") +
              "#" + std::to_string(d.ordinal) + " \"" + d.name + "\" " + d.guid;
    }
    g_real_pads.store(real);
    if (list != last_) {
      last_ = list;
      REXLOG_INFO("cogbox_pads: {} device(s), {} local players, device -> slot: {}", devices.size(), players_,
                  list.empty() ? "none" : list);
    }
  }
  void DevicesForUser(uint32_t user_index, std::vector<rex::input::DeviceId>& out) const override {
    out.clear();
    if (user_index < rex::input::kMaxGuestUsers) out = users_[user_index];
  }

 private:
  uint32_t players_;
  std::vector<rex::input::DeviceId> users_[rex::input::kMaxGuestUsers];
  std::string last_ = "\x01";
};

// Returns false while the runtime has no input system yet (it is made after the dialogs: job 1101's Gears 1 log), so the
// pads thread below retries; true once decided.
bool InstallPadAssignment(rex::Runtime* runtime) {
  auto* input = runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  if (!input) {   // the dialogs can come before the runtime has its input system (job 1103's logs): ask the runtime itself
    if (auto* rt = rex::Runtime::instance()) input = static_cast<rex::input::InputSystem*>(rt->input_system());
  }
  if (!input) return false;
  const uint32_t players = rex::cvar::Query<uint32_t>("local_players");
  const bool script2 = !rex::cvar::Query<std::string>("gow2_input_script2").empty();
  if (!REXCVAR_GET(cogbox_pad_merge) || players > 1 || script2) {
    REXLOG_INFO("cogbox_pads: one controller per player (local_players {}, merge {}, second script pad {})", players,
                REXCVAR_GET(cogbox_pad_merge), script2);
    if (players > 1) input->SetDeviceAssignment(std::make_unique<SplitScreenPads>(players));   // 0.4.9: logs the slots
    return true;
  }
  input->SetDeviceAssignment(std::make_unique<SinglePlayerPads>());
  REXLOG_INFO("cogbox_pads: one local player - every controller drives player 1");
  return true;
}

// GitHub #108 (Steam Deck): the SDK's SDL driver pumps SDL only from calls on a controller it already knows, and under
// Wine / Proton SDL finds controllers only while it pumps - so the game never saw one (SDK patch 59 fixes it in the
// runtime). Until that runtime ships: if the runtime exports SDL_PumpEvents, pump on the UI thread every 0.5 s while
// no controller is known. Nothing happens when the export is missing or a controller is already there.
void StartPadPump(rex::ui::Window* window, rex::Runtime* runtime) {
  using PumpFn = void(__cdecl*)(void);
  PumpFn pump = nullptr;
  for (const wchar_t* dll : {L"rexruntime.dll", L"SDL3.dll"}) {
    if (HMODULE m = GetModuleHandleW(dll)) {
      pump = reinterpret_cast<PumpFn>(GetProcAddress(m, "SDL_PumpEvents"));
      if (pump) break;
    }
  }
  REXLOG_INFO("cogbox_pads: SDL_PumpEvents {}", pump ? "found - pumped while no controller is known" : "not exported - left to the SDK");
  if (g_pump.joinable()) return;
  g_pump_stop.store(false);
  g_pump = std::thread([pump, window, runtime] {
    bool logged = false, assigned = InstallPadAssignment(runtime);
    for (int tries = 0; !g_pump_stop.load(); ++tries) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      if (!assigned) {
        assigned = InstallPadAssignment(runtime);
        if (!assigned && tries == 120) REXLOG_WARN("cogbox_pads: no input system after 60 s");
      }
      if (!pump || !window || g_pump_stop.load() || g_real_pads.load() != 0) continue;
      if (!logged) { logged = true; REXLOG_INFO("cogbox_pads: no controller known yet - pumping SDL for new ones"); }
      window->app_context().CallInUIThread([pump] { pump(); });
    }
  });
}

void StopPadPump() {
  g_pump_stop.store(true);
  if (g_pump.joinable()) g_pump.join();
}

// ---- the settings COGBox gave us ----------------------------------------------------------------------------------
struct MenuOption {
  std::string id, tab, label, kind, why, tip;
  std::vector<std::string> choices;
  double value = 0, start = 0, min = 0, max = 1, step = 1;
  double applied = 0;   // achievements part 4: the value last set live (options LiveCvarFor knows)
  int decimals = 0;
  bool enabled = true;
  // 0.4.9 live settings: COGBox's live= lines - the values that apply at once and their cvars (this game's names)
  std::vector<xenos::ach::LiveValue> live;
  bool live_failed = false;   // a cvar of it couldn't be set (an older exe): it needs the restart after all
};

// 0.4.9: the cvars `o` gets for `value` when that value applies at once; nullopt = it needs the restart. COGBox's table
// first; without one (an older COGBox) the popup settings as before (LiveCvarFor).
std::optional<std::vector<std::pair<std::string, std::string>>> LiveFor(const MenuOption& o, double value) {
  if (o.live_failed) return std::nullopt;
  if (!o.live.empty()) {
    if (const xenos::ach::LiveValue* lv = xenos::ach::FindLive(o.live, value)) return lv->cvars;
    return std::nullopt;
  }
  if (auto c = xenos::ach::LiveCvarFor(o.id, value)) return std::vector<std::pair<std::string, std::string>>{*c};
  return std::nullopt;
}
// A change of `o` that is waiting for the restart (its row shows "Restart required").
bool NeedsRestart(const MenuOption& o) { return o.value != o.start && !LiveFor(o, o.value); }

struct MenuFile {
  std::string game, changes_path, restart_path;
  std::vector<MenuOption> options;
  std::vector<std::string> tabs;
};

std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && s[i] == ' ') ++i;
  return s.substr(i);
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(std::max(0, n)), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

MenuFile LoadMenuFile(const std::string& path) {
  MenuFile m;
  if (path.empty()) return m;
  std::ifstream in(Widen(path), std::ios::binary);
  if (!in) {
    REXLOG_WARN("cogbox_menu: can't read {}", path);
    return m;
  }
  std::string line;
  int cur = -1;  // index into m.options (-1: the [menu] part)
  while (std::getline(in, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == ';') continue;
    if (line == "[menu]") { cur = -1; continue; }
    if (line == "[option]") { m.options.emplace_back(); cur = int(m.options.size()) - 1; continue; }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    if (cur < 0) {
      if (k == "game") m.game = v;
      else if (k == "changes") m.changes_path = v;
      else if (k == "restart") m.restart_path = v;
      continue;
    }
    MenuOption& o = m.options[size_t(cur)];
    if (k == "id") o.id = v;
    else if (k == "tab") o.tab = v;
    else if (k == "label") o.label = v;
    else if (k == "kind") o.kind = v;
    else if (k == "why") o.why = v;
    else if (k == "tip") o.tip = v;
    else if (k == "value") o.value = o.start = o.applied = std::atof(v.c_str());
    else if (k == "min") o.min = std::atof(v.c_str());
    else if (k == "max") o.max = std::atof(v.c_str());
    else if (k == "step") o.step = std::atof(v.c_str());
    else if (k == "decimals") o.decimals = std::atoi(v.c_str());
    else if (k == "enabled") o.enabled = v != "0";
    else if (k == "live") {   // 0.4.9
      if (auto lv = xenos::ach::ParseLiveLine(v)) o.live.push_back(std::move(*lv));
    }
    else if (k == "choices") {
      size_t a = 0;
      for (;;) {
        const size_t b = v.find('|', a);
        o.choices.push_back(v.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
      }
    }
  }
  m.options.erase(std::remove_if(m.options.begin(), m.options.end(), [](const MenuOption& o) { return o.id.empty(); }),
                  m.options.end());
  for (const MenuOption& o : m.options)
    if (std::find(m.tabs.begin(), m.tabs.end(), o.tab) == m.tabs.end()) m.tabs.push_back(o.tab);
  // 0.4.9: a fixed tab order, whatever order the file lists them in (unknown tabs after these, in file order)
  {
    static const char* const kOrder[] = {"Display", "Graphics", "Controls", "Gameplay", "Achievements"};
    auto rank = [](const std::string& t) {
      for (size_t i = 0; i < std::size(kOrder); ++i) if (t == kOrder[i]) return int(i);
      return int(std::size(kOrder));
    };
    std::stable_sort(m.tabs.begin(), m.tabs.end(), [&](const std::string& a, const std::string& b) { return rank(a) < rank(b); });
  }
  REXLOG_INFO("cogbox_menu: {} setting(s) in {} tab(s) from {}", m.options.size(), m.tabs.size(), path);
  return m;
}

bool WriteText(const std::string& path, const std::string& text) {
  if (path.empty()) return false;
  std::ofstream out(Widen(path), std::ios::binary | std::ios::trunc);
  out << text;
  return bool(out);
}

// ---- the controller: XInput on a polling thread -----------------------------------------------------------------
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

std::mutex g_pad_mutex;
XINPUT_GAMEPAD g_pad = {};  // the first connected controller's latest state
bool g_pad_connected = false;

std::atomic<bool> g_open{false};          // the menu is showing (the game gets no input)
std::atomic<bool> g_open_wanted{false};   // a combo / F1 asked for it (the UI thread makes it)
std::atomic<bool> g_stop{false};
std::thread g_poller;
rex::ui::ImGuiDrawer* g_drawer = nullptr;
rex::ui::Window* g_window = nullptr;
MenuFile g_file;
// Achievements part 3: the Achievements page (made on first use, UI thread; it keeps the chosen game, sort and Show
// while the game runs).
rex::ui::ImmediateDrawer* g_immediate = nullptr;
rex::Runtime* g_runtime = nullptr;
std::unique_ptr<xenos::AchievementsBrowser> g_browser;
xenos::AchievementsBrowser* Browser() {
  if (!g_browser) g_browser = std::make_unique<xenos::AchievementsBrowser>(g_immediate, g_runtime);
  return g_browser.get();
}
enum class MenuPage { kSettings, kAchievements, kSocial };   // social step 5: kSocial
// Friends' profile pictures for the Social page (UI thread): loaded once each, kept while the game runs.
std::map<std::string, std::unique_ptr<rex::ui::ImmediateTexture>> g_profile_icons;
void ToggleSocialOnUIThread();   // social step 5: F8 / Guide

void RequestOpenOnUIThread();
void ScheduleDrop();
void BlockGameInput(bool on);   // 0.2.0, below

// Based on PR #134 by Flippage (https://github.com/Flippage): mouse control in the menu and no input bleeding into the
// game. COGBox's take: we never touch the window's mouse capture ourselves - the keyboard/mouse driver captures only
// while mouse look is active, so switching mouse look off makes the driver release its own capture request, and putting
// back exactly what was there at open makes it take it again (no capture-count drift). The OS cursor's visibility is
// saved the first time we hide it (Dear ImGui draws its own cursor) and restored on close.
bool g_look_saved = false, g_look_before = false;   // mouse look as it was when the menu opened
bool g_cursor_hidden_by_us = false;
rex::ui::Window::CursorVisibility g_cursor_before = rex::ui::Window::CursorVisibility::kVisible;
std::atomic<bool> g_unblock_pending{false};   // closed, waiting for the closing buttons to be released

// PR #134: the buttons that close the menu (or click Resume) must be up before the game gets input back, or the same
// click fires the weapon / rolls. Read with GetAsyncKeyState only while our window is in front.
struct ClosingInputs {
  bool keys = false;          // mouse buttons, Esc, Enter
  uint8_t lt = 0, rt = 0;     // triggers
  uint16_t pad = 0;           // buttons
};
ClosingInputs SampleClosingInputs() {
  ClosingInputs s;
  HWND hwnd = g_window ? static_cast<HWND>(g_window->GetNativeWindowHandle()) : nullptr;
  if (!hwnd || GetForegroundWindow() == hwnd) {
    for (int vk : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_ESCAPE, VK_RETURN}) s.keys |= (GetAsyncKeyState(vk) & 0x8000) != 0;
  }
  std::lock_guard<std::mutex> l(g_pad_mutex);
  s.lt = g_pad.bLeftTrigger; s.rt = g_pad.bRightTrigger; s.pad = g_pad.wButtons;
  return s;
}
bool ClosingInputsHeld(const ClosingInputs& s) {
  return s.keys || s.lt > 30 || s.rt > 30 || (s.pad & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START)) != 0;
}
constexpr int kDismissTimeoutMs = 250;   // PR #134: give input back after this even if something stays held

// Job 523: the controller through the game's own input system (SDL: Xbox, PlayStation, Switch and Steam Deck pads), as
// the game itself reads it; plain XInput only when there is none.
std::atomic<rex::input::InputSystem*> g_input{nullptr};   // PR #134: atomic, looked up late (made after the dialogs)
rex::input::InputSystem* InputSys() {
  rex::input::InputSystem* in = g_input.load();
  if (!in) {
    if (auto* rt = rex::Runtime::instance()) {
      in = static_cast<rex::input::InputSystem*>(rt->input_system());
      if (in) g_input.store(in);
    }
  }
  return in;
}

void PollPads() {
  HMODULE xi = LoadLibraryW(L"xinput1_4.dll");
  if (!xi) xi = LoadLibraryW(L"xinput9_1_0.dll");
  auto get = xi ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(xi, "XInputGetState")) : nullptr;
  if (!get && !InputSys() && !rex::Runtime::instance()) {
    REXLOG_WARN("cogbox_menu: no controller input found - the menu opens with F1 only");
    return;
  }
  bool combo_was = false;
  xenos::GuideEdge guide;   // social step 5
  while (!g_stop.load()) {
    XINPUT_GAMEPAD pad = {};
    bool connected = false;
    rex::input::InputSystem* input_sys = InputSys();
    if (input_sys) {
      for (uint32_t i = 0; i < 4 && !connected; ++i) {
        rex::input::X_INPUT_STATE st = {};
        if (input_sys->GetStateForUI(i, &st) == rex::X_RESULT(0)) {   // X_ERROR_SUCCESS
          pad.wButtons = static_cast<uint16_t>(st.gamepad.buttons);   // the same bit layout as XInput
          pad.bLeftTrigger = st.gamepad.left_trigger;
          pad.bRightTrigger = st.gamepad.right_trigger;
          pad.sThumbLX = static_cast<int16_t>(st.gamepad.thumb_lx);
          pad.sThumbLY = static_cast<int16_t>(st.gamepad.thumb_ly);
          pad.sThumbRX = static_cast<int16_t>(st.gamepad.thumb_rx);
          pad.sThumbRY = static_cast<int16_t>(st.gamepad.thumb_ry);
          connected = true;
        }
      }
    } else {
      for (DWORD i = 0; i < 4 && !connected; ++i) {
        XINPUT_STATE s = {};
        if (get(i, &s) == ERROR_SUCCESS) { pad = s.Gamepad; connected = true; }
      }
    }
    {
      std::lock_guard<std::mutex> l(g_pad_mutex);
      g_pad = pad;
      g_pad_connected = connected;
    }
    const bool combo = connected && (pad.wButtons & XINPUT_GAMEPAD_BACK) && (pad.wButtons & XINPUT_GAMEPAD_START);
    if (combo && !combo_was && !g_open.load()) RequestOpenOnUIThread();
    combo_was = combo;
    // social step 5: Guide / PS opens (or closes) the Social page; the game itself never sees Guide (SDK input)
    if (guide.Pressed(connected && (pad.wButtons & 0x0400) != 0) && REXCVAR_GET(cogbox_social_guide) && !REXCVAR_GET(guide_button) &&
        XenosSocialAvailable())   // review: with guide_button on the game owns Guide
      ToggleSocialOnUIThread();
    XenosPumpSocialPopup();   // social step 5: new invites / requests / friends online -> pop-ups
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }
}

// ---- 0.4.9: the menu's fonts (Noto Sans) ----------------------------------------------------------------------------
xenos::menu::Fonts g_menu_fonts;
// Noto Sans when it was loaded; else the popups' fonts (Inter / the PC's own) so the menu still draws.
const xenos::menu::Fonts& MenuFonts() {
  static xenos::menu::Fonts fallback;
  if (g_menu_fonts.ok()) return g_menu_fonts;
  fallback.regular = fallback.regular_small = XenosAchievementFont(false);
  fallback.semibold = fallback.semibold_small = fallback.bold = XenosAchievementFont(true);
  return fallback;
}
// The tallest display mode of the PC's monitors (physical pixels, not DPI-scaled): the size the fonts are baked for.
float MonitorHeight() {
  DWORD best = 0;
  DISPLAY_DEVICEW dd{};
  dd.cb = sizeof dd;
  for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i, dd.cb = sizeof dd) {
    if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) best = std::max(best, dm.dmPelsHeight);
  }
  return best ? float(best) : 1080.0f;
}

// ---- the menu itself --------------------------------------------------------------------------------------------
class InGameMenu final : public rex::ui::ImGuiDialog {
 public:
  InGameMenu(rex::ui::ImGuiDrawer* drawer, MenuPage page) : ImGuiDialog(drawer) {
    ShowPage(page);
    ImGuiIO& io = GetIO();
    saved_config_ = io.ConfigFlags;
    saved_backend_ = io.BackendFlags;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    std::lock_guard<std::mutex> l(g_pad_mutex);
    last_ = g_pad;  // buttons already down (the Back + Start that opened us) count only after a release
    held_at_open_ = g_pad.wButtons;
    // PR #134: Dear ImGui's own cursor (the OS cursor doesn't show over a D3D12 swap chain / borderless window); not when
    // the menu was opened with the controller - it comes back as soon as the mouse moves or clicks.
    saved_draw_cursor_ = io.MouseDrawCursor;
    show_cursor_ = !((held_at_open_ & XINPUT_GAMEPAD_BACK) && (held_at_open_ & XINPUT_GAMEPAD_START));
    io.MouseDrawCursor = show_cursor_;
    // menu3: the legend starts as the game's prompts show (the prompt monitor's style) unless the pad opened the menu
    // menu4: only at open - from then on the menu's own last device wins (the game's prompts are not read again)
    mode_.SetKeys(show_cursor_ && ::xenos::input::GlobalPromptDeviceMonitor().PublishedStyle() == ::xenos::renderer::prompt::DeviceStyle::kKeyboard);
    prev_mouse_ = io.MousePos;
  }
  ~InGameMenu() override {
    ImGuiIO& io = GetIO();
    io.ConfigFlags = saved_config_;
    io.BackendFlags = saved_backend_;
    io.MouseDrawCursor = saved_draw_cursor_;
  }
  bool done() const { return done_; }
  bool OnSocial() const { return page_ == MenuPage::kSocial; }   // social step 5
  void GoSocial() { ShowPage(MenuPage::kSocial); }
  void Close() { Resume(); }   // F7 while open

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (close_scheduled_) return;
    if (unblock_pending_) {   // PR #134: closed; wait (hidden) for the closing buttons to be released
      io.MouseDrawCursor = false;
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - dismissed_at_).count();
      if (!ClosingInputsHeld(SampleClosingInputs()) || ms >= kDismissTimeoutMs) {
        close_scheduled_ = true;
        ScheduleDrop();
      }
      return;
    }
    // the OS cursor stays hidden while the menu is open (saved once, put back on close)
    if (g_window && g_window->GetCursorVisibility() != rex::ui::Window::CursorVisibility::kHidden) {
      if (!g_cursor_hidden_by_us) { g_cursor_before = g_window->GetCursorVisibility(); g_cursor_hidden_by_us = true; }
      g_window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
    }
    if (io.MousePos.x != prev_mouse_.x || io.MousePos.y != prev_mouse_.y || io.MouseDown[0] || io.MouseDown[1] ||
        io.MouseDown[2] || io.MouseWheel != 0.0f) {
      show_cursor_ = true;
      mouse_used_ = true;   // menu3 / menu4: the mouse -> the keyboard legend (InputModeTracker in FeedPad)
      prev_mouse_ = io.MousePos;
    }
    FeedPad(io);
    io.MouseDrawCursor = show_cursor_;
    const float scale = std::max(1.0f, io.DisplaySize.y / 900.0f);   // the achievements browser's own scale
    ImGui::GetStyle().MouseCursorScale = scale;
    // 0.4.9 (Shagg): the whole screen, the launcher's look, Noto Sans - see xenos/menu_look.h. A short fade in.
    const float fade = std::min(1.0f, std::chrono::duration<float>(std::chrono::steady_clock::now() - opened_at_).count() / 0.15f);
    const bool pad_legend = !mode_.keys() && PadConnected();   // menu3: the last device used (FeedPad / mouse above)
    const xenos::menu::Look L = xenos::menu::MakeLook(io.DisplaySize, fade, MenuFonts(), REXCVAR_GET(cogbox_menu_ps), pad_legend);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    xenos::menu::PushStyle(L);
    if (first_frame_) ImGui::SetNextWindowFocus();
    if (ImGui::Begin("##cogbox_menu", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse)) {
      menu_root_ = ImGui::GetCurrentContext()->CurrentWindow->RootWindow;   // review 049: whose popups hold B back (OwnPopupOpen)
      ImGui::PushFont(L.fonts.regular, L.px(xenos::menu::kBody));   // widgets (combo boxes, sliders, lists) in Noto Sans
      if (page_ == MenuPage::kAchievements) DrawAchievements(L, scale);
      else if (page_ == MenuPage::kSocial) DrawSocial(L, scale);
      else DrawSettings(L);
      ImGui::PopFont();
    }
    ImGui::End();
    xenos::menu::PopStyle();
    first_frame_ = false;
    // Closing (PR #134): hidden at once; the game gets its input back once the closing buttons are up (or after
    // kDismissTimeoutMs), then the dialog is dropped by one deferred call on the UI thread (not from inside its Draw).
    if (done_ && !unblock_pending_) {
      unblock_pending_ = true;
      dismissed_at_ = std::chrono::steady_clock::now();
      g_unblock_pending.store(true);
      io.MouseDrawCursor = false;
    }
  }

 private:
  // The controller's buttons into Dear ImGui (B closes the menu when no list is open; Start closes it too).
  void FeedPad(ImGuiIO& io) {
    XINPUT_GAMEPAD p;
    {
      std::lock_guard<std::mutex> l(g_pad_mutex);
      p = g_pad;
    }
    held_at_open_ &= p.wButtons;  // a button released once counts from then on
    const WORD live = WORD(p.wButtons & ~held_at_open_);
    struct Map { WORD bit; ImGuiKey key; } map[] = {
        {XINPUT_GAMEPAD_A, ImGuiKey_GamepadFaceDown},       {XINPUT_GAMEPAD_B, ImGuiKey_GamepadFaceRight},
        {XINPUT_GAMEPAD_X, ImGuiKey_GamepadFaceLeft},       {XINPUT_GAMEPAD_Y, ImGuiKey_GamepadFaceUp},
        {XINPUT_GAMEPAD_DPAD_UP, ImGuiKey_GamepadDpadUp},   {XINPUT_GAMEPAD_DPAD_DOWN, ImGuiKey_GamepadDpadDown},
        {XINPUT_GAMEPAD_DPAD_LEFT, ImGuiKey_GamepadDpadLeft}, {XINPUT_GAMEPAD_DPAD_RIGHT, ImGuiKey_GamepadDpadRight},
        {XINPUT_GAMEPAD_LEFT_SHOULDER, ImGuiKey_GamepadL1}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, ImGuiKey_GamepadR1},
        {XINPUT_GAMEPAD_START, ImGuiKey_GamepadStart},      {XINPUT_GAMEPAD_BACK, ImGuiKey_GamepadBack}};
    // social step 5 test: keys bound to pad buttons (1-4 = D-pad) moved the focus while typing a tag - while a text box
    // takes typing, the pad gives Dear ImGui nothing
    const bool typing = io.WantTextInput;
    const float dz = 0.35f, lx = typing ? 0.0f : p.sThumbLX / 32767.0f, ly = typing ? 0.0f : p.sThumbLY / 32767.0f;
    // menu4 (RC4-1): the pad state is the SDK's merged player 1 - with keyboard-and-mouse on, the key binds (arrows /
    // WASD -> left stick, Enter / Esc -> Start, Space -> A ...) show up in it. Only a real controller EVENT takes the
    // legend (xenos/menu_input.h), and pad moves made by the keyboard's binds don't step the focus a second time.
    bool key_down = false, key_pressed = false;
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_GamepadStart; ++k) {
      if (k >= ImGuiKey_MouseLeft && k <= ImGuiKey_MouseWheelY) continue;
      key_down |= ImGui::IsKeyDown(ImGuiKey(k));
      key_pressed |= ImGui::IsKeyPressed(ImGuiKey(k), false);
    }
    {
      xenos::menu::PadSample ps;
      ps.buttons = live;
      ps.lx = lx; ps.ly = ly; ps.rx = p.sThumbRX / 32767.0f; ps.ry = p.sThumbRY / 32767.0f;
      ps.lt = p.bLeftTrigger / 255.0f; ps.rt = p.bRightTrigger / 255.0f;
      const double now_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
      if (mode_.Update(now_ms, ps, key_down, key_pressed, mouse_used_)) show_cursor_ = false;   // PR #134: pad in use - no cursor
      mouse_used_ = false;
    }
    const bool pad_from_keys = mode_.keyboard_recent();
    // Review 049 (on-PC: the settings rows never took pad focus): on the settings page the menu moves the focus
    // itself (xenos::menu::SettingsPage::focus) - the D-pad and left stick go to Dear ImGui only while a list of ours
    // is open (a combo's choices) or on the achievements page.
    const bool own_popup = OwnPopupOpen();
    const bool own_nav = page_ == MenuPage::kSettings && !own_popup && !g_file.tabs.empty() && !ImGui::IsAnyItemActive();
    constexpr WORD kDirs = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;
    // Keys the same way: Dear ImGui's keyboard navigation is off while we move the focus (arrows are read below);
    // Enter / Space press the focused item as A does.
    if (own_nav) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    else io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    const bool key_a = own_nav && (ImGui::IsKeyDown(ImGuiKey_Enter) || ImGui::IsKeyDown(ImGuiKey_KeypadEnter) || ImGui::IsKeyDown(ImGuiKey_Space));
    for (const Map& m : map)
      io.AddKeyEvent(m.key, ((live & m.bit) != 0 && !(typing && (m.bit & kDirs)) && !(own_nav && (m.bit & kDirs))) || (m.key == ImGuiKey_GamepadFaceDown && key_a));
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, !own_nav && lx < -dz, !own_nav && lx < -dz ? -lx : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, !own_nav && lx > dz, !own_nav && lx > dz ? lx : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, !own_nav && ly > dz, !own_nav && ly > dz ? ly : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, !own_nav && ly < -dz, !own_nav && ly < -dz ? -ly : 0.0f);
    nav_move_ = nav_adjust_ = 0;
    if (own_nav) {   // D-pad or stick: one step on the press, then repeats while held (0.35 s, then every 0.11 s)
      int dir = 0;   // 1 up, 2 down, 3 left, 4 right (none while the keyboard's binds drive the pad state)
      if (pad_from_keys) dir = 0;
      else if ((live & XINPUT_GAMEPAD_DPAD_UP) || ly > dz) dir = 1;
      else if ((live & XINPUT_GAMEPAD_DPAD_DOWN) || ly < -dz) dir = 2;
      else if ((live & XINPUT_GAMEPAD_DPAD_LEFT) || lx < -dz) dir = 3;
      else if ((live & XINPUT_GAMEPAD_DPAD_RIGHT) || lx > dz) dir = 4;
      const auto now = std::chrono::steady_clock::now();
      bool step = false;
      if (dir != nav_dir_) { step = dir != 0; nav_next_ = now + std::chrono::milliseconds(350); }
      else if (dir && now >= nav_next_) { step = true; nav_next_ = now + std::chrono::milliseconds(110); }
      nav_dir_ = dir;
      // keys repeat by themselves; menu3: WASD as the arrows
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) || ImGui::IsKeyPressed(ImGuiKey_W, true)) nav_move_ = -1;
      else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) || ImGui::IsKeyPressed(ImGuiKey_S, true)) nav_move_ = 1;
      else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) || ImGui::IsKeyPressed(ImGuiKey_A, true)) nav_adjust_ = -1;
      else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) || ImGui::IsKeyPressed(ImGuiKey_D, true)) nav_adjust_ = 1;
      if (step) {
        if (dir == 1) nav_move_ = -1;
        if (dir == 2) nav_move_ = 1;
        if (dir == 3) nav_adjust_ = -1;
        if (dir == 4) nav_adjust_ = 1;
      }
    } else {
      nav_dir_ = 0;
    }
    // our own presses: LB / RB tabs, B / Start / Esc close (edge-triggered)
    WORD pressed = WORD(live & ~last_live_);
    last_live_ = live;
    if (pad_from_keys) pressed = 0;   // menu4: those are the keyboard's binds (Enter / Esc -> Start ...); the keys act themselves
    const bool popup = own_popup;
    const bool editing = ImGui::IsAnyItemActive();   // menu2: a slider in edit mode (A entered it; A / B leave)
    if (page_ == MenuPage::kAchievements) {   // achievements part 3: LB / RB game, X sort, Y show
      if (!popup) {
        if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) Browser()->Previous();
        if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) Browser()->Next();
        if (pressed & XINPUT_GAMEPAD_X) Browser()->CycleSort();
        if (pressed & XINPUT_GAMEPAD_Y) Browser()->CycleShow();
      }
    } else if (!g_file.tabs.empty()) {
      if (!popup && ImGui::IsKeyPressed(ImGuiKey_Q, false)) pressed |= XINPUT_GAMEPAD_LEFT_SHOULDER;    // 0.4.9: keys for the tabs
      if (!popup && ImGui::IsKeyPressed(ImGuiKey_E, false)) pressed |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
      if (quit_confirm_) pressed &= WORD(~(XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER));
      if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) tab_ = (tab_ + int(g_file.tabs.size()) - 1) % int(g_file.tabs.size());
      if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) tab_ = (tab_ + 1) % int(g_file.tabs.size());
      if (pressed & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER)) focus_reset_ = true;   // first row of the tab
    }
    const bool back = (pressed & XINPUT_GAMEPAD_B) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    const bool in_box = quit_confirm_;
    if (in_box) {   // review 049: the "Quit?" box: B / Esc = No (back on Quit game); Start does nothing
      if (back) { quit_confirm_ = false; focus_ = 1 << 20; }   // past the end = Quit game
      else if (!own_nav) {
        // 0.5.0 (Shagg, rc6: "can't select Yes"): without a settings file (no tabs) our own navigation is off and
        // Dear ImGui's is on, and the box put its focus back on its selection every frame - arrows never moved it and
        // Enter / A did nothing. Read the box's keys here then: left / right (arrows, A / D, D-pad) pick, Enter /
        // Space / A (pad) confirm.
        // (0.5.0 review: not the A / D keys - a WASD player's A then Space would pick Yes and quit by accident)
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) || (pressed & XINPUT_GAMEPAD_DPAD_LEFT)) quit_sel_ = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) || (pressed & XINPUT_GAMEPAD_DPAD_RIGHT)) quit_sel_ = 1;
        const bool ok = (pressed & XINPUT_GAMEPAD_A) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false);
        if (ok && quit_open_frames_ > 1) {
          quit_confirm_ = false;
          if (quit_sel_ == 0) quit_now_ = true;
          else focus_ = 1 << 20;
        }
      }
      ++quit_open_frames_;
    } else {
      quit_open_frames_ = 0;
    }
    if (!in_box && !popup && !popup_last_ && !editing && !editing_last_) {   // menu2: B in a slider's edit mode only leaves it
      if (back && page_ == MenuPage::kSocial && xenos::social::ConsumeBack(ImGui::IsAnyItemActive())) {
        // social step 5 review: B / Esc closes the on-screen keyboard or ends typing, the page stays
      } else if (back && page_ != MenuPage::kSettings) ShowPage(MenuPage::kSettings);   // B: back to the settings
      else if (back || (pressed & XINPUT_GAMEPAD_START)) Resume();               // resume
    }
    popup_last_ = popup;
    editing_last_ = editing;
  }

  // Review 049 (verifier: B did nothing): IsPopupOpen("", AnyPopupId) at the root counts EVERY open popup in the context,
  // also another dialog's - an SDK XAM message box (xam_ui.cpp, BeginPopupModal) hidden under this full-screen menu -
  // and then B / Start never closed us. Only popups opened from this menu's window (a combo, a list) hold B back now.
  // A foreign modal still takes Dear ImGui's navigation (A / D-pad) while it is open: logged once, B still resumes.
  bool OwnPopupOpen() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    bool own = false;
    for (const ImGuiPopupData& pd : g.OpenPopupStack) {
      bool ours = false;   // opened from our window, a child of it, or a popup of ours (nested: ParentWindow chain)
      for (const ImGuiWindow* w = pd.RestoreNavWindow; w && !ours; w = w->ParentWindow) ours = menu_root_ && w->RootWindow == menu_root_;
      if (ours) own = true;
      else {
        if (!foreign_logged_) {
          foreign_logged_ = true;
          REXLOG_WARN("cogbox_menu: another dialog's popup is open ('{}') - it takes the controller's A / D-pad until "
                      "closed; B / Start still close the menu", pd.Window ? pd.Window->Name : "not drawn yet");
        }
      }
    }
    return own;
  }

  void ShowPage(MenuPage page) {
    if (page_ == MenuPage::kSocial && page != MenuPage::kSocial) xenos::social::OnLeave();   // social step 5
    page_ = page;
    page_first_frame_ = true;
    if (page == MenuPage::kAchievements) Browser()->Reload();
  }

  // Achievements part 3: the browser in the full-screen frame, then Back to settings / Resume.
  void DrawAchievements(const xenos::menu::Look& L, float scale) {
    const xenos::menu::Rect r = xenos::menu::BeginAchievements(L, GameName().c_str(), XenosTr("Achievements"), "");
    ImGui::SetCursorScreenPos(r.a);
    if (ImGui::BeginChild("##ach", ImVec2(r.b.x - r.a.x, r.b.y - r.a.y), ImGuiChildFlags_NavFlattened)) {
      ImGui::PushFont(XenosAchievementFont(false), 0.0f);   // the browser keeps its own font and sizes
      const float tip_h = ImGui::GetTextLineHeightWithSpacing() * 2.2f;
      const std::string tip = Browser()->Draw(scale, std::max(ImGui::GetContentRegionAvail().y - tip_h, 100.0f * scale));
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextDisabled("%s", tip.c_str());
      ImGui::PopTextWrapPos();
      ImGui::PopFont();
    }
    ImGui::EndChild();
    std::vector<xenos::menu::LegendItem> legend;
    if (L.pad) legend = {{false, "LB", ""}, {false, "RB", XenosTr("game")}, {false, "X", XenosTr("sort")}, {false, "Y", XenosTr("show")},
                         {false, "B", XenosTr("back to settings")}};
    else legend = {{true, "Esc", XenosTr("back to settings")}, {true, "F7", XenosTr("back to the game")}};
    switch (xenos::menu::EndAchievements(L, XenosTr("Back to settings"), XenosTr("Resume"), legend)) {
      case xenos::menu::Action::kBack: ShowPage(MenuPage::kSettings); break;
      case xenos::menu::Action::kResume: Resume(); break;
      default: break;
    }
    page_first_frame_ = false;
  }
  // social step 5: the Social page in the menu's own frame
  void DrawSocial(const xenos::menu::Look& L, float scale) {
    const xenos::menu::Rect r = xenos::menu::BeginAchievements(L, GameName().c_str(), XenosTr("Social"), "");
    ImGui::SetCursorScreenPos(r.a);
    auto result = xenos::social::PageResult::None;
    if (ImGui::BeginChild("##social_page", ImVec2(r.b.x - r.a.x, r.b.y - r.a.y), ImGuiChildFlags_NavFlattened)) {
      result = xenos::social::DrawPage(scale, L.pad, REXCVAR_GET(cogbox_menu_ps));
    }
    ImGui::EndChild();
    std::vector<xenos::menu::LegendItem> legend;
    if (L.pad) legend = {{false, "A", XenosTr("select")}, {false, "B", XenosTr("back to settings")}};
    else legend = {{true, "Esc", XenosTr("back to settings")}};
    const xenos::menu::Action a = xenos::menu::EndAchievements(L, XenosTr("Back to settings"), XenosTr("Resume"), legend);
    if (a == xenos::menu::Action::kBack || result == xenos::social::PageResult::Back) ShowPage(MenuPage::kSettings);
    if (a == xenos::menu::Action::kResume || result == xenos::social::PageResult::CloseMenu) Resume();
    page_first_frame_ = false;
  }
  static std::string GameName() { return g_file.game.empty() ? std::string(XenosTr("GAME MENU")) : g_file.game; }
  static bool PadConnected() {
    std::lock_guard<std::mutex> l(g_pad_mutex);
    return g_pad_connected;
  }

  // Changes that need the restart (the popup settings apply at once, see ApplyLive).
  int Changes() const {
    int n = 0;
    for (const MenuOption& o : g_file.options) n += NeedsRestart(o);
    return n;
  }
  // 0.4.9: changes already applied (live), for the bar's text.
  static int LiveChanges() {
    int n = 0;
    for (const MenuOption& o : g_file.options) n += o.value != o.start && !NeedsRestart(o);
    return n;
  }

  // The changed options, for COGBox to save once the game has closed (live_only: just the popup settings - see
  // xenos::ach::MenuChangesText).
  static std::string ChangesText(bool live_only) {
    std::vector<xenos::ach::MenuChange> opts;
    for (const MenuOption& o : g_file.options) opts.push_back({o.id, o.value, o.start, !NeedsRestart(o)});
    return xenos::ach::MenuChangesText(opts, live_only);
  }

  // Achievements part 4: the popup settings take effect at once (the popups read their cvars on every unlock), and are
  // written to the changes file straight away so COGBox keeps them even if the game is closed another way.
  // 0.4.9: every option COGBox marked live (frame cap, brightness, mouse, HDR levels, FPS bar...) the same way. A value
  // the table has no line for keeps waiting for the restart (applied stays where it was). Values are absolute (the
  // same cvar values the command line gets after the restart), so a live change and the restart never stack.
  static void ApplyLive() {
    bool changed = false, gamma = false, overlays = false, prompts = false;
    int fullscreen = -1, win_w = 0, win_h = 0;   // 0.4.9 batch 2: window changes, done after the loop on the UI thread
    for (MenuOption& o : g_file.options) {
      if (o.value == o.applied) continue;
      const auto cv = LiveFor(o, o.value);
      if (!cv) continue;
      bool all = true;
      // 0.4.9 batch 2: frame generation can only pause / resume what was made at start - asked before any cvar is set
      for (const auto& [name, val] : *cv)
        if (name == "gow2_frame_generation" && !XenosFrameGenerationLive(val)) {
          REXLOG_WARN("cogbox_menu: frame generation can't switch to {} here - {} needs the restart", val, o.id);
          all = false;
        }
      if (!all) {
        o.live_failed = true;
        o.applied = o.value;
        changed = true;
        continue;
      }
      for (const auto& [name, val] : *cv) {
        bool set;
        if (name.size() > 8 && name.compare(name.size() - 8, 8, "_max_fps") == 0) {
          // review 049: published as an atomic, never written to the cvar the game thread reads every tick
          char* end = nullptr;
          const long v = std::strtol(val.c_str(), &end, 10);
          set = end && end != val.c_str() && *end == 0;
          if (set) XenosSetLiveMaxFps(int32_t(v));
        } else {
          std::lock_guard<std::mutex> lock(XenosAchievementSettingsMutex());  // F-8: an unlock may read them right now
          set = rex::cvar::SetFlagByName(name, val);
        }
        if (!set) {
          REXLOG_WARN("cogbox_menu: couldn't set {} - {} needs the restart", name, o.id);
          all = false;
          continue;
        }
        REXLOG_INFO("cogbox_menu: live {} -> {}={}", o.id, name, val);
        if (name == "gow2_present_gamma") gamma = true;
        if (name == "gow2_fps_bar" || name == "gow2_hide_onscreen_notes") overlays = true;
        // 0.4.9 batch 2
        if (name == "gow2_present_aa") XenosPresentAaChanged(val == "smaa");
        if (name == "gow_button_prompts") prompts = true;
        if (name == "fullscreen") fullscreen = val == "true" ? 1 : 0;
        if (name == "window_width") win_w = std::atoi(val.c_str());
        if (name == "window_height") win_h = std::atoi(val.c_str());
      }
      if (!all) o.live_failed = true;   // counted as a restart change from now on
      o.applied = o.value;
      changed = true;
    }
    if (gamma) XenosPresentGammaChanged();   // the display ramp is built again with the new gamma
    if (overlays) XenosRefreshOverlays();    // the FPS bar / shader note made again (UI thread, deferred)
    if (prompts) ::xenos::input::GlobalPromptDeviceMonitor().ReloadMode();   // 0.4.9 batch 2
    // 0.4.9 batch 2: windowed <-> borderless and the window size, as Alt+Enter / a drag would (deferred: the menu is
    // drawing now, and the swap chain is resized by the window's own resize path)
    if (rex::ui::Window* w = g_window; w && (fullscreen >= 0 || (win_w > 0 && win_h > 0))) {
      w->app_context().CallInUIThreadDeferred([fullscreen, win_w, win_h] {
        rex::ui::Window* uw = g_window;
        if (!uw) return;
        if (fullscreen >= 0) uw->SetFullscreen(fullscreen == 1);
        if (win_w > 0 && win_h > 0 && !uw->IsFullscreen())
          uw->SetDesiredLogicalSize(static_cast<uint32_t>(win_w), static_cast<uint32_t>(win_h));
        REXLOG_INFO("cogbox_menu: window {} {}x{}", uw->IsFullscreen() ? "full screen (borderless)" : "windowed",
                    uw->GetDesiredLogicalWidth(), uw->GetDesiredLogicalHeight());
      });
    }
    if (changed) SaveLive();
  }
  // The live changes, so COGBox keeps them whatever way the game closes (on each change and on Resume).
  static void SaveLive() {
    if (!g_file.changes_path.empty() && !WriteText(g_file.changes_path, ChangesText(true)))
      REXLOG_WARN("cogbox_menu: couldn't write {}", g_file.changes_path);
  }
  void Resume() {
    REXLOG_INFO("cogbox_menu: resume");
    if (LiveChanges()) SaveLive();
    done_ = true;
  }

  void Finish(bool restart) {
    const std::string text = ChangesText(false);
    if (!text.empty() && !WriteText(g_file.changes_path, text)) REXLOG_WARN("cogbox_menu: couldn't write {}", g_file.changes_path);
    if (restart && !WriteText(g_file.restart_path, "restart\r\n")) REXLOG_WARN("cogbox_menu: couldn't write {}", g_file.restart_path);
    REXLOG_INFO("cogbox_menu: {} with {} change(s)", restart ? "restart" : "quit", Changes());
    done_ = true;
    if (rex::ui::Window* w = g_window) {
      // 0.2.0: while the game is still the window in front, let COGBox come to the front when it closes - Windows only
      // lets the program in front hand that permission on (COGBox's own attempt only flashed its taskbar button).
      AllowSetForegroundWindow(ASFW_ANY);
      w->app_context().CallInUIThreadDeferred([w] { w->RequestClose(); });
    }
  }

  // 0.4.9: the settings page (xenos::menu::DrawSettings draws it; the values are edited in g_file in place).
  void DrawSettings(const xenos::menu::Look& L) {
    namespace m = xenos::menu;
    m::SettingsPage pg;
    const std::string game = GameName();
    pg.game = game.c_str();
    pg.has_file = !g_file.tabs.empty();
    if (pg.has_file) {
      tab_ = std::clamp(tab_, 0, int(g_file.tabs.size()) - 1);
      for (const std::string& t : g_file.tabs) pg.tabs.push_back(XenosTr(t));
      pg.tab = &tab_;
      for (MenuOption& o : g_file.options) {
        if (o.tab != g_file.tabs[size_t(tab_)]) continue;
        m::Row r;
        r.id = o.id; r.label = o.label; r.kind = o.kind; r.tip = o.tip; r.why = o.why;
        r.choices = &o.choices; r.value = &o.value; r.min = o.min; r.max = o.max; r.step = o.step; r.decimals = o.decimals;
        r.enabled = o.enabled;
        r.restart = NeedsRestart(o);
        r.live = LiveFor(o, o.value).has_value();
        pg.rows.push_back(std::move(r));
      }
    }
    const int n = Changes(), live_n = LiveChanges();
    if (n) pg.status = std::to_string(n) + " " + XenosTr("change(s) need a restart - Restart to apply.") +
                       (live_n ? std::string("  ") + XenosTr("Other changes are already applied.") : std::string());
    else if (live_n) pg.status = XenosTr("Changes are applied straight away.");
    else if (pg.has_file) pg.status = XenosTr("Most settings apply straight away; the others show \"Restart required\".");
    pg.restart_changes = n;
    pg.can_restart = !g_file.restart_path.empty();
    pg.focus_resume = page_first_frame_;
    // review 049 / menu2: our own pad focus (first row of the tab at open / tab change); right: the buttons, left: back
    pg.focus = &focus_;
    pg.focus_reset = page_first_frame_ || focus_reset_ || tab_ != focus_tab_;
    focus_reset_ = false;
    focus_tab_ = tab_;
    pg.nav_move = nav_move_;
    pg.nav_adjust = nav_adjust_;
    pg.last_row = &last_row_;
    pg.confirm_quit = &quit_confirm_;
    pg.confirm_sel = &quit_sel_;
    pg.t_quit_confirm = XenosTr("Quit the game? Changes that wait for a restart are kept for the next start.");
    pg.t_yes = XenosTr("Yes");
    pg.t_no = XenosTr("No");
    page_first_frame_ = false;
    if (XenosSocialAvailable()) pg.t_social = XenosTr("Social");   // social step 5: when the game runs online
    pg.t_resume = XenosTr("Resume"); pg.t_achievements = XenosTr("Achievements"); pg.t_restart = XenosTr("Restart to apply");
    pg.t_quit = XenosTr("Quit game");
    restart_tag_ = XenosTr("Restart required");
    for (char& ch : restart_tag_) if (ch >= 'a' && ch <= 'z') ch = char(ch - 32);
    pg.t_restart_required = restart_tag_.c_str();
    pg.t_on = XenosTr("On"); pg.t_off = XenosTr("Off");
    pg.t_applies_now = XenosTr("Applies at once");
    pg.t_needs_restart = XenosTr("Needs a restart to change");
    pg.t_waiting = XenosTr("Restart required - waits for Restart to apply");
    pg.t_no_file = XenosTr("Start the game from COGBox to change its settings here.");
    // menu2: the legend follows the mode - a list open (pick / confirm / cancel), a slider in edit mode (adjust / done),
    // else the rows and buttons (D-pad up / down, D-pad right: the buttons, A select)
    if (L.pad && OwnPopupOpen())
      pg.legend = {{false, "D-pad", XenosTr("pick")}, {false, "A", XenosTr("confirm")}, {false, "B", XenosTr("cancel")}};
    else if (L.pad && ImGui::IsAnyItemActive())
      pg.legend = {{false, "D-pad", XenosTr("adjust")}, {false, "A", XenosTr("done")}, {false, "B", XenosTr("cancel")}};
    else if (L.pad)
      pg.legend = {{false, "LB", ""}, {false, "RB", XenosTr("tabs")}, {false, "D-pad", XenosTr("move, right: buttons")},
                   {false, "A", XenosTr("select")}, {false, "B", XenosTr("back to the game")}, {false, "Start", XenosTr("resume")}};
    else pg.legend = {{true, "Q", ""}, {true, "E", XenosTr("tabs")}, {true, "Enter", XenosTr("change")},
                      {true, "Esc", XenosTr("back to the game")}, {true, "F7", XenosTr("Achievements")}};
    const m::Action a = m::DrawSettings(L, pg);
    if (focus_ != logged_focus_) {   // review 049: where the pad / key focus is (job checks read this)
      logged_focus_ = focus_;
      const int nr = int(pg.rows.size());
      REXLOG_INFO("cogbox_menu: focus {} {}", focus_ < nr ? "row" : "button",
                  focus_ < nr ? pg.rows[size_t(focus_)].id : std::to_string(focus_ - nr));
    }
    // job 1465: compared with the last LOGGED state - B / Esc close the box in FeedPad, before this draw, so a
    // before/after-the-draw compare never saw that close (the box did close; only the line was missing)
    if (quit_confirm_ != logged_confirm_) {
      logged_confirm_ = quit_confirm_;
      REXLOG_INFO("cogbox_menu: quit confirmation {}", quit_confirm_ ? "asked" : "closed");
    }
    ApplyLive();   // achievements part 4 / 0.4.9 live settings
    switch (a) {
      case m::Action::kResume: Resume(); break;
      case m::Action::kAchievements: ShowPage(MenuPage::kAchievements); break;
      case m::Action::kSocial: ShowPage(MenuPage::kSocial); break;
      case m::Action::kRestart: Finish(true); break;
      case m::Action::kQuit: Finish(false); break;
      default: break;
    }
    if (quit_now_) { quit_now_ = false; Finish(false); }   // 0.5.0: Yes picked with the keys / pad (no settings file)
  }

  ImGuiConfigFlags saved_config_ = 0;
  ImGuiBackendFlags saved_backend_ = 0;
  XINPUT_GAMEPAD last_ = {};
  WORD held_at_open_ = 0, last_live_ = 0;
  int tab_ = 0;
  MenuPage page_ = MenuPage::kSettings;   // achievements part 3
  bool page_first_frame_ = true;
  bool first_frame_ = true, done_ = false, popup_last_ = false, close_scheduled_ = false;
  bool unblock_pending_ = false, saved_draw_cursor_ = false, show_cursor_ = true;   // PR #134
  ImGuiWindow* menu_root_ = nullptr;   // review 049: this menu's window (OwnPopupOpen)
  int focus_ = -1, focus_tab_ = -1, nav_move_ = 0, nav_adjust_ = 0, nav_dir_ = 0, quit_sel_ = 0;   // review 049: pad focus
  bool focus_reset_ = false, quit_confirm_ = false, quit_now_ = false;   // 0.5.0: quit_now_ = Yes from FeedPad
  int quit_open_frames_ = 0;
  int logged_focus_ = -1;
  bool logged_confirm_ = false;
  int last_row_ = 0;           // menu2: the row D-pad left goes back to from the buttons
  xenos::menu::InputModeTracker mode_;   // menu4: keys or controller legend (the last real device)
  bool mouse_used_ = false;
  bool editing_last_ = false;  // menu2: a slider was in edit mode last frame (its B is not "back to the game")
  std::chrono::steady_clock::time_point nav_next_{};
  bool foreign_logged_ = false;
  std::chrono::steady_clock::time_point dismissed_at_{};
  ImVec2 prev_mouse_{-FLT_MAX, -FLT_MAX};
  std::chrono::steady_clock::time_point opened_at_ = std::chrono::steady_clock::now();   // 0.4.9: the fade in
  std::string restart_tag_;
};

std::unique_ptr<InGameMenu> g_menu;

// The UI thread owns the dialog: made on request, dropped by one deferred call once it says it's done. (Not a deferred
// call that queues itself again: the SDK runs newly queued calls in the same pass, so that would never return.)
// 0.2.0 (Shagg 27 Oct 10:51): while the menu is open the game must get no controller / keyboard / mouse input at all.
// The SDK's UI input blocker does exactly that (GetState gives the game nothing), and when it is removed, buttons still
// held stay hidden from the game until released - so the press that closes the menu isn't also a press in the game.
// The menu itself reads through GetStateForUI, which the blocker doesn't stop. (The driver "active" callback used
// before could be replaced by the SDK's own, which let input through.)
std::atomic<bool> g_blocking{false};
void BlockGameInput(bool on) {
  rex::input::InputSystem* in = InputSys();   // PR #134: found late too, so the blocker is always added
  if (!in) return;
  if (g_blocking.exchange(on) == on) return;
  if (on) in->AddUIInputBlocker();
  else in->RemoveUIInputBlocker();
}

// PR #134: mouse look off while open (the driver then releases its capture: the mouse points at the menu, the camera
// stays still); exactly the state from before is put back on close. UI thread.
void SaveMouseState() {
  if (g_look_saved) return;
  g_look_before = rex::input::mnk::IsMouseLookActive();
  g_look_saved = true;
  rex::input::mnk::SetMouseLookActive(false);
}
void RestoreMouseState() {
  if (g_cursor_hidden_by_us && g_window) g_window->SetCursorVisibility(g_cursor_before);
  g_cursor_hidden_by_us = false;
  if (g_look_saved) rex::input::mnk::SetMouseLookActive(g_look_before);
  g_look_saved = false;
}

void OpenNow(MenuPage page = MenuPage::kSettings) {
  if (g_menu || !g_drawer) return;
  g_open.store(true);
  SaveMouseState();
  BlockGameInput(true);
  g_menu = std::make_unique<InGameMenu>(g_drawer, page);
  REXLOG_INFO("cogbox_menu: opened");
}

void ScheduleDrop() {
  if (!g_open.load()) return;
  g_unblock_pending.store(false);
  RestoreMouseState();
  g_open.store(false);
  BlockGameInput(false);   // 0.2.0
  if (g_window) g_window->app_context().CallInUIThreadDeferred([] { g_menu.reset(); });
  else g_menu.reset();
}

void RequestOpenOnUIThread() {
  if (g_window) g_window->app_context().CallInUIThreadDeferred([] { OpenNow(); });
}

// Social step 5: F8 / Guide - the menu on its Social page, or closed again when it is showing that page.
void ToggleSocial() {
  if (!XenosSocialAvailable()) return;
  if (g_menu) {
    if (g_menu->OnSocial()) g_menu->Close();
    else g_menu->GoSocial();
  } else if (!g_open.load()) {
    OpenNow(MenuPage::kSocial);
  }
}
// The game's own Invite / friends button (the Guide's friends screen on a 360): the menu on its Social page.
void OpenSocialOnUIThread() {
  if (!g_window) return;
  g_window->app_context().CallInUIThreadDeferred([] {
    if (!XenosSocialAvailable()) return;
    if (g_menu) g_menu->GoSocial();
    else if (!g_open.load()) OpenNow(MenuPage::kSocial);
  });
}
void ToggleSocialOnUIThread() {
  if (g_window) g_window->app_context().CallInUIThreadDeferred([] { ToggleSocial(); });
}

}  // namespace

// GitHub #143: the OS cursor stayed over the game when playing with a controller (keyboard and mouse off, or mouse look
// not active). A watcher hides it after 3 s without mouse movement while the game is in front, and shows it again on the
// first movement. Mouse look (the driver hides the cursor itself) and the open menu (it draws its own) are left alone.
// Visibility is changed on the UI thread only; what was there before is put back.
namespace {
std::atomic<rex::ui::Window*> g_idle_window{nullptr};
std::atomic<bool> g_idle_stop{false};
std::thread g_idle_thread;
std::atomic<bool> g_idle_hidden{false};   // set / cleared on the UI thread; the watcher reads it
rex::ui::Window::CursorVisibility g_idle_before = rex::ui::Window::CursorVisibility::kVisible;

void IdleCursorWatch() {
  POINT last{};
  GetCursorPos(&last);
  auto moved_at = std::chrono::steady_clock::now();
  bool hide_sent = false;
  while (!g_idle_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    rex::ui::Window* w = g_idle_window.load();
    if (!w) continue;
    POINT p{};
    GetCursorPos(&p);
    DWORD pid = 0;
    if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &pid);
    const bool front = pid == GetCurrentProcessId();
    const auto now = std::chrono::steady_clock::now();
    if (p.x != last.x || p.y != last.y || !front) {
      last = p;
      moved_at = now;
      if (hide_sent || g_idle_hidden.load()) {   // review 1188: while the hide is ours, every movement asks to restore
        hide_sent = false;
        w->app_context().CallInUIThreadDeferred([] {
          // Review 1186: with the menu open or mouse look on, the hide stays ours (the flag kept), so the next
          // movement after they end still puts the cursor back - clearing it here left the cursor hidden for good.
          rex::ui::Window* uw = g_idle_window.load();
          if (!uw || !g_idle_hidden.load() || g_open.load() || rex::input::mnk::IsMouseLookActive()) return;
          uw->SetCursorVisibility(g_idle_before);
          g_idle_hidden = false;
        });
      }
    } else if (!hide_sent && now - moved_at >= std::chrono::seconds(3) && !g_open.load()) {
      hide_sent = true;
      w->app_context().CallInUIThreadDeferred([] {
        rex::ui::Window* uw = g_idle_window.load();
        if (!uw || g_idle_hidden.load() || g_open.load() || rex::input::mnk::IsMouseLookActive()) return;
        if (uw->GetCursorVisibility() == rex::ui::Window::CursorVisibility::kHidden) return;
        g_idle_before = uw->GetCursorVisibility();
        uw->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
        g_idle_hidden = true;
      });
    }
  }
}
}  // namespace

void XenosSetPromptGame(uint32_t title_id) {
  ::xenos::renderer::prompt::GlobalPromptCompositor().SetGameTitle(title_id);
}

void XenosInstallInGameMenu(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, rex::ui::Window* window,
                            rex::Runtime* runtime) {
  StartPadPump(window, runtime);   // GitHub #108, menu or no menu: pad assignment (+ SDL pump when exported)
  // Shagg 3 Nov: \ is the screenshot key in every game, menu or no menu (frame_capture.cpp saves the next frame)
  rex::ui::RegisterBind("bind_cogbox_screenshot", "Backslash", "COGBox screenshot (Pictures\\COGBox)", [] { XenosRequestScreenshot(); });
  if (window && !g_idle_thread.joinable()) {   // GitHub #143, menu or no menu
    g_idle_window.store(window);
    g_idle_stop.store(false);
    g_idle_thread = std::thread(IdleCursorWatch);
  }
  // PR #117: the button-prompt monitor (its own thread; nothing with --gow_button_prompts=original), menu or no menu.
  ::xenos::input::GlobalPromptDeviceMonitor().Start(
      window, runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr);
  if (!REXCVAR_GET(cogbox_menu_enabled) || !drawer || !window) return;
  g_drawer = drawer;
  g_window = window;
  g_immediate = immediate;   // achievements part 3
  g_runtime = runtime;
  g_file = LoadMenuFile(REXCVAR_GET(cogbox_menu));
  // While the menu is open the game gets no controller / keyboard / mouse input.
  if (runtime) {
    if (auto* input = static_cast<rex::input::InputSystem*>(runtime->input_system())) {
      g_input.store(input);   // job 523; 0.2.0: blocking is BlockGameInput (the UI input blocker), not an active callback
    }
  }
  rex::ui::RegisterBind("bind_cogbox_menu", "F1", "COGBox in-game menu", [] {
    if (!g_open.load()) OpenNow();
  });
  // Achievements part 3: F7 opens the menu on the Achievements page (and closes it again), instead of the SDK's own
  // achievements window (its bind is registered before ours and would win the key).
  rex::ui::UnregisterBind("bind_achievements");
  rex::ui::RegisterBind("bind_cogbox_achievements", "F7", "COGBox achievements", [] {
    if (g_menu) g_menu->Close();
    else if (!g_open.load()) OpenNow(MenuPage::kAchievements);
  });
  XenosInstallSocialPopup(drawer, window);   // social step 5
  // Social step 5: F8 opens the menu on its Social page (and closes it again), when the game runs online.
  rex::kernel::xam::net::SetFriendsUiHandler([] { OpenSocialOnUIThread(); });   // the game's own Invite button
  rex::ui::RegisterBind("bind_cogbox_social", "F8", "COGBox social (friends, invites)", [] { ToggleSocial(); });
  g_stop.store(false);
  g_poller = std::thread(PollPads);
  if (REXCVAR_GET(cogbox_menu_open_at_start)) RequestOpenOnUIThread();
  REXLOG_INFO("cogbox_menu: ready (Back + Start or F1)");
}

void XenosShutdownInGameMenu() {
  StopPadPump();   // GitHub #108
  rex::ui::UnregisterBind("bind_cogbox_screenshot");
  g_idle_stop.store(true);   // GitHub #143
  if (g_idle_thread.joinable()) g_idle_thread.join();
  if (rex::ui::Window* w = g_idle_window.exchange(nullptr); w && g_idle_hidden.load()) w->SetCursorVisibility(g_idle_before);
  g_idle_hidden = false;
  ::xenos::input::GlobalPromptDeviceMonitor().Stop();
  ::xenos::input::GlobalPromptDeviceMonitor().DetachWindow(nullptr);
  AllowSetForegroundWindow(ASFW_ANY);   // 0.2.0: any other way out too (window closed, the game's own quit)
  g_stop.store(true);
  if (g_poller.joinable()) g_poller.join();
  XenosShutdownSocialPopup();   // social step 5 (after the pad thread that feeds it)
  g_menu.reset();
  if (g_open.load()) RestoreMouseState();   // PR #134
  g_unblock_pending.store(false);
  g_open.store(false);
  BlockGameInput(false);   // 0.2.0
  rex::ui::UnregisterBind("bind_cogbox_menu");
  rex::ui::UnregisterBind("bind_cogbox_achievements");
  rex::ui::UnregisterBind("bind_cogbox_social");   // social step 5
  rex::kernel::xam::net::SetFriendsUiHandler(nullptr);
  g_browser.reset();
  g_profile_icons.clear();   // made through g_immediate: freed before it goes
  g_immediate = nullptr;
  g_runtime = nullptr;
  g_drawer = nullptr;
  g_window = nullptr;
  g_input.store(nullptr);
}

bool XenosInGameMenuIsOpen() noexcept { return g_open.load(); }   // PR #134

// 0.4.9: called from XenosConfigureAchievementFonts (the app's OnConfigureFonts). Noto Sans from <COGBox>\assets\fonts,
// baked for the monitor's height, with only the characters the menu can show (this game's menu file, the in-game
// text's translation); Korean / Chinese / Japanese merged from the PC's fonts as the popups and the launcher do.
void XenosConfigureMenuFonts(ImFontAtlas* atlas) {
  if (!atlas || !REXCVAR_GET(cogbox_menu_enabled)) return;
  namespace fs = std::filesystem;
  const fs::path dir = XenosCogboxDir() / "assets" / "fonts";
  wchar_t win[MAX_PATH] = L"";
  GetWindowsDirectoryW(win, MAX_PATH);
  const fs::path sys = fs::path(win) / "Fonts";
  auto u8 = [](const fs::path& p) { return rex::path_to_utf8(p); };
  xenos::menu::FontFiles files;
  files.regular = u8(dir / "NotoSans-Regular.ttf");
  files.semibold = u8(dir / "NotoSans-SemiBold.ttf");
  files.bold = u8(dir / "NotoSans-Bold.ttf");
  files.hangul = u8(sys / "malgun.ttf");
  std::error_code ec;
  for (const char* f : XenosUiLangCode() == "ja" ? std::vector<const char*>{"YuGothM.ttc", "meiryo.ttc", "msgothic.ttc", "msyh.ttc"}
                                                 : std::vector<const char*>{"msyh.ttc", "msjh.ttc", "simsun.ttc"})
    if (fs::exists(sys / f, ec)) { files.han = u8(sys / f); break; }
  std::string text = XenosUiLangAllText();
  {
    std::ifstream in(Widen(REXCVAR_GET(cogbox_menu)), std::ios::binary);
    if (in) text.append(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  const float h = MonitorHeight();
  g_menu_fonts = xenos::menu::ConfigureFonts(atlas, files, text, h);
  if (g_menu_fonts.ok()) REXLOG_INFO("cogbox_menu: Noto Sans baked for a {}-pixel-high screen", int(h));
  else REXLOG_WARN("cogbox_menu: no Noto Sans in {} - the popups' font is used", u8(dir));
}

// Review 049: the live frame cap, handed from the menu (UI thread) to the game thread (dev_tools.h).
static std::atomic<int64_t> g_live_max_fps{INT64_MIN};
void XenosSetLiveMaxFps(int32_t fps) { g_live_max_fps.store(fps, std::memory_order_release); }
bool XenosLiveMaxFps(int32_t* fps) {
  const int64_t v = g_live_max_fps.load(std::memory_order_acquire);
  if (v == INT64_MIN) return false;
  *fps = int32_t(v);
  return true;
}

REXCVAR_DECLARE(std::string, cogbox_dir);  // achievements_export.cpp

ImTextureID xenos::social::ProfileIcon(const std::string& name) {
  if (!safe_icon_name(name) || !g_immediate) return ImTextureID(0);
  auto it = g_profile_icons.find(name);
  if (it == g_profile_icons.end()) {
    std::unique_ptr<rex::ui::ImmediateTexture> tex;
    const std::string dir = REXCVAR_GET(cogbox_dir);
    std::string bytes;
    if (!dir.empty() && xenos::ach::ReadFile(std::filesystem::path(dir) / name, &bytes)) {
      int w = 0, h = 0;
      std::vector<uint8_t> rgba = rex::ui::DecodeImageRGBA(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), w, h);
      if (!rgba.empty() && w > 0 && h > 0)
        tex = g_immediate->CreateTexture(uint32_t(w), uint32_t(h), rex::ui::ImmediateTextureFilter::kLinear, false, rgba.data());
    }
    it = g_profile_icons.emplace(name, std::move(tex)).first;
  }
  return it->second ? reinterpret_cast<ImTextureID>(it->second.get()) : ImTextureID(0);
}
