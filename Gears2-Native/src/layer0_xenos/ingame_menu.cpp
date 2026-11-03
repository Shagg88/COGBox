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
#include <xenos/input/prompt_device_monitor.h>
#include <rex/cvar.h>
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
#include <fstream>
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
  int decimals = 0;
  bool enabled = true;
};

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
    else if (k == "value") o.value = o.start = std::atof(v.c_str());
    else if (k == "min") o.min = std::atof(v.c_str());
    else if (k == "max") o.max = std::atof(v.c_str());
    else if (k == "step") o.step = std::atof(v.c_str());
    else if (k == "decimals") o.decimals = std::atoi(v.c_str());
    else if (k == "enabled") o.enabled = v != "0";
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
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }
}

// ---- the menu itself --------------------------------------------------------------------------------------------
class InGameMenu final : public rex::ui::ImGuiDialog {
 public:
  explicit InGameMenu(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {
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
    prev_mouse_ = io.MousePos;
  }
  ~InGameMenu() override {
    ImGuiIO& io = GetIO();
    io.ConfigFlags = saved_config_;
    io.BackendFlags = saved_backend_;
    io.MouseDrawCursor = saved_draw_cursor_;
  }
  bool done() const { return done_; }

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
      prev_mouse_ = io.MousePos;
    }
    FeedPad(io);
    io.MouseDrawCursor = show_cursor_;
    const float scale = std::max(1.0f, io.DisplaySize.y / 900.0f);
    ImGui::GetStyle().MouseCursorScale = scale;
    const float w = std::min(io.DisplaySize.x - 40.0f, 900.0f * scale), h = std::min(io.DisplaySize.y - 40.0f, 640.0f * scale);
    ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - w) * 0.5f, (io.DisplaySize.y - h) * 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f * scale, 16.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * scale, 5.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f * scale, 8.0f * scale));
    // 0.2.0 (Shagg 27 Oct 10:51): the COGBox / Gears look - gunmetal, crimson, sharp cut corners, rivets
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));   // the plate is drawn below
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.22f, 0.25f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.62f, 0.10f, 0.08f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.78f, 0.13f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_NavHighlight, ImVec4(0.96f, 0.26f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.09f, 0.10f, 0.11f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.16f, 0.17f, 0.19f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.20f, 0.21f, 0.23f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.42f, 0.44f, 0.47f, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.92f, 0.20f, 0.16f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.72f, 0.12f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.92f, 0.20f, 0.16f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.62f, 0.10f, 0.08f, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.62f, 0.10f, 0.08f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.78f, 0.13f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.72f, 0.12f, 0.10f, 0.75f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.07f, 0.075f, 0.085f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.90f, 0.88f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.55f, 0.56f, 0.57f, 1.0f));
    if (first_frame_) ImGui::SetNextWindowFocus();
    if (ImGui::Begin("##cogbox_menu", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoCollapse)) {
      DrawPlate(scale);   // 0.2.0
#if defined(IMGUI_VERSION_NUM) && IMGUI_VERSION_NUM >= 19200
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.35f * scale);
#else
      ImGui::SetWindowFontScale(1.35f * scale);
#endif
      DrawContents(scale);
#if defined(IMGUI_VERSION_NUM) && IMGUI_VERSION_NUM >= 19200
      ImGui::PopFont();
