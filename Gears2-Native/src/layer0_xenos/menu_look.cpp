// Layer 0 (game-free): the in-game menu's look - see xenos/menu_look.h. Our code (MIT).
#include <xenos/menu_look.h>

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace xenos::menu {
namespace {

// The launcher's colours (cogbox/src/theme.cpp, namespace col).
ImVec4 Rgb(int r, int g, int b, float a = 1.f) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); }
const ImVec4 kText = Rgb(226, 221, 208), kDim = Rgb(158, 154, 144), kFaint = Rgb(104, 102, 98);
const ImVec4 kRed = Rgb(158, 20, 26), kRedHi = Rgb(206, 34, 40), kRedDark = Rgb(84, 10, 14);
const ImVec4 kAmber = Rgb(226, 160, 52), kGood = Rgb(124, 178, 98), kBad = Rgb(224, 74, 64);
const ImVec4 kSteel = Rgb(34, 37, 41), kSteelHi = Rgb(54, 58, 64), kEdge = Rgb(112, 118, 126);

ImU32 Col(const ImVec4& c, float a, const Look& L) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * a * L.fade));
}
ImU32 Col(const ImVec4& c, const Look& L) { return Col(c, 1.f, L); }

bool Exists(const std::string& p) {
  if (p.empty()) return false;
  ImFileHandle f = ImFileOpen(p.c_str(), "rb");   // UTF-8 paths, as the atlas opens them
  if (!f) return false;
  ImFileClose(f);
  return true;
}

void Codepoints(const std::string& s, std::vector<uint32_t>* out) {
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = 0;
    int len = 1;
    if (c < 0x80) cp = c;
    else if ((c >> 5) == 0x6) { cp = c & 0x1F; len = 2; }
    else if ((c >> 4) == 0xE) { cp = c & 0x0F; len = 3; }
    else if ((c >> 3) == 0x1E) { cp = c & 0x07; len = 4; }
    else { ++i; continue; }
    if (i + len > s.size()) break;
    for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    out->push_back(cp);
    i += len;
  }
}

std::vector<ImWchar> Ranges(const std::vector<uint32_t>& cps) {
  ImFontGlyphRangesBuilder b;
  for (uint32_t cp : cps)
    if (cp <= 0xFFFF) b.AddChar(static_cast<ImWchar>(cp));
  ImVector<ImWchar> r;
  b.BuildRanges(&r);
  return std::vector<ImWchar>(r.begin(), r.end());
}

void Chamfer(ImDrawList* dl, ImVec2 a, ImVec2 b, float c) {
  dl->PathLineTo(ImVec2(a.x + c, a.y)); dl->PathLineTo(ImVec2(b.x - c, a.y)); dl->PathLineTo(ImVec2(b.x, a.y + c));
  dl->PathLineTo(ImVec2(b.x, b.y - c)); dl->PathLineTo(ImVec2(b.x - c, b.y)); dl->PathLineTo(ImVec2(a.x + c, b.y));
  dl->PathLineTo(ImVec2(a.x, b.y - c)); dl->PathLineTo(ImVec2(a.x, a.y + c));
}

ImFont* Or(ImFont* f, ImFont* g) { return f ? f : g ? g : ImGui::GetFont(); }

}  // namespace

// ---- fonts -----------------------------------------------------------------------------------------------------------
Fonts ConfigureFonts(ImFontAtlas* atlas, const FontFiles& files, const std::string& text, float bake_height) {
  Fonts f;
  if (!atlas || !Exists(files.regular)) return f;
  f.bake_height = std::clamp(bake_height, 720.f, 2160.f);
  float bu = f.bake_height / 1080.f;
  // Noto Sans covers Latin, Greek and Cyrillic; everything else goes to the PC's fonts below.
  std::vector<uint32_t> cps, hangul, han;
  for (uint32_t c = 0x20; c <= 0xFF; ++c) if (c < 0x7F || c >= 0xA0) cps.push_back(c);
  for (uint32_t c : {0x2013u, 0x2014u, 0x2018u, 0x2019u, 0x201Cu, 0x201Du, 0x2022u, 0x2026u, 0x2039u, 0x203Au, 0x00D7u})
    cps.push_back(c);
  std::vector<uint32_t> all;
  Codepoints(text, &all);
  for (uint32_t cp : all) {
    if ((cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F)) hangul.push_back(cp);
    else if ((cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xFF00 && cp <= 0xFFEF)) han.push_back(cp);
    else if (cp >= 0x20) cps.push_back(cp);
  }
  // The atlas keeps these pointers until it is built.
  static std::vector<ImWchar> noto_ranges, title_ranges, hangul_ranges, han_ranges;
  noto_ranges = Ranges(cps);
  hangul_ranges = Ranges(hangul);
  han_ranges = Ranges(han);
  static const ImWchar kAscii[] = {0x0020, 0x007E, 0};
  const bool have_hangul = hangul_ranges.size() > 1 && Exists(files.hangul);
  const bool have_han = han_ranges.size() > 1 && Exists(files.han);
  // 0.4.9 review (atlas size): the CJK glyphs are already only the ones the menu's text uses, but they are baked once
  // per font size (4 sizes). On a 4K screen with a long CJK text that grows the atlas fast, so with more than 1500
  // CJK glyphs the bake is capped at 1440p (drawn scaled up a little on larger screens) - roughly half the atlas.
  if ((have_hangul || have_han) && hangul.size() + han.size() > 1500 && f.bake_height > 1440.f) {
    f.bake_height = 1440.f;
    bu = f.bake_height / 1080.f;
  }
  auto add = [&](const std::string& file, float units, const ImWchar* ranges, bool cjk) -> ImFont* {
    if (!Exists(file)) return nullptr;
    ImFontConfig cfg;
    cfg.OversampleH = 1;   // baked at the size it is drawn at: no oversampling needed (and half the atlas)
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    ImFont* font = atlas->AddFontFromFileTTF(file.c_str(), std::round(units * bu), &cfg, ranges);
    if (!font || !cjk) return font;
    ImFontConfig m;
    m.MergeMode = true;
    m.OversampleH = m.OversampleV = 1;
    if (have_hangul) atlas->AddFontFromFileTTF(files.hangul.c_str(), std::round(units * bu), &m, hangul_ranges.data());
    if (have_han) atlas->AddFontFromFileTTF(files.han.c_str(), std::round(units * bu), &m, han_ranges.data());
    return font;
  };
  f.regular = add(files.regular, kBody, noto_ranges.data(), true);
  f.regular_small = add(files.regular, kSmall, noto_ranges.data(), true);
  f.semibold = add(Exists(files.semibold) ? files.semibold : files.regular, kBody, noto_ranges.data(), true);
  f.semibold_small = add(Exists(files.semibold) ? files.semibold : files.regular, kTag, noto_ranges.data(), true);
  // the title: COGBOX and the game's name (ASCII), in Noto Sans Bold
  f.bold = add(Exists(files.bold) ? files.bold : files.semibold, kTitle, kAscii, false);
  return f;
}

