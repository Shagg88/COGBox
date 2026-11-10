// Offline mock-up of the in-game menu (0.4.9): runs src/layer0_xenos/menu_look.cpp - the code the game draws the menu
// with - on Dear ImGui with no window, rasterises the draw lists in software and writes RGBA (composited onto a game
// screenshot by mock.py). Build: see mock.sh. Our code (MIT).
#include <xenos/menu_look.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace xenos::menu;
static int W, H; static std::vector<float> img;   // RGBA float, straight alpha over transparent
static const unsigned char* tex; static int tw, th;
static void Sample(float u, float v, float* o) {
  float x = u * tw - 0.5f, y = v * th - 0.5f; int x0 = (int)std::floor(x), y0 = (int)std::floor(y); float fx = x - x0, fy = y - y0;
  for (int c = 0; c < 4; ++c) o[c] = 0;
  for (int j = 0; j < 2; ++j) for (int i = 0; i < 2; ++i) {
    int xx = std::clamp(x0 + i, 0, tw - 1), yy = std::clamp(y0 + j, 0, th - 1); float w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
    for (int c = 0; c < 4; ++c) o[c] += w * tex[(yy * tw + xx) * 4 + c] / 255.f; }
}
static void Tri(const ImDrawVert& a, const ImDrawVert& b, const ImDrawVert& c, ImVec4 clip) {
  float x0 = std::max(clip.x, std::min({a.pos.x, b.pos.x, c.pos.x})), x1 = std::min(clip.z, std::max({a.pos.x, b.pos.x, c.pos.x}));
  float y0 = std::max(clip.y, std::min({a.pos.y, b.pos.y, c.pos.y})), y1 = std::min(clip.w, std::max({a.pos.y, b.pos.y, c.pos.y}));
  float d = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (c.pos.x - a.pos.x) * (b.pos.y - a.pos.y); if (std::fabs(d) < 1e-8f) return;
  for (int y = std::max(0, (int)std::floor(y0)); y < std::min(H, (int)std::ceil(y1)); ++y)
    for (int x = std::max(0, (int)std::floor(x0)); x < std::min(W, (int)std::ceil(x1)); ++x) {
      float px = x + 0.5f, py = y + 0.5f;
      float w1 = ((px - a.pos.x) * (c.pos.y - a.pos.y) - (c.pos.x - a.pos.x) * (py - a.pos.y)) / d;
      float w2 = ((b.pos.x - a.pos.x) * (py - a.pos.y) - (px - a.pos.x) * (b.pos.y - a.pos.y)) / d, w0 = 1 - w1 - w2;
      if (w0 < 0 || w1 < 0 || w2 < 0) continue;
      float u = w0 * a.uv.x + w1 * b.uv.x + w2 * c.uv.x, v = w0 * a.uv.y + w1 * b.uv.y + w2 * c.uv.y, t[4]; Sample(u, v, t);
      float s[4]; const ImDrawVert* V[3] = {&a, &b, &c}; float W3[3] = {w0, w1, w2};
      for (int k = 0; k < 4; ++k) { s[k] = 0; for (int q = 0; q < 3; ++q) s[k] += W3[q] * ((V[q]->col >> (8 * k)) & 255) / 255.f; s[k] *= t[k]; }
      float* o = &img[(y * W + x) * 4]; float al = s[3], oa = o[3] + al * (1 - o[3]);   // "over" in straight alpha
      for (int k = 0; k < 3; ++k) o[k] = oa > 0 ? (s[k] * al + o[k] * o[3] * (1 - al)) / oa : 0; o[3] = oa;
    }
}
int main(int argc, char** argv) {
  W = atoi(argv[1]); H = atoi(argv[2]); const char* fonts = argv[3]; const char* out = argv[4]; bool pad = argc > 5 && argv[5][0] == 'p';
  ImGui::CreateContext(); ImGuiIO& io = ImGui::GetIO(); io.DisplaySize = ImVec2(W, H); io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  FontFiles ff{std::string(fonts) + "/NotoSans-Regular.ttf", std::string(fonts) + "/NotoSans-SemiBold.ttf", std::string(fonts) + "/NotoSans-Bold.ttf", "", ""};
  Fonts F = ConfigureFonts(io.Fonts, ff, "Gears of War 2", (float)H);
  unsigned char* px; io.Fonts->GetTexDataAsRGBA32(&px, &tw, &th); tex = px;
  // the rows: a real menu file from the launcher (argv[6]), the tab to show (argv[7])
  struct Opt { std::string id, tab, label, kind, tip, why; std::vector<std::string> choices; double value = 0, min = 0, max = 1, step = 1;
               int dec = 0; bool enabled = true, live = false; };
  std::vector<Opt> opts; std::vector<std::string> tabs; std::string game = "Gears of War 2";
  if (argc > 6) { FILE* mf = fopen(argv[6], "rb"); char line[8192];
    while (mf && fgets(line, sizeof line, mf)) { std::string l = line; while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
      if (l.rfind("\xEF\xBB\xBF", 0) == 0) l = l.substr(3);
      if (l == "[option]") { opts.emplace_back(); continue; }
      size_t e = l.find('='); if (e == std::string::npos || l[0] == ';') continue; std::string k = l.substr(0, e), v = l.substr(e + 1);
      if (opts.empty()) { if (k == "game") game = v; continue; } Opt& o = opts.back();
      if (k == "id") o.id = v; else if (k == "tab") { o.tab = v; if (std::find(tabs.begin(), tabs.end(), v) == tabs.end()) tabs.push_back(v); }
      else if (k == "label") o.label = v; else if (k == "kind") o.kind = v; else if (k == "tip") o.tip = v; else if (k == "why") o.why = v;
      else if (k == "value") o.value = atof(v.c_str()); else if (k == "min") o.min = atof(v.c_str()); else if (k == "max") o.max = atof(v.c_str());
      else if (k == "step") o.step = atof(v.c_str()); else if (k == "decimals") o.dec = atoi(v.c_str()); else if (k == "enabled") o.enabled = v != "0";
      else if (k == "live") o.live = true;
      else if (k == "choices") { size_t a = 0; for (;;) { size_t b = v.find('|', a); o.choices.push_back(v.substr(a, b - a)); if (b == std::string::npos) break; a = b + 1; } } }
    if (mf) fclose(mf); }
  { static const char* const kOrder[] = {"Display", "Graphics", "Controls", "Gameplay", "Achievements"};   // as ingame_menu.cpp
    auto rank = [](const std::string& t) { for (int i = 0; i < 5; ++i) if (t == kOrder[i]) return i; return 5; };
    std::stable_sort(tabs.begin(), tabs.end(), [&](const std::string& a, const std::string& b) { return rank(a) < rank(b); }); }
  const std::string want = argc > 7 ? argv[7] : "Display";
  static int tab = 0; for (size_t t = 0; t < tabs.size(); ++t) if (tabs[t] == want) tab = int(t);
  // one restart-only change waiting (the first restart-only Graphics row), as after a real change
  std::string pending; for (Opt& o : opts) if (o.tab == "Graphics" && !o.live && o.enabled && o.kind == "bool") { pending = o.id; o.value = o.value ? 0 : 1; break; }
  for (int frame = 0; frame < 12; ++frame) {
    io.DeltaTime = 1 / 60.f;
    if (frame == 4) io.AddKeyEvent(ImGuiKey_LeftArrow, true); if (frame == 5) io.AddKeyEvent(ImGuiKey_LeftArrow, false);
    if (frame == 6) io.AddKeyEvent(ImGuiKey_UpArrow, true); if (frame == 7) io.AddKeyEvent(ImGuiKey_UpArrow, false);
    if (frame == 8) io.AddKeyEvent(ImGuiKey_UpArrow, true); if (frame == 9) io.AddKeyEvent(ImGuiKey_UpArrow, false);
    ImGui::NewFrame();
    Look L = MakeLook(io.DisplaySize, 1.f, F, false, pad);
    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(io.DisplaySize);
    PushStyle(L);
    ImGui::Begin("m", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);
    SettingsPage pg; pg.game = game.c_str(); pg.t_right = "";
    pg.tabs = tabs; pg.tab = &tab;
    for (Opt& o : opts) { if (o.tab != tabs[size_t(tab)]) continue;
      Row r; r.id = o.id; r.label = o.label; r.kind = o.kind; r.tip = o.tip; r.why = o.why; r.choices = &o.choices; r.value = &o.value;
      r.min = o.min; r.max = o.max; r.step = o.step; r.decimals = o.dec; r.enabled = o.enabled; r.live = o.live; r.restart = o.id == pending;
      pg.rows.push_back(r); }
    pg.restart_changes = pending.empty() ? 0 : 1; pg.can_restart = true; pg.focus_resume = frame == 0;
    pg.status = pending.empty() ? "Most settings apply straight away; the others show \"Restart required\"."
                                : "1 change(s) need a restart - Restart to apply.  Other changes are already applied.";
    pg.t_restart_required = "RESTART REQUIRED";
    if (pad) pg.legend = {{false, "LB", ""}, {false, "RB", "tabs"}, {false, "A", "change"}, {false, "B", "back to the game"}, {false, "Start", "resume"}};
    else pg.legend = {{true, "Q", ""}, {true, "E", "tabs"}, {true, "Enter", "change"}, {true, "Esc", "back to the game"}, {true, "F7", "Achievements"}};
    DrawSettings(L, pg);
    ImGui::End(); PopStyle(); ImGui::Render();
  }
  img.assign(W * H * 4, 0.f);
  ImDrawData* dd = ImGui::GetDrawData();
  for (ImDrawList* dl : dd->CmdLists) for (const ImDrawCmd& cmd : dl->CmdBuffer) {
    if (cmd.UserCallback) continue;
    for (unsigned i = 0; i < cmd.ElemCount; i += 3) { const ImDrawIdx* ix = &dl->IdxBuffer[cmd.IdxOffset + i];
      Tri(dl->VtxBuffer[cmd.VtxOffset + ix[0]], dl->VtxBuffer[cmd.VtxOffset + ix[1]], dl->VtxBuffer[cmd.VtxOffset + ix[2]], cmd.ClipRect); } }
  FILE* f = fopen(out, "wb"); std::vector<unsigned char> b(W * H * 4); for (size_t i = 0; i < b.size(); ++i) b[i] = (unsigned char)std::lround(std::clamp(img[i], 0.f, 1.f) * 255);
  fwrite(b.data(), 1, b.size(), f); fclose(f); printf("atlas %dx%d\n", tw, th); return 0;
}