#endif
    }
    ImGui::End();
    ImGui::PopStyleColor(19);
    ImGui::PopStyleVar(7);
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
    for (const Map& m : map) io.AddKeyEvent(m.key, (live & m.bit) != 0);
    const float dz = 0.35f, lx = p.sThumbLX / 32767.0f, ly = p.sThumbLY / 32767.0f;
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, lx < -dz, lx < -dz ? -lx : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, lx > dz, lx > dz ? lx : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, ly > dz, ly > dz ? ly : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, ly < -dz, ly < -dz ? -ly : 0.0f);
    if (live != 0 || std::abs(lx) > dz || std::abs(ly) > dz) show_cursor_ = false;   // PR #134: pad in use - no cursor
    // our own presses: LB / RB tabs, B / Start / Esc close (edge-triggered)
    const WORD pressed = WORD(live & ~last_live_);
    last_live_ = live;
    const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    if (!g_file.tabs.empty()) {
      if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) tab_ = (tab_ + int(g_file.tabs.size()) - 1) % int(g_file.tabs.size());
      if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) tab_ = (tab_ + 1) % int(g_file.tabs.size());
    }
    if (!popup && !popup_last_ && ((pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
      done_ = true;  // resume
    popup_last_ = popup;
  }

  int Changes() const {
    int n = 0;
    for (const MenuOption& o : g_file.options) n += o.value != o.start;
    return n;
  }

  void Finish(bool restart) {
    std::string text;
    for (const MenuOption& o : g_file.options) {
      if (o.value == o.start) continue;
      char v[64];
      std::snprintf(v, sizeof v, "%.6g", o.value);
      text += o.id + "=" + v + "\r\n";
    }
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

  // 0.2.0: the window's plate - a dark gunmetal gradient with cut corners, a crimson edge along the top, rivets
  static void Chamfer(ImDrawList* dl, ImVec2 a, ImVec2 b, float c) {
    dl->PathLineTo(ImVec2(a.x + c, a.y)); dl->PathLineTo(ImVec2(b.x - c, a.y)); dl->PathLineTo(ImVec2(b.x, a.y + c));
    dl->PathLineTo(ImVec2(b.x, b.y - c)); dl->PathLineTo(ImVec2(b.x - c, b.y)); dl->PathLineTo(ImVec2(a.x + c, b.y));
    dl->PathLineTo(ImVec2(a.x, b.y - c)); dl->PathLineTo(ImVec2(a.x, a.y + c));
  }
  void DrawPlate(float scale) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetWindowPos(), b(a.x + ImGui::GetWindowWidth(), a.y + ImGui::GetWindowHeight());
    const float c = 18.0f * scale;
    dl->PushClipRectFullScreen();
    Chamfer(dl, ImVec2(a.x + 4 * scale, a.y + 6 * scale), ImVec2(b.x + 4 * scale, b.y + 6 * scale), c);
    dl->PathFillConvex(IM_COL32(0, 0, 0, 150));                       // shadow
    const int v0 = dl->VtxBuffer.Size;
    Chamfer(dl, a, b, c);
    dl->PathFillConvex(IM_COL32_WHITE);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, a, ImVec2(a.x, b.y), IM_COL32(40, 43, 48, 244),
                                                  IM_COL32(14, 15, 17, 244));
    Chamfer(dl, a, b, c);
    dl->PathStroke(IM_COL32(110, 114, 120, 170), ImDrawFlags_Closed, 1.5f * scale);
    dl->AddLine(ImVec2(a.x + c, a.y + 1.5f * scale), ImVec2(b.x - c, a.y + 1.5f * scale), IM_COL32(200, 34, 26, 255), 3.0f * scale);
    for (ImVec2 r : {ImVec2(a.x + c * 0.9f, a.y + c * 0.9f), ImVec2(b.x - c * 0.9f, a.y + c * 0.9f),
                     ImVec2(a.x + c * 0.9f, b.y - c * 0.9f), ImVec2(b.x - c * 0.9f, b.y - c * 0.9f)}) {
      dl->AddCircleFilled(r, 3.6f * scale, IM_COL32(70, 73, 78, 255));
      dl->AddCircleFilled(ImVec2(r.x - 1.0f * scale, r.y - 1.0f * scale), 1.6f * scale, IM_COL32(160, 164, 170, 255));
    }
    dl->PopClipRect();
  }

  void DrawContents(float scale) {
    // 0.2.0: a crimson bar, the title in capitals, the game's name dimmer
    {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float th = ImGui::GetTextLineHeight();
      dl->AddRectFilled(ImVec2(p.x, p.y + th * 0.12f), ImVec2(p.x + 5.0f * scale, p.y + th * 0.92f), IM_COL32(200, 34, 26, 255));
      ImGui::SetCursorScreenPos(ImVec2(p.x + 14.0f * scale, p.y));
    }
    ImGui::TextUnformatted("COGBOX");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", (g_file.game.empty() ? std::string("GAME MENU") : g_file.game).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled(REXCVAR_GET(cogbox_menu_ps) ? "   L1 / R1: tabs   Cross: change   Circle: back to the game"   // job 523
                                                    : "   LB / RB: tabs   A: change   B: back to the game");
    ImGui::Separator();
    const float bar_h = ImGui::GetFrameHeightWithSpacing() * 2.6f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (!g_file.tabs.empty()) {
      // the tabs: buttons, the chosen one lit
      tab_ = std::clamp(tab_, 0, int(g_file.tabs.size()) - 1);
      const float tw = (avail.x - (g_file.tabs.size() - 1) * ImGui::GetStyle().ItemSpacing.x) / float(g_file.tabs.size());
      for (int i = 0; i < int(g_file.tabs.size()); ++i) {
        if (i) ImGui::SameLine();
        const bool sel = i == tab_;
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.10f, 0.08f, 1.0f));
        if (ImGui::Button((g_file.tabs[size_t(i)] + "##tab").c_str(), ImVec2(tw, 0))) tab_ = i;
        if (sel) ImGui::PopStyleColor();
      }
      // the rows
      std::string tip;
      if (ImGui::BeginChild("##rows", ImVec2(0, ImGui::GetContentRegionAvail().y - bar_h - ImGui::GetTextLineHeightWithSpacing() * 2.2f),
                            ImGuiChildFlags_NavFlattened)) {
        const float label_w = ImGui::GetContentRegionAvail().x * 0.55f;
        const float ctrl_w = ImGui::GetContentRegionAvail().x - label_w - 10.0f * scale;
        for (MenuOption& o : g_file.options) {
          if (o.tab != g_file.tabs[size_t(tab_)]) continue;
          ImGui::PushID(o.id.c_str());
          ImGui::AlignTextToFramePadding();
          if (!o.enabled) ImGui::BeginDisabled();
          ImGui::TextUnformatted(o.label.c_str());
          ImGui::SameLine(label_w);
          ImGui::SetNextItemWidth(ctrl_w);
          if (o.kind == "bool") {
            bool b = o.value != 0;
            if (ImGui::Checkbox("##v", &b)) o.value = b ? 1 : 0;
          } else if (o.kind == "choice" && !o.choices.empty()) {
            int i = std::clamp(int(std::lround(o.value)), 0, int(o.choices.size()) - 1);
            if (ImGui::BeginCombo("##v", o.choices[size_t(i)].c_str())) {
              for (int c = 0; c < int(o.choices.size()); ++c) {
                if (ImGui::Selectable(o.choices[size_t(c)].c_str(), c == i)) o.value = c;
                if (c == i) ImGui::SetItemDefaultFocus();
              }
              ImGui::EndCombo();
            }
          } else if (o.kind == "int") {
            int v = int(std::lround(o.value));
            if (ImGui::SliderInt("##v", &v, int(o.min), int(o.max))) {
              const int st = std::max(1, int(o.step));
              o.value = o.min + std::lround((v - o.min) / st) * st;
            }
          } else if (o.kind == "float") {
            float v = float(o.value);
            char fmt[16];
            std::snprintf(fmt, sizeof fmt, "%%.%df", std::max(0, o.decimals));
            if (ImGui::SliderFloat("##v", &v, float(o.min), float(o.max), fmt)) {
              const double st = o.step > 0 ? o.step : 0.1;
              o.value = o.min + std::round((v - o.min) / st) * st;
            }
          }
          const bool focused = ImGui::IsItemFocused() || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
          if (!o.enabled) ImGui::EndDisabled();
          if (focused) tip = (!o.enabled && !o.why.empty() ? o.why + "  " : std::string()) + o.tip;
          ImGui::PopID();
        }
      }
      ImGui::EndChild();
      // the description of the row in focus
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextDisabled("%s", tip.c_str());
      ImGui::PopTextWrapPos();
      ImGui::SetCursorPosY(ImGui::GetWindowHeight() - bar_h - ImGui::GetStyle().WindowPadding.y + ImGui::GetStyle().ItemSpacing.y);
    } else {
      ImGui::TextWrapped("Start the game from COGBox to change its settings here.");
      ImGui::SetCursorPosY(ImGui::GetWindowHeight() - bar_h - ImGui::GetStyle().WindowPadding.y + ImGui::GetStyle().ItemSpacing.y);
    }
    // the bar
    ImGui::Separator();
    const int n = Changes();
    if (n) ImGui::TextDisabled("%d change(s) - they take effect when the game restarts.", n);
    else ImGui::TextDisabled("Settings take effect when the game restarts.");
    const float bw = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
    if (first_frame_) ImGui::SetKeyboardFocusHere();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.10f, 0.08f, 1.0f));   // 0.2.0: Resume is the crimson main button
    if (ImGui::Button("Resume", ImVec2(bw, 0))) done_ = true;
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (!n || g_file.restart_path.empty()) ImGui::BeginDisabled();
    if (ImGui::Button(n ? ("Restart to apply (" + std::to_string(n) + ")").c_str() : "Restart to apply", ImVec2(bw, 0))) Finish(true);
    if (!n || g_file.restart_path.empty()) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Quit game", ImVec2(bw, 0))) Finish(false);
  }

  ImGuiConfigFlags saved_config_ = 0;
  ImGuiBackendFlags saved_backend_ = 0;
  XINPUT_GAMEPAD last_ = {};
  WORD held_at_open_ = 0, last_live_ = 0;
  int tab_ = 0;
  bool first_frame_ = true, done_ = false, popup_last_ = false, close_scheduled_ = false;
  bool unblock_pending_ = false, saved_draw_cursor_ = false, show_cursor_ = true;   // PR #134
  std::chrono::steady_clock::time_point dismissed_at_{};
  ImVec2 prev_mouse_{-FLT_MAX, -FLT_MAX};
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

void OpenNow() {
  if (g_menu || !g_drawer) return;
  g_open.store(true);
  SaveMouseState();
  BlockGameInput(true);
  g_menu = std::make_unique<InGameMenu>(g_drawer);
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

void XenosInstallInGameMenu(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, rex::Runtime* runtime) {
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
  g_menu.reset();
  if (g_open.load()) RestoreMouseState();   // PR #134
  g_unblock_pending.store(false);
  g_open.store(false);
  BlockGameInput(false);   // 0.2.0
  rex::ui::UnregisterBind("bind_cogbox_menu");
  g_drawer = nullptr;
  g_window = nullptr;
  g_input.store(nullptr);
}

bool XenosInGameMenuIsOpen() noexcept { return g_open.load(); }   // PR #134