// ---- the frame ----------------------------------------------------------------------------------------------------
Look MakeLook(ImVec2 display, float fade, const Fonts& fonts, bool ps, bool pad) {
  Look L;
  L.display = display;
  L.u = std::max(0.5f, display.y / 1080.f);
  const float w = std::min(display.x, display.y * 16.f / 9.f);
  L.c0 = ImVec2(std::floor((display.x - w) * 0.5f), 0.f);
  L.c1 = ImVec2(L.c0.x + w, display.y);
  L.fade = std::clamp(fade, 0.f, 1.f);
  L.ps = ps;
  L.pad = pad;
  L.fonts = fonts;
  return L;
}

float Text(ImDrawList* dl, const Look& L, ImFont* f, float size_units, ImVec2 p, ImU32 col, const char* text,
           float wrap_w) {
  f = Or(f, L.fonts.regular);
  const float s = L.px(size_units);
  dl->AddText(f, s, ImVec2(std::round(p.x), std::round(p.y)), col, text, nullptr, wrap_w);
  return f->CalcTextSizeA(s, FLT_MAX, wrap_w, text).x;
}
ImVec2 TextSize(const Look& L, ImFont* f, float size_units, const char* text, float wrap_w) {
  f = Or(f, L.fonts.regular);
  return f->CalcTextSizeA(L.px(size_units), FLT_MAX, wrap_w, text);
}

void PushStyle(const Look& L) {
  const float u = L.u;
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, L.fade);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14 * u, 9 * u));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10 * u, 8 * u));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2 * u);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 2 * u);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 2 * u);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(1.f, u));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, std::max(1.f, u));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 12 * u);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 22 * u);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.07f, 0.075f, 0.085f, 0.98f));
  ImGui::PushStyleColor(ImGuiCol_Text, kText);
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, kFaint);
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(kEdge.x, kEdge.y, kEdge.z, 0.55f));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.075f, 0.08f, 0.09f, 0.95f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.14f, 0.15f, 0.17f, 1.f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.18f, 0.19f, 0.21f, 1.f));
  ImGui::PushStyleColor(ImGuiCol_Button, kSteelHi);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.27f, 0.29f, 0.32f, 1.f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, kRedDark);
  ImGui::PushStyleColor(ImGuiCol_CheckMark, kRedHi);
  ImGui::PushStyleColor(ImGuiCol_SliderGrab, kRed);
  ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, kRedHi);
  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(kRed.x, kRed.y, kRed.z, 0.55f));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(kRed.x, kRed.y, kRed.z, 0.80f));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, kRedHi);
  ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(kEdge.x, kEdge.y, kEdge.z, 0.35f));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0.2f));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, kSteelHi);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, kEdge);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, kRedHi);
  ImGui::PushStyleColor(ImGuiCol_NavCursor, kText);   // accessibility: a bright focus ring, not a dark red one
}
void PopStyle() {
  ImGui::PopStyleColor(24);
  ImGui::PopStyleVar(14);
}

void Backdrop(ImDrawList* dl, const Look& L) {
  const ImVec2 d = L.display;
  dl->PushClipRectFullScreen();
  // the game stays visible, dimmed: darker at the top and bottom where the header and legend sit
  const ImU32 top = Col(ImVec4(0.02f, 0.02f, 0.025f, 0.96f), L), mid = Col(ImVec4(0.02f, 0.02f, 0.025f, 0.70f), L);
  const float h = d.y;
  dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(d.x, h * 0.25f), top, top, mid, mid);
  dl->AddRectFilled(ImVec2(0, h * 0.25f), ImVec2(d.x, h * 0.75f), mid);
  dl->AddRectFilledMultiColor(ImVec2(0, h * 0.75f), ImVec2(d.x, h), mid, mid, top, top);
  // ultrawide: the sides darker still, with a soft edge into the content area
  if (L.c0.x > 1.f) {
    const ImU32 side = Col(ImVec4(0, 0, 0, 0.45f), L), none = Col(ImVec4(0, 0, 0, 0), L);
    const float e = 48 * L.u;
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(L.c0.x - e, h), side);
    dl->AddRectFilledMultiColor(ImVec2(L.c0.x - e, 0), ImVec2(L.c0.x, h), side, none, none, side);
    dl->AddRectFilled(ImVec2(L.c1.x + e, 0), ImVec2(d.x, h), side);
    dl->AddRectFilledMultiColor(ImVec2(L.c1.x, 0), ImVec2(L.c1.x + e, h), none, side, side, none);
  }
  // (menu3: no crimson line along the very top any more - Shagg; the tab underline and row marker keep the red)
  dl->PopClipRect();
}

void Plate(ImDrawList* dl, ImVec2 a, ImVec2 b, float c, const Look& L, bool lit, float al) {
  const float u = L.u;
  Chamfer(dl, ImVec2(a.x + 3 * u, a.y + 4 * u), ImVec2(b.x + 3 * u, b.y + 4 * u), c);
  dl->PathFillConvex(Col(ImVec4(0, 0, 0, 0.45f), al, L));
  const int v0 = dl->VtxBuffer.Size;
  Chamfer(dl, a, b, c);
  dl->PathFillConvex(IM_COL32_WHITE);
  ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, a, ImVec2(a.x, b.y),
                                                Col(ImVec4(0.115f, 0.125f, 0.14f, 0.94f), al, L),
                                                Col(ImVec4(0.055f, 0.06f, 0.068f, 0.94f), al, L));
  Chamfer(dl, a, b, c);
  dl->PathStroke(lit ? Col(kRedHi, 0.9f * al, L) : Col(kEdge, 0.45f * al, L), ImDrawFlags_Closed, std::max(1.f, 1.2f * u));
  dl->AddLine(ImVec2(a.x + c, a.y + u), ImVec2(b.x - c, a.y + u), Col(ImVec4(1, 1, 1, 0.12f), al, L), std::max(1.f, u));
  // four rivets
  const float r = 3.2f * u, in = c * 0.5f + 7 * u;
  if (b.x - a.x > 6 * in && b.y - a.y > 3 * in)
    for (ImVec2 p : {ImVec2(a.x + in, a.y + in), ImVec2(b.x - in, a.y + in), ImVec2(a.x + in, b.y - in), ImVec2(b.x - in, b.y - in)}) {
      dl->AddCircleFilled(ImVec2(p.x + r * 0.3f, p.y + r * 0.4f), r, Col(ImVec4(0, 0, 0, 0.55f), al, L), 12);
      dl->AddCircleFilled(p, r, Col(ImVec4(0.30f, 0.32f, 0.35f, 1), al, L), 12);
      dl->AddCircleFilled(ImVec2(p.x - r * 0.3f, p.y - r * 0.3f), r * 0.45f, Col(ImVec4(0.62f, 0.64f, 0.67f, 0.8f), al, L), 8);
    }
}

