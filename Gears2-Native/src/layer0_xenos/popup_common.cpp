// Layer 0 (game-free): the achievement popups' shared shapes - see popup_styles.h. Our code (MIT).
#include "popup_styles.h"

#include <xenos/achievement_popup_assets.h>

#include <algorithm>
#include <cmath>

namespace xenos::popup {

ImU32 Col(uint8_t r, uint8_t g, uint8_t b, float alpha) {
  const int a = static_cast<int>(std::clamp(alpha, 0.f, 1.f) * 255.f + 0.5f);
  return IM_COL32(r, g, b, a);
}

ImU32 Col(const ach::Rgb& c, float alpha, float shade) {
  auto s = [shade](uint8_t v) {
    const float x = shade <= 1.f ? v * shade : v + (255 - v) * (shade - 1.f);
    return static_cast<uint8_t>(std::clamp(x, 0.f, 255.f));
  };
  return Col(s(c.r), s(c.g), s(c.b), alpha);
}

namespace {
ImFont* FontFor(bool semibold) {
  ImFont* f = XenosAchievementFont(semibold);
  return f ? f : ImGui::GetFont();
}
}  // namespace

float TextWidth(bool semibold, float size, const std::string& text) {
  return FontFor(semibold)->CalcTextSizeA(size, FLT_MAX, 0.f, text.c_str()).x;
}

void Text(ImDrawList* dl, bool semibold, float size, ImVec2 pos, ImU32 col, const std::string& text, float max_width) {
  if (text.empty() || ((col >> IM_COL32_A_SHIFT) & 0xFF) == 0) return;
  float scale = 1.f;
  const std::string fitted =
      ach::FitText(text, max_width, [&](const std::string& s) { return TextWidth(semibold, size, s); }, &scale);
  const float sz = size * scale;
  // Keep the line's vertical centre where it was at full size.
  dl->AddText(FontFor(semibold), sz, ImVec2(pos.x, pos.y + (size - sz) * 0.5f), col, fitted.c_str());
}

void Trophy(ImDrawList* dl, ImVec2 c, float h, ImU32 col, float sx) {
  const float u = h / 34.f;  // drawn on a 34-unit box, as the approved mockup's cup
  auto P = [&](float x, float y) { return ImVec2(c.x + (x - 17.f) * u * sx, c.y + (y - 17.f) * u); };
  // Bowl: a rounded cup from y 5 to 19.
  dl->PathLineTo(P(9, 5));
  dl->PathLineTo(P(25, 5));
  dl->PathLineTo(P(25, 11));
  dl->PathBezierCubicCurveTo(P(25, 21), P(9, 21), P(9, 11));
  dl->PathFillConvex(col);
  // Handles.
  dl->AddBezierCubic(P(9, 7), P(3, 7), P(3, 14), P(10, 14), col, 1.8f * u);
  dl->AddBezierCubic(P(25, 7), P(31, 7), P(31, 14), P(24, 14), col, 1.8f * u);
  // Stem and base.
  dl->AddRectFilled(P(15.5f, 18), P(18.5f, 25), col);
  dl->AddRectFilled(P(11, 25), P(23, 29), col, 1.f * u);
}

void Diamond(ImDrawList* dl, ImVec2 c, float h, ImU32 col, float sx) {
  const float u = h / 34.f;
  auto P = [&](float x, float y) { return ImVec2(c.x + (x - 17.f) * u * sx, c.y + (y - 17.f) * u); };
  dl->PathLineTo(P(7, 12));
  dl->PathLineTo(P(12, 6));
  dl->PathLineTo(P(22, 6));
  dl->PathLineTo(P(27, 12));
  dl->PathLineTo(P(17, 26));
  dl->PathFillConvex(col);
  const ImU32 facet = (col & ~IM_COL32_A_MASK) | (((((col >> IM_COL32_A_SHIFT) & 0xFF) * 3) / 5) << IM_COL32_A_SHIFT);
  dl->AddLine(P(7, 12), P(27, 12), IM_COL32(120, 120, 120, (col >> IM_COL32_A_SHIFT) & 0xFF), 1.f * u);
  dl->AddLine(P(12, 6), P(17, 12), facet, 1.f * u);
  dl->AddLine(P(22, 6), P(17, 12), facet, 1.f * u);
  // Laurels: two arcs under the gem.
  dl->PathArcTo(P(17, 17), 13.f * u, IM_PI * 0.62f, IM_PI * 0.95f, 8);
  dl->PathStroke(col, 0, 1.6f * u);
  dl->PathArcTo(P(17, 17), 13.f * u, IM_PI * 0.05f, IM_PI * 0.38f, 8);
  dl->PathStroke(col, 0, 1.6f * u);
}

void RoundIcon(ImDrawList* dl, ImTextureID icon, ImVec2 c, float r, float alpha, float sx) {
  if (!icon || r <= 0.f || alpha <= 0.f) return;
  const float rx = r * sx;
  dl->AddImageRounded(icon, ImVec2(c.x - rx, c.y - r), ImVec2(c.x + rx, c.y + r), ImVec2(0, 0), ImVec2(1, 1),
                      Col(255, 255, 255, alpha), std::min(rx, r));
}

void GCoin(ImDrawList* dl, ImVec2 c, float r, float alpha) {
  dl->AddCircleFilled(c, r, Col(255, 255, 255, alpha), 24);
  const float size = r * 1.35f;
  const float w = TextWidth(true, size, "G");
  dl->AddText(XenosAchievementFont(true) ? XenosAchievementFont(true) : ImGui::GetFont(), size,
              ImVec2(c.x - w * 0.5f, c.y - size * 0.55f), Col(0x22, 0x22, 0x22, alpha), "G");
}

ImU32 GradeCol(ach::TrophyGrade g, float alpha) {
  switch (g) {
    case ach::TrophyGrade::Gold: return Col(0xE6, 0xC3, 0x4A, alpha);
    case ach::TrophyGrade::Silver: return Col(0xC4, 0xCA, 0xD0, alpha);
    case ach::TrophyGrade::Platinum: return Col(0xB8, 0xC7, 0xE0, alpha);
    default: return Col(0xC0, 0x7A, 0x45, alpha);
  }
}

}  // namespace xenos::popup
