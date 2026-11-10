// menu5 (RC4-4): the in-game menu's sliders with the pad - an int option with a step > 1 (Judgment's Brightness, 50..150
// step 5) and a float one, driven through xenos::menu::DrawSettings on Dear ImGui with no window, the way
// InGameMenu::FeedPad drives it (own focus on the rows; A enters a slider, then Dear ImGui's gamepad navigation adjusts it).
//   I=../../cogbox/third_party/imgui; g++ -std=c++17 -O1 -I../../src/layer0_xenos/include -I$I slider_step_test.cpp \
//     ../../src/layer0_xenos/menu_look.cpp $I/imgui.cpp $I/imgui_draw.cpp $I/imgui_widgets.cpp $I/imgui_tables.cpp \
//     -o slider_step_test && ./slider_step_test ../../assets/fonts
#include <xenos/menu_look.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <cstdio>
#include <string>
#include <vector>
using namespace xenos::menu;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct Menu {
  Fonts F;
  double bright = 65, gamma = 1.0;   // int 50..150 step 5; float 0.5..1.5 step 0.05
  int focus = -1, last_row = 0, sel = 1;
  bool confirm = false;
  ImGuiKey held = ImGuiKey_None;
  int frame = 0;
  // one frame: 'd'/'u'/'l'/'r' (D-pad), 'a', 'b', '.' (nothing) - as FeedPad sends them
  void step(char k) {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const bool own = g.OpenPopupStack.empty() && g.ActiveId == 0;
    if (held != ImGuiKey_None) { io.AddKeyEvent(held, false); held = ImGuiKey_None; }
    int move = 0, adj = 0;
    ImGuiKey key = k == 'a' ? ImGuiKey_GamepadFaceDown : k == 'b' ? ImGuiKey_GamepadFaceRight : ImGuiKey_None;
    if (own) { move = k == 'd' ? 1 : k == 'u' ? -1 : 0; adj = k == 'r' ? 1 : k == 'l' ? -1 : 0; }
    else if (k == 'd' || k == 'u' || k == 'l' || k == 'r')
      key = k == 'd' ? ImGuiKey_GamepadDpadDown : k == 'u' ? ImGuiKey_GamepadDpadUp : k == 'l' ? ImGuiKey_GamepadDpadLeft : ImGuiKey_GamepadDpadRight;
    if (key != ImGuiKey_None) { io.AddKeyEvent(key, true); held = key; }
    io.DeltaTime = 1 / 60.f;
    ImGui::NewFrame();
    Look L = MakeLook(io.DisplaySize, 1.f, F, false, true);
    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(io.DisplaySize);
    PushStyle(L);
    if (frame == 0) ImGui::SetNextWindowFocus();
    ImGui::Begin("m", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);
    std::vector<std::string> tabs = {"Display"};
    int tab = 0;
    SettingsPage pg; pg.game = "Gears of War: Judgment"; pg.tabs = tabs; pg.tab = &tab;
    Row b; b.id = "Brightness"; b.label = "Brightness"; b.kind = "int"; b.value = &bright; b.min = 50; b.max = 150; b.step = 5; b.live = true;
    Row f; f.id = "Gamma"; f.label = "Gamma"; f.kind = "float"; f.value = &gamma; f.min = 0.5; f.max = 1.5; f.step = 0.05; f.decimals = 2; f.live = true;
    pg.rows = {b, f};
    pg.can_restart = true; pg.focus = &focus; pg.focus_reset = frame == 0; pg.last_row = &last_row;
    pg.confirm_quit = &confirm; pg.confirm_sel = &sel; pg.nav_move = move; pg.nav_adjust = adj;
    DrawSettings(L, pg);
    ImGui::End(); PopStyle(); ImGui::Render();
    frame++;
  }
  void run(const char* s) { for (; *s; ++s) { step(*s); step('.'); step('.'); } }
};

int main(int, char** argv) {
  ImGui::CreateContext(); ImGuiIO& io = ImGui::GetIO(); io.DisplaySize = ImVec2(1920, 1080); io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad; io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  const std::string fd = argv[1] ? argv[1] : "../../assets/fonts";
  FontFiles ff{fd + "/NotoSans-Regular.ttf", fd + "/NotoSans-SemiBold.ttf", fd + "/NotoSans-Bold.ttf", "", ""};
  Menu m;
  m.F = ConfigureFonts(io.Fonts, ff, "x", 1080.f);
  unsigned char* px; int tw, th; io.Fonts->GetTexDataAsRGBA32(&px, &tw, &th);
  m.run("..");
  CHECK(m.focus == 0);
  m.run("rl");                 // right / left on a row only move (to the buttons and back): no value change
  CHECK(m.bright == 65 && m.focus == 0);
  m.run("arrr");               // A enters the slider; three steps of 5 (it moved 1 and snapped back to 65 before)
  CHECK(m.bright == 80);
  m.run("l");
  CHECK(m.bright == 75);
  m.run("a");                  // A leaves; left / right then move again, they don't change it
  m.run("rl");
  CHECK(m.bright == 75);
  m.run("arrrrrrrrrrrrrrrrrrrrrrr");   // stops at the top
  CHECK(m.bright == 150);
  m.run("a");
  m.run("ab");                 // B leaves and puts the value back
  CHECK(m.bright == 150);
  m.run("dar");                // the float row: one step of 0.05
  CHECK(m.gamma > 1.04 && m.gamma < 1.06);
  m.run("a");
  std::printf("slider_step_test: %d passed, %d failed (Brightness %g, Gamma %g)\n", g_pass, g_fail, m.bright, m.gamma);
  return g_fail ? 1 : 0;
}