float Header(ImDrawList* dl, const Look& L, const char* game, const char* right) {
  const float u = L.u, x = L.c0.x + 56 * u, y = 34 * u, x1 = L.c1.x - 56 * u;
  const float th = L.px(kTitle);
  dl->AddRectFilled(ImVec2(x, y + th * 0.16f), ImVec2(x + 6 * u, y + th * 1.04f), Col(kRedHi, L));
  float tx = x + 20 * u;
  tx += Text(dl, L, L.fonts.bold, kTitle, ImVec2(tx, y), Col(kText, L), "COGBOX");
  if (game && *game) {
    tx += 18 * u;
    const float by = y + th * 0.80f - L.px(kBody) * 0.80f;   // the same baseline as the title
    Text(dl, L, L.fonts.regular, kBody, ImVec2(tx, by), Col(kDim, L), game);
  }
  if (right && *right) {
    const ImVec2 s = TextSize(L, L.fonts.regular_small, kSmall, right);
    Text(dl, L, L.fonts.regular_small, kSmall, ImVec2(x1 - s.x, y + th * 0.80f - L.px(kSmall) * 0.80f), Col(kDim, L), right);
  }
  const float rule = y + th * 1.04f + 14 * u;
  dl->AddLine(ImVec2(x, rule), ImVec2(x1, rule), Col(kEdge, 0.30f, L), std::max(1.f, u));
  return rule;
}

void Heading(ImDrawList* dl, const Look& L, ImVec2 p, const char* text, float x1) {
  const float u = L.u, h = L.px(kBody);
  dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.18f), ImVec2(p.x + 4 * u, p.y + h * 1.06f), Col(kRedHi, L));
  const float w = Text(dl, L, L.fonts.semibold, kBody, ImVec2(p.x + 14 * u, p.y), Col(kText, L), text);
  const float tx = p.x + 14 * u + w + 14 * u;
  if (x1 > tx) dl->AddLine(ImVec2(tx, p.y + h * 0.66f), ImVec2(x1, p.y + h * 0.66f), Col(kEdge, 0.35f, L), std::max(1.f, u));
}

namespace {
// A controller button / key picture centred at c with height h (launcher front.cpp); returns its width.
float Glyph(ImDrawList* dl, const Look& L, ImVec2 left_mid, float h, bool key, const char* name, bool draw = true) {
  const bool face = !key && name[1] == 0 && std::strchr("ABXY", name[0]);
  const float fs = h * 0.62f;
  ImFont* f = Or(L.fonts.semibold_small, L.fonts.semibold);
  std::string shown = name;
  if (!key && L.ps) {   // PlayStation names for the non-face buttons
    if (shown == "LB") shown = "L1"; else if (shown == "RB") shown = "R1";
    else if (shown == "LT") shown = "L2"; else if (shown == "RT") shown = "R2";
    else if (shown == "Start") shown = "Options"; else if (shown == "Back") shown = "Share";
  }
  if (face) {
    const float r = h * 0.5f;
    if (!draw) return h;
    const ImVec2 c(left_mid.x + r, left_mid.y);
    if (!L.ps) {
      const ImVec4 k = name[0] == 'A' ? Rgb(80, 170, 60) : name[0] == 'B' ? Rgb(200, 50, 50) : name[0] == 'X' ? Rgb(40, 110, 200) : Rgb(220, 180, 40);
      dl->AddCircleFilled(c, r, Col(k, L), 32);
      const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, name);
      dl->AddText(f, fs, ImVec2(std::round(c.x - ts.x * 0.5f), std::round(c.y - ts.y * 0.5f)), Col(ImVec4(1, 1, 1, 1), L), name);
    } else {
      dl->AddCircleFilled(c, r, Col(Rgb(40, 40, 46), L), 32);
      const float q = r * 0.46f, t = std::max(1.5f, 2.f * L.u);
      if (name[0] == 'A') {
        const ImU32 k = Col(Rgb(120, 160, 230), L);
        dl->AddLine(ImVec2(c.x - q, c.y - q), ImVec2(c.x + q, c.y + q), k, t);
        dl->AddLine(ImVec2(c.x - q, c.y + q), ImVec2(c.x + q, c.y - q), k, t);
      } else if (name[0] == 'B') dl->AddCircle(c, q, Col(Rgb(230, 90, 100), L), 24, t);
      else if (name[0] == 'X') dl->AddRect(ImVec2(c.x - q, c.y - q), ImVec2(c.x + q, c.y + q), Col(Rgb(220, 120, 200), L), 0, 0, t);
      else dl->AddTriangle(ImVec2(c.x, c.y - q), ImVec2(c.x + q, c.y + q * 0.8f), ImVec2(c.x - q, c.y + q * 0.8f), Col(Rgb(60, 200, 170), L), t);
    }
    return h;
  }
  const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, shown.c_str());
  const float w = std::max(h, ts.x + h * 0.6f);
  if (!draw) return w;
  const ImVec2 a(left_mid.x, left_mid.y - h * 0.5f), b(left_mid.x + w, left_mid.y + h * 0.5f);
  dl->AddRectFilled(a, b, Col(Rgb(48, 48, 54), L), 4 * L.u);
  dl->AddRect(a, b, Col(Rgb(110, 110, 118), L), 4 * L.u, 0, std::max(1.f, L.u));
  dl->AddText(f, fs, ImVec2(std::round(a.x + (w - ts.x) * 0.5f), std::round(left_mid.y - ts.y * 0.5f)), Col(Rgb(235, 235, 235), L), shown.c_str());
  return w;
}
}  // namespace

