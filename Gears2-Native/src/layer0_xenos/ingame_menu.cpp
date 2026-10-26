// Layer 0 (game-free): COGBox's in-game menu. Back + Start on a controller (or F1) opens it over the game; the game
// keeps running underneath, but while the menu is open the controller, keyboard and mouse only work the menu (the SDK's
// input "active" callback says no to the game). The menu shows COGBox's settings for this game, read from the file
// COGBox names in --cogbox_menu (written just before it started the game), with Resume, Restart to apply and Quit game.
// Changes are written to the file COGBox named for them; "Restart to apply" also leaves a restart flag; then both close
// the window the normal way - COGBox applies the changes to its settings and, with the flag, starts the game again.
// Without --cogbox_menu (started by hand) the menu has only Resume and Quit game.
// The controller is read with XInput (loaded at run time, nothing to link) on a small polling thread; while the menu is
// open its buttons are fed to Dear ImGui's gamepad navigation. Our code (MIT).
#include <xenos/dev_tools.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/input/input_system.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>
#include <imgui.h>

#include <windows.h>
#include <Xinput.h>

#include <algorithm>
#include <atomic>
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
REXCVAR_DEFINE_BOOL(cogbox_menu_open_at_start, false, "Gears2",
                    "Test switch: open the in-game menu as soon as the window exists");

namespace {

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

void PollPads() {
  HMODULE xi = LoadLibraryW(L"xinput1_4.dll");
  if (!xi) xi = LoadLibraryW(L"xinput9_1_0.dll");
  auto get = xi ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(xi, "XInputGetState")) : nullptr;
  if (!get) {
    REXLOG_WARN("cogbox_menu: XInput not found - the menu opens with F1 only");
    return;
  }
  bool combo_was = false;
  while (!g_stop.load()) {
    XINPUT_GAMEPAD pad = {};
    bool connected = false;
    for (DWORD i = 0; i < 4 && !connected; ++i) {
      XINPUT_STATE s = {};
      if (get(i, &s) == ERROR_SUCCESS) { pad = s.Gamepad; connected = true; }
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
  }
  ~InGameMenu() override {
    ImGuiIO& io = GetIO();
    io.ConfigFlags = saved_config_;
    io.BackendFlags = saved_backend_;
  }
  bool done() const { return done_; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    FeedPad(io);
    const float scale = std::max(1.0f, io.DisplaySize.y / 900.0f);
    const float w = std::min(io.DisplaySize.x - 40.0f, 900.0f * scale), h = std::min(io.DisplaySize.y - 40.0f, 640.0f * scale);
    ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - w) * 0.5f, (io.DisplaySize.y - h) * 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f * scale, 16.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * scale, 5.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f * scale, 8.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * scale);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.07f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.24f, 0.27f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.12f, 0.12f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.70f, 0.14f, 0.14f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_NavHighlight, ImVec4(0.95f, 0.30f, 0.25f, 1.0f));
    if (first_frame_) ImGui::SetNextWindowFocus();
    if (ImGui::Begin("##cogbox_menu", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoCollapse)) {
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
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(4);
    first_frame_ = false;
    // Closing: the game gets its input back at once; the dialog is dropped by one deferred call on the UI thread
    // (not from inside its own Draw).
    if (done_ && !close_scheduled_) {
      close_scheduled_ = true;
      g_open.store(false);
      ScheduleDrop();
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
      w->app_context().CallInUIThreadDeferred([w] { w->RequestClose(); });
    }
  }

  void DrawContents(float scale) {
    const std::string title = "COGBox  -  " + (g_file.game.empty() ? std::string("Game menu") : g_file.game);
    ImGui::TextUnformatted(title.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("   LB / RB: tabs   A: change   B: back to the game");
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
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1.0f));
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
    if (ImGui::Button("Resume", ImVec2(bw, 0))) done_ = true;
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
};

std::unique_ptr<InGameMenu> g_menu;

// The UI thread owns the dialog: made on request, dropped by one deferred call once it says it's done. (Not a deferred
// call that queues itself again: the SDK runs newly queued calls in the same pass, so that would never return.)
void OpenNow() {
  if (g_menu || !g_drawer) return;
  g_open.store(true);
  g_menu = std::make_unique<InGameMenu>(g_drawer);
  REXLOG_INFO("cogbox_menu: opened");
}

void ScheduleDrop() {
  if (g_window) g_window->app_context().CallInUIThreadDeferred([] { g_menu.reset(); });
}

void RequestOpenOnUIThread() {
  if (g_window) g_window->app_context().CallInUIThreadDeferred([] { OpenNow(); });
}

}  // namespace

void XenosInstallInGameMenu(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, rex::Runtime* runtime) {
  if (!REXCVAR_GET(cogbox_menu_enabled) || !drawer || !window) return;
  g_drawer = drawer;
  g_window = window;
  g_file = LoadMenuFile(REXCVAR_GET(cogbox_menu));
  // While the menu is open the game gets no controller / keyboard / mouse input.
  if (runtime) {
    if (auto* input = static_cast<rex::input::InputSystem*>(runtime->input_system()))
      input->SetActiveCallback([] { return !g_open.load(); });
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
  g_stop.store(true);
  if (g_poller.joinable()) g_poller.join();
  g_menu.reset();
  g_open.store(false);
  rex::ui::UnregisterBind("bind_cogbox_menu");
  g_drawer = nullptr;
  g_window = nullptr;
}