int Tabs(const Look& L, ImVec2 a, ImVec2 b, const std::vector<std::string>& names, int current, const char* prev,
         const char* next) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float u = L.u, h = b.y - a.y, gh = h * 0.56f, gap = 6 * u;
  float x0 = a.x, x1 = b.x;
  if (prev && *prev) x0 += Glyph(dl, L, ImVec2(a.x, (a.y + b.y) * 0.5f), gh, !L.pad, prev) + 14 * u;
  if (next && *next) {
    const float w = Glyph(dl, L, ImVec2(0, 0), gh, !L.pad, next, false);
    Glyph(dl, L, ImVec2(b.x - w, (a.y + b.y) * 0.5f), gh, !L.pad, next);
    x1 -= w + 14 * u;
  }
  const int n = int(names.size());
  if (n == 0) return -1;
  const float tw = (x1 - x0 - gap * (n - 1)) / n;
  int clicked = -1;
  for (int i = 0; i < n; ++i) {
    const ImVec2 ta(std::round(x0 + i * (tw + gap)), a.y), tb(std::round(x0 + i * (tw + gap) + tw), b.y);
    ImGui::SetCursorScreenPos(ta);
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("##tab", ImVec2(tb.x - ta.x, tb.y - ta.y), ImGuiButtonFlags_None)) clicked = i;
    const bool hov = ImGui::IsItemHovered(), sel = i == current;
    ImGui::PopID();
    const float c = 10 * u;
    dl->PathLineTo(ImVec2(ta.x, tb.y)); dl->PathLineTo(ImVec2(ta.x, ta.y + c)); dl->PathLineTo(ImVec2(ta.x + c, ta.y));
    dl->PathLineTo(ImVec2(tb.x - c, ta.y)); dl->PathLineTo(ImVec2(tb.x, ta.y + c)); dl->PathLineTo(ImVec2(tb.x, tb.y));
    dl->PathFillConvex(sel ? Col(kSteelHi, 0.98f, L) : hov ? Col(kSteel, 0.95f, L) : Col(ImVec4(0.07f, 0.075f, 0.085f, 0.88f), L));
    if (sel) dl->AddRectFilled(ImVec2(ta.x + c, ta.y), ImVec2(tb.x - c, ta.y + 4 * u), Col(kRedHi, L));
    std::string up = names[size_t(i)];
    for (char& ch : up) if (ch >= 'a' && ch <= 'z') ch = char(ch - 32);   // ASCII only (other scripts as they are)
    ImFont* f = L.fonts.semibold;
    const ImVec2 ts = TextSize(L, f, kBody * 0.86f, up.c_str());
    Text(dl, L, f, kBody * 0.86f, ImVec2(ta.x + (tb.x - ta.x - ts.x) * 0.5f, ta.y + (h - ts.y) * 0.5f + 2 * u),
         Col(sel ? kText : kDim, L), up.c_str());
  }
  // the line the tabs stand on
  dl->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), Col(kRedHi, 0.55f, L), std::max(1.f, 1.5f * u));
  return clicked;
}

bool Button(const Look& L, const char* label, ImVec2 size, bool red, bool enabled) {
  ImGui::PushID(label);
  if (!enabled) ImGui::BeginDisabled();
  const ImVec2 a = ImGui::GetCursorScreenPos();
  ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));   // 0.5.0: our own focus look only (no square ImGui rect)
  const bool clicked = ImGui::InvisibleButton("##b", size, ImGuiButtonFlags_EnableNav);
  ImGui::PopStyleColor();
  const bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
  const bool focused = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
  if (!enabled) ImGui::EndDisabled();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 b(a.x + size.x, a.y + size.y);
  const float u = L.u, c = std::min(size.y * 0.30f, 12 * u), al = enabled ? 1.f : 0.40f;
  Chamfer(dl, ImVec2(a.x + 2 * u, a.y + 3 * u), ImVec2(b.x + 2 * u, b.y + 3 * u), c);
  dl->PathFillConvex(Col(ImVec4(0, 0, 0, 0.5f), al, L));
  ImVec4 top;
  if (red) top = held ? kRedDark : (hov || focused) ? kRedHi : kRed;
  else top = held ? kSteel : (hov || focused) ? Rgb(69, 74, 82) : kSteelHi;
  const ImVec4 bot(top.x * 0.58f, top.y * 0.58f, top.z * 0.58f, 1);
  const int v0 = dl->VtxBuffer.Size;
  Chamfer(dl, a, b, c);
  dl->PathFillConvex(IM_COL32_WHITE);
  ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, a, ImVec2(a.x, b.y), Col(top, al, L), Col(bot, al, L));
  Chamfer(dl, a, b, c);
  // 0.5.0 (Shagg: the focused button "looks terrible"): one soft light edge, no outer ring (it was clipped at small
  // boxes' edges and left stray marks); the brighter fill above already shows which button is chosen.
  dl->PathStroke(focused ? Col(ImVec4(1.f, 0.93f, 0.90f, 0.85f), L) : red ? Col(Rgb(255, 115, 102), 0.6f * al, L) : Col(kEdge, 0.6f * al, L),
                 ImDrawFlags_Closed, std::max(1.f, (focused ? 1.6f : 1.f) * u));
  dl->AddLine(ImVec2(a.x + c, a.y + u), ImVec2(b.x - c, a.y + u), Col(ImVec4(1, 1, 1, 0.20f), al, L), std::max(1.f, u));
  const char* end = ImGui::FindRenderedTextEnd(label);
  const std::string text(label, end);
  ImFont* f = L.fonts.semibold;
  const ImVec2 ts = TextSize(L, f, kBody * 0.92f, text.c_str());
  const ImVec2 tp(a.x + (size.x - ts.x) * 0.5f, a.y + (size.y - ts.y) * 0.5f + (held ? u : 0));
  Text(dl, L, f, kBody * 0.92f, ImVec2(tp.x + u, tp.y + u), Col(ImVec4(0, 0, 0, 0.6f), al, L), text.c_str());
  Text(dl, L, f, kBody * 0.92f, tp, Col(kText, al, L), text.c_str());
  ImGui::PopID();
  return clicked && enabled;
}

bool Toggle(const Look& L, const char* id, bool* v, ImVec2 size, const char* on_text, const char* off_text) {
  // only the switch and its word take focus / clicks (a compact focus ring), right-aligned in `size`
  {
    const float th0 = std::min(size.y, 32 * L.u);
    const float ww = std::max(TextSize(L, L.fonts.regular, kBody, on_text).x, TextSize(L, L.fonts.regular, kBody, off_text).x);
    const float w = std::min(size.x, th0 * 2.f + 14 * L.u + ww + 20 * L.u);
    const ImVec2 c = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(c.x + size.x - w, c.y));
    size.x = w;
  }
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
  if (clicked) *v = !*v;
  const bool hov = ImGui::IsItemHovered();
  const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
  const float al = disabled ? 0.40f : 1.f;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float u = L.u, th = std::min(size.y, 32 * u), tw = th * 2.0f;
  const ImVec2 t0(a.x + size.x - tw, a.y + (size.y - th) * 0.5f), t1(t0.x + tw, t0.y + th);
  dl->AddRectFilled(t0, t1, *v ? Col(kRed, al, L) : Col(ImVec4(0.10f, 0.105f, 0.12f, 1.f), al, L), th * 0.5f);
  dl->AddRect(t0, t1, *v ? Col(kRedHi, al, L) : Col(kEdge, (hov ? 0.9f : 0.6f) * al, L), th * 0.5f, 0, std::max(1.f, u));
  const float r = th * 0.5f - 4 * u;
  const ImVec2 kc(*v ? t1.x - th * 0.5f : t0.x + th * 0.5f, (t0.y + t1.y) * 0.5f);
  dl->AddCircleFilled(ImVec2(kc.x + u, kc.y + 1.5f * u), r, Col(ImVec4(0, 0, 0, 0.4f), al, L), 24);
  dl->AddCircleFilled(kc, r, Col(*v ? kText : Rgb(150, 154, 160), al, L), 24);
  const char* word = *v ? on_text : off_text;
  const ImVec2 ws = TextSize(L, L.fonts.regular, kBody, word);
  Text(dl, L, L.fonts.regular, kBody, ImVec2(t0.x - 14 * u - ws.x, a.y + (size.y - ws.y) * 0.5f),
       Col(*v ? kText : kDim, al, L), word);
  return clicked;
}

float Tag(ImDrawList* dl, const Look& L, ImVec2 p, const char* text, ImVec4 colour) {
  const float u = L.u;
  ImFont* f = Or(L.fonts.semibold_small, L.fonts.semibold);
  const ImVec2 ts = TextSize(L, f, kTag, text);
  const ImVec2 a = p, b(p.x + ts.x + 16 * u, p.y + ts.y + 6 * u);
  const float c = 5 * u;
  Chamfer(dl, a, b, c);
  dl->PathFillConvex(Col(colour, 0.16f, L));
  Chamfer(dl, a, b, c);
  dl->PathStroke(Col(colour, 0.85f, L), ImDrawFlags_Closed, std::max(1.f, u));
  Text(dl, L, f, kTag, ImVec2(a.x + 8 * u, a.y + 3 * u), Col(colour, L), text);
  return b.x - a.x;
}

void Lamp(ImDrawList* dl, const Look& L, ImVec2 c, float r, int state) {
  const ImVec4 k = state == 1 ? kGood : state == 2 ? kAmber : state == 3 ? kBad : kFaint;
  if (state) dl->AddCircleFilled(c, r * 2.f, Col(k, 0.18f, L), 16);
  dl->AddCircleFilled(c, r, Col(k, L), 16);
  dl->AddCircleFilled(ImVec2(c.x - r * 0.3f, c.y - r * 0.3f), r * 0.35f, Col(ImVec4(1, 1, 1, 0.35f), L), 8);
}

void Legend(ImDrawList* dl, const Look& L, const std::vector<LegendItem>& items) {
  const float u = L.u, gh = 34 * u, y = L.display.y - 30 * u - gh * 0.5f, gap = 30 * u;
  ImFont* f = L.fonts.regular_small;
  float total = 0;
  for (const LegendItem& it : items)
    total += Glyph(dl, L, ImVec2(0, 0), gh, it.key, it.name, false) + 10 * u + TextSize(L, f, kSmall, it.label.c_str()).x + gap;
  total -= gap;
  float x = std::max(L.c0.x + 56 * u, (L.c0.x + L.c1.x - total) * 0.5f);
  for (const LegendItem& it : items) {
    x += Glyph(dl, L, ImVec2(x, y), gh, it.key, it.name) + 10 * u;
    const ImVec2 ls = TextSize(L, f, kSmall, it.label.c_str());
    x += Text(dl, L, f, kSmall, ImVec2(x, y - ls.y * 0.5f), Col(kDim, L), it.label.c_str()) + gap;
  }
}

// ---- the settings page --------------------------------------------------------------------------------------------
Action DrawSettings(const Look& L, SettingsPage& pg) {
  Action act = Action::kNone;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float u = L.u, mx = 56 * u;
  Backdrop(dl, L);
  const float rule = Header(dl, L, pg.game, pg.t_right);
  const float x0 = L.c0.x + mx, x1 = L.c1.x - mx;
  const float legend_top = L.display.y - 72 * u;
  // the tabs
  float body_top = rule + 22 * u;
  if (!pg.tabs.empty() && pg.tab) {
    const float tab_h = 54 * u;
    const int c = Tabs(L, ImVec2(x0, body_top), ImVec2(x1, body_top + tab_h), pg.tabs, *pg.tab, L.pad ? "LB" : "Q",
                       L.pad ? "RB" : "E");
    if (c >= 0) *pg.tab = c;
    body_top += tab_h + 22 * u;
  }
  const float body_bot = legend_top - 18 * u;
  // two columns: the rows (left), the selected row's description and the buttons (right)
  const float gap = 28 * u, right_w = std::clamp((x1 - x0) * 0.33f, 360 * u, 560 * u);
  const ImVec2 la(x0, body_top), lb(x1 - right_w - gap, body_bot);
  const ImVec2 ra(x1 - right_w, body_top), rb(x1, body_bot);
  Plate(dl, la, lb, 16 * u, L);
  // the buttons, bottom of the right column
  const float bh = 58 * u, bgap = 14 * u;
  const int nbuttons = (pg.has_file ? 4 : 2) + (pg.has_file && pg.t_social ? 1 : 0);   // social step 5: + Social
  const float buttons_top = rb.y - nbuttons * bh - (nbuttons - 1) * bgap;
  // the status note sits inside the description plate, in its own strip at the bottom (opaque, full panel width)
  const float status_w = right_w - 60 * u;
  const float status_h = pg.status.empty() ? 0.f : TextSize(L, L.fonts.regular_small, kSmall, pg.status.c_str(), status_w).y + 36 * u;
  const ImVec2 da(ra.x, ra.y), db(rb.x, buttons_top - 22 * u);
  Plate(dl, da, db, 16 * u, L);

  // ---- review 049: our own focus (see SettingsPage::focus)
  const int n_rows = int(pg.rows.size());
  struct Btn { std::string label; bool red, enabled; Action a; };
  std::vector<Btn> btns;
  btns.push_back({pg.t_resume, true, true, Action::kResume});
  if (pg.has_file) {
    btns.push_back({pg.t_achievements, false, true, Action::kAchievements});
    if (pg.t_social) btns.push_back({pg.t_social, false, true, Action::kSocial});
    std::string rs = pg.t_restart;
    if (pg.restart_changes) rs += " (" + std::to_string(pg.restart_changes) + ")";
    btns.push_back({rs, false, pg.can_restart && pg.restart_changes > 0, Action::kRestart});
  }
  btns.push_back({pg.t_quit, false, true, Action::kQuit});
  const bool confirming = pg.confirm_quit && *pg.confirm_quit;
  const int n_all = n_rows + int(btns.size());
  bool want_focus = false;   // put Dear ImGui's nav focus on *pg.focus this frame
  static bool s_refocus = false;   // the quit box closed with No: focus back on Quit game next frame
  if (pg.focus && !confirming) {
    int& f = *pg.focus;
    if (s_refocus) { s_refocus = false; want_focus = true; }
    if (pg.focus_reset || f < 0) { f = n_rows ? 0 : n_rows; want_focus = true; }
    else if (f >= n_all) { f = n_all - 1; want_focus = true; }   // past the end (the menu's "back on Quit game")
    // menu2 (Shagg's spec): up / down move within the rows, or within the buttons; right from a row jumps to the
    // buttons (Resume first), left from the buttons goes back to the row it came from. Left / right never change a value.
    static int s_last_row = 0;
    int& last_row = pg.last_row ? *pg.last_row : s_last_row;
    auto first_button = [&]() { for (int k = 0; k < int(btns.size()); ++k) if (btns[size_t(k)].enabled) return n_rows + k; return n_rows; };
    if (pg.nav_move) {
      if (f < n_rows) {
        f = std::clamp(f + (pg.nav_move > 0 ? 1 : -1), 0, n_rows - 1);
      } else {
        int k = f;
        for (int step = 0; step < int(btns.size()); ++step) {   // the next enabled button (disabled ones skipped)
          const int nk = std::clamp(k + (pg.nav_move > 0 ? 1 : -1), n_rows, n_all - 1);
          if (nk == k) break;
          k = nk;
          if (btns[size_t(k - n_rows)].enabled) { f = k; break; }
        }
      }
      want_focus = true;
    }
    if (pg.nav_adjust > 0 && f < n_rows) { last_row = f; f = first_button(); want_focus = true; }
    else if (pg.nav_adjust < 0 && f >= n_rows && n_rows) { f = std::clamp(last_row, 0, n_rows - 1); want_focus = true; }
    if (f < n_rows) last_row = f;
  }
  auto take_focus = [&](int index) {   // after the item: focus it when asked; follow Dear ImGui (mouse / keys) otherwise
    if (!pg.focus || confirming) return;
    const ImGuiID id = ImGui::GetItemID();
    if (want_focus && *pg.focus == index) {
      ImGui::SetFocusID(id, ImGui::GetCurrentContext()->CurrentWindow);
      ImGui::SetNavCursorVisible(true);
      ImGui::ScrollToItem();
    } else if (!want_focus && id && ImGui::GetCurrentContext()->NavId == id) {
      *pg.focus = index;
    }
  };

  // ---- the rows
  std::string tip_label, tip_text, tip_why;
  int tip_state = 0;   // 1 live, 2 waiting for the restart, 4 needs a restart to change
  bool any_focus = false;
  ImGui::SetCursorScreenPos(ImVec2(la.x + 10 * u, la.y + 14 * u));
  if (pg.rows.empty() && pg.t_no_file && *pg.t_no_file) {
    Text(dl, L, L.fonts.regular, kBody, ImVec2(la.x + 32 * u, la.y + 32 * u), Col(kDim, L), pg.t_no_file, lb.x - la.x - 64 * u);
  }
  if (confirming) ImGui::BeginDisabled();
  if (ImGui::BeginChild("##rows", ImVec2(lb.x - la.x - 20 * u, lb.y - la.y - 28 * u), ImGuiChildFlags_NavFlattened)) {
    ImDrawList* cl = ImGui::GetWindowDrawList();
    const float row_h = 62 * u, rw = ImGui::GetContentRegionAvail().x;
    const float ctrl_w = std::clamp(rw * 0.42f, 240 * u, 420 * u);
    const float frame_h = L.px(kBody) + 2 * 9 * u;
    for (size_t i = 0; i < pg.rows.size(); ++i) {
      Row& r = pg.rows[i];
      ImGui::PushID(r.id.c_str());
      const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + rw, a.y + row_h);
      ImDrawListSplitter split;
      split.Split(cl, 2);
      split.SetCurrentChannel(cl, 1);
      // the label, then the restart tag
      const float lx = a.x + 26 * u, ly = a.y + (row_h - L.px(kBody)) * 0.5f - 2 * u;
      const float label_w = Text(cl, L, L.fonts.regular, kBody, ImVec2(lx, ly), Col(r.enabled ? kText : kFaint, L), r.label.c_str());
      if (r.restart) Tag(cl, L, ImVec2(lx + label_w + 14 * u, a.y + (row_h - L.px(kTag) - 6 * u) * 0.5f), pg.t_restart_required, kAmber);
      // the control
      const float cx = b.x - 18 * u - ctrl_w;
      ImGui::SetCursorScreenPos(ImVec2(cx, a.y + (row_h - frame_h) * 0.5f));
      if (!r.enabled) ImGui::BeginDisabled();
      ImGui::SetNextItemWidth(ctrl_w);
      const ImGuiID ctrl_id = ImGui::GetID("##v");
      bool open_here = false;   // menu3: this row's dropdown is open (its help stays on the right, the row stays lit)   // the control's id (a combo's last item is its popup when open)
      if (r.kind == "bool") {
        bool v = *r.value != 0;
        if (Toggle(L, "##v", &v, ImVec2(ctrl_w, frame_h), pg.t_on, pg.t_off)) *r.value = v ? 1 : 0;
      } else if (r.kind == "choice" && r.choices && !r.choices->empty()) {
        const int n = int(r.choices->size());
        const int cur = std::clamp(int(std::lround(*r.value)), 0, n - 1);
        if (ImGui::BeginCombo("##v", (*r.choices)[size_t(cur)].c_str(), ImGuiComboFlags_HeightLarge)) {
          open_here = true;
          for (int c = 0; c < n; ++c) {
            ImGui::PushID(c);
            // menu3: only the picked (focused) entry is lit; the current value is plain text with a check mark
            if (ImGui::Selectable((*r.choices)[size_t(c)].c_str(), false, 0, ImVec2(0, L.px(kBody) + 12 * u))) *r.value = c;
            if (c == cur) {
              ImGui::SetItemDefaultFocus();
              const ImRect ir = ImGui::GetCurrentContext()->LastItemData.Rect;   // (no new runtime imports)
              const ImVec2 ia = ir.Min, ib = ir.Max;
              const float h = ib.y - ia.y, cs = h * 0.34f, cx = ib.x - h * 0.55f, cy = (ia.y + ib.y) * 0.5f;
              const ImVec2 pts[3] = {ImVec2(cx - cs * 0.55f, cy), ImVec2(cx - cs * 0.15f, cy + cs * 0.45f), ImVec2(cx + cs * 0.6f, cy - cs * 0.5f)};
              ImGui::GetWindowDrawList()->AddPolyline(pts, 3, Col(kText, L), ImDrawFlags_None, std::max(1.5f, 2.2f * u));
            }
            ImGui::PopID();
          }
          ImGui::EndCombo();
        }
      } else if (r.kind == "int") {
        // menu5 (RC4-4, Judgment's Brightness 50..150 step 5): a slider over the option's own steps, as for "float".
        // It was a slider over every integer, snapped to the step afterwards - a pad / key nudge moves it by 1 (65 -> 66),
        // which the snap put straight back (66 -> 65), so a stepped int slider never moved with the pad or keys.
        const int st = std::max(1, int(std::lround(r.step)));
        const int steps = std::max(1, int((std::lround(r.max) - std::lround(r.min)) / st));
        int k = std::clamp(int(std::lround((*r.value - r.min) / st)), 0, steps);
        char fmt[32];
        std::snprintf(fmt, sizeof fmt, "%d", int(std::lround(r.min)) + k * st);
        if (ImGui::SliderInt("##v", &k, 0, steps, fmt)) *r.value = std::round(r.min) + std::clamp(k, 0, steps) * st;
      } else if (r.kind == "float") {
        // in the option's own steps (as the launcher does): an integer slider over the steps, showing the value
        const double st = r.step > 0 ? r.step : 0.1;
        const int steps = std::max(1, int(std::lround((r.max - r.min) / st)));
        int k = std::clamp(int(std::lround((*r.value - r.min) / st)), 0, steps);
        char fmt[32];
        std::snprintf(fmt, sizeof fmt, "%.*f", std::max(0, r.decimals), r.min + k * st);
        if (ImGui::SliderInt("##v", &k, 0, steps, fmt)) *r.value = r.min + std::clamp(k, 0, steps) * st;
      }
      if (pg.focus && !confirming) {   // review 049: as take_focus, by the control's id
        if (want_focus && *pg.focus == int(i)) {
          ImGui::SetFocusID(ctrl_id, ImGui::GetCurrentContext()->CurrentWindow);
          ImGui::SetNavCursorVisible(true);
          ImGui::SetScrollHereY(0.5f);
        } else if (!want_focus && ImGui::GetCurrentContext()->NavId == ctrl_id) {
          *pg.focus = int(i);
        }
      }
      const bool focused = ImGui::GetCurrentContext()->NavId == ctrl_id || open_here;
      if (!r.enabled) ImGui::EndDisabled();
      const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                           ImGui::IsMouseHoveringRect(a, b);
      // the row's background: the selected row lit, with a crimson bar (the focus indicator)
      split.SetCurrentChannel(cl, 0);
      if (focused || hovered) {
        cl->AddRectFilled(a, b, focused ? Col(kSteelHi, 0.85f, L) : Col(kSteel, 0.80f, L), 3 * u);
        if (focused) {
          cl->AddRectFilled(a, ImVec2(a.x + 6 * u, b.y), Col(kRedHi, L), 3 * u, ImDrawFlags_RoundLeft);
          cl->AddRect(a, b, Col(kText, 0.30f, L), 3 * u, 0, std::max(1.f, u));
        }
      } else if (i % 2) {
        cl->AddRectFilled(a, b, Col(ImVec4(1, 1, 1, 0.025f), L));
      }
      split.Merge(cl);
      if (focused || (hovered && !any_focus && tip_label.empty())) {
        tip_label = r.label;
        tip_text = r.tip;
        tip_why = r.enabled ? std::string() : r.why;
        tip_state = r.restart ? 2 : r.live ? 1 : 4;
      }
      any_focus |= focused;
      ImGui::SetCursorScreenPos(a);
      ImGui::Dummy(ImVec2(rw, row_h + 4 * u));
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
  if (confirming) ImGui::EndDisabled();

  // ---- the description of the selected row
  {
    const float px = da.x + 30 * u, pw = db.x - da.x - 60 * u;
    float y = da.y + 30 * u;
    if (!tip_label.empty()) {
      y += TextSize(L, L.fonts.semibold, kBody * 1.08f, tip_label.c_str(), pw).y;
      Text(dl, L, L.fonts.semibold, kBody * 1.08f, ImVec2(px, da.y + 30 * u), Col(kText, L), tip_label.c_str(), pw);
      y += 12 * u;
      const char* st = tip_state == 1 ? pg.t_applies_now : tip_state == 2 ? pg.t_waiting : pg.t_needs_restart;
      const ImVec2 ss = TextSize(L, L.fonts.regular_small, kSmall, st, pw - 26 * u);
      Lamp(dl, L, ImVec2(px + 6 * u, y + L.px(kSmall) * 0.62f), 5 * u, tip_state == 1 ? 1 : tip_state == 2 ? 2 : 0);
      Text(dl, L, L.fonts.regular_small, kSmall, ImVec2(px + 24 * u, y), Col(tip_state == 2 ? kAmber : kDim, L), st, pw - 26 * u);
      y += ss.y + 16 * u;
      dl->AddLine(ImVec2(px, y), ImVec2(px + pw, y), Col(kEdge, 0.30f, L), std::max(1.f, u));
      y += 16 * u;
      if (!tip_why.empty()) {
        y += 6 * u + Text(dl, L, L.fonts.regular_small, kSmall, ImVec2(px, y), Col(kAmber, L), tip_why.c_str(), pw) * 0.f +
             TextSize(L, L.fonts.regular_small, kSmall, tip_why.c_str(), pw).y;
      }
      dl->PushClipRect(ImVec2(da.x, da.y), ImVec2(db.x, db.y - status_h - 14 * u), true);
      Text(dl, L, L.fonts.regular, kBody * 0.88f, ImVec2(px, y), Col(kDim, L), tip_text.c_str(), pw);
      dl->PopClipRect();
    }
  }
  // ---- the status line and the buttons
  if (!pg.status.empty()) {
    const ImVec2 sa(da.x + 10 * u, db.y - status_h - 4 * u), sb(db.x - 10 * u, db.y - 10 * u);
    const ImVec4 k = pg.restart_changes ? kAmber : kDim;
    dl->AddRectFilled(sa, sb, Col(ImVec4(0.045f, 0.048f, 0.055f, 1.f), L), 3 * u);   // opaque: no game behind the note
    if (pg.restart_changes) dl->AddRectFilled(sa, ImVec2(sa.x + 4 * u, sb.y), Col(kAmber, L), 3 * u, ImDrawFlags_RoundLeft);
    dl->AddRect(sa, sb, Col(k, 0.35f, L), 3 * u, 0, std::max(1.f, u));
    Text(dl, L, L.fonts.regular_small, kSmall, ImVec2(da.x + 30 * u, sa.y + 14 * u), Col(k, L), pg.status.c_str(), status_w);
  }
  float by = buttons_top;
  if (!pg.focus && pg.focus_resume) ImGui::SetKeyboardFocusHere();
  if (confirming) ImGui::BeginDisabled();
  for (size_t k = 0; k < btns.size(); ++k) {
    ImGui::SetCursorScreenPos(ImVec2(ra.x, by));
    if (Button(L, btns[k].label.c_str(), ImVec2(right_w, bh), btns[k].red, btns[k].enabled)) act = btns[k].a;
    take_focus(n_rows + int(k));
    by += bh + bgap;
  }
  if (confirming) ImGui::EndDisabled();
  // review 049: Quit game asks first (with the pad, one D-pad down + A quit at once before)
  if (act == Action::kQuit && pg.confirm_quit && !confirming) {
    *pg.confirm_quit = true;
    if (pg.confirm_sel) *pg.confirm_sel = 0;   // 0.5.0 (Shagg): Yes - Quit game was already chosen; B / Esc cancel
    act = Action::kNone;
  }
  if (confirming) {
    int& sel = *pg.confirm_sel;
    if (pg.nav_move || pg.nav_adjust) sel = (pg.nav_move ? pg.nav_move : pg.nav_adjust) < 0 ? 0 : 1;
    // menu2: the dim is the first thing on the BOX window's own draw list (unclipped, the whole display), so it lies
    // over the menu and its rows child but under the box's plate, text and buttons. (It was on the foreground list,
    // which is drawn over every window - the box too.)
    const float bw = std::min(760 * u, L.display.x - 80 * u), pad = 34 * u;
    const ImVec2 ts = TextSize(L, L.fonts.regular, kBody, pg.t_quit_confirm, bw - 2 * pad);
    const float bxh = pad + ts.y + 28 * u + bh + pad;
    const ImVec2 ba((L.display.x - bw) * 0.5f, (L.display.y - bxh) * 0.5f), bb(ba.x + bw, ba.y + bxh);
    ImGui::SetNextWindowPos(ba);
    ImGui::SetNextWindowSize(ImVec2(bw, bxh));
    static int s_confirm_frame = -1;   // focus the box once when it opens (each frame would re-run Dear ImGui's nav init)
    if (s_confirm_frame != ImGui::GetFrameCount() - 1) ImGui::SetNextWindowFocus();
    s_confirm_frame = ImGui::GetFrameCount();
    ImGui::Begin("##cogbox_quit_confirm", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
    ImDrawList* wl = ImGui::GetWindowDrawList();
    wl->PushClipRect(ImVec2(0, 0), L.display, false);
    wl->AddRectFilled(ImVec2(0, 0), L.display, Col(ImVec4(0, 0, 0, 0.55f), L));
    wl->PopClipRect();
    Plate(wl, ba, bb, 16 * u, L);
    Text(wl, L, L.fonts.regular, kBody, ImVec2(ba.x + pad, ba.y + pad), Col(kText, L), pg.t_quit_confirm, bw - 2 * pad);
    const float yb = ba.y + pad + ts.y + 28 * u, half = (bw - 2 * pad - 20 * u) * 0.5f;
    const char* labels[2] = {pg.t_yes, pg.t_no};
    for (int k = 0; k < 2; ++k) {
      ImGui::SetCursorScreenPos(ImVec2(ba.x + pad + k * (half + 20 * u), yb));
      const bool hit = Button(L, labels[k], ImVec2(half, bh), k == 0, true);
      if (sel == k && ImGui::GetCurrentContext()->NavId != ImGui::GetItemID()) {
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentContext()->CurrentWindow);
        ImGui::SetNavCursorVisible(true);
      }
      else if (ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride)) sel = k;   // the mouse only (not the nav cursor)
      if (hit) {
        *pg.confirm_quit = false;
        if (k == 0) act = Action::kQuit;
        else if (pg.focus) { *pg.focus = n_all - 1; s_refocus = true; }   // back on Quit game
      }
    }
    ImGui::End();
  }
  Legend(dl, L, pg.legend);
  return act;
}

Rect BeginAchievements(const Look& L, const char* game, const char* heading, const char* right) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float u = L.u, mx = 56 * u;
  Backdrop(dl, L);
  const float rule = Header(dl, L, game, right);
  const float x0 = L.c0.x + mx, x1 = L.c1.x - mx;
  Heading(dl, L, ImVec2(x0, rule + 22 * u), heading, x1);
  const float top = rule + 22 * u + L.px(kBody) + 26 * u, bot = L.display.y - 72 * u - 18 * u - 58 * u - 22 * u;
  Plate(dl, ImVec2(x0, top), ImVec2(x1, bot), 16 * u, L);
  return Rect{ImVec2(x0 + 28 * u, top + 22 * u), ImVec2(x1 - 28 * u, bot - 18 * u)};
}

Action EndAchievements(const Look& L, const char* t_back, const char* t_resume, const std::vector<LegendItem>& legend) {
  Action act = Action::kNone;
  const float u = L.u, mx = 56 * u, x1 = L.c1.x - mx, bh = 58 * u, bw = 340 * u;
  const float y = L.display.y - 72 * u - 18 * u - bh;
  ImGui::SetCursorScreenPos(ImVec2(x1 - 2 * bw - 14 * u, y));
  if (Button(L, t_back, ImVec2(bw, bh), false)) act = Action::kBack;
  ImGui::SetCursorScreenPos(ImVec2(x1 - bw, y));
  if (Button(L, t_resume, ImVec2(bw, bh), true)) act = Action::kResume;
  Legend(ImGui::GetWindowDrawList(), L, legend);
  return act;
}

}  // namespace xenos::menu
