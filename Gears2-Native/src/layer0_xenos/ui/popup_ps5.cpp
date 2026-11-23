// Layer 0 (game-free): the PS5 trophy style popup - a dark box with the achievement's icon, a grade cup and the name,
// fading in with a short slide from the right (approved animation v4). Our code (MIT).
#include "popup_styles.h"

namespace xenos::popup {

namespace {
constexpr float kW = 640.f, kH = 108.f, kIcon = 66.f;

// The small grade cup next to the name.
void Cup(ImDrawList* dl, ImVec2 c, float h, ImU32 col) {
  const float u = h / 18.f;
  auto P = [&](float x, float y) { return ImVec2(c.x + (x - 8.f) * u, c.y + (y - 9.f) * u); };
  dl->PathLineTo(P(3, 1));
  dl->PathLineTo(P(13, 1));
  dl->PathLineTo(P(13, 5));
  dl->PathBezierCubicCurveTo(P(13, 11), P(3, 11), P(3, 5));
  dl->PathFillConvex(col);
  dl->AddBezierCubic(P(3, 2), P(0, 2), P(0, 6), P(4, 7), col, 1.2f * u);
  dl->AddBezierCubic(P(13, 2), P(16, 2), P(16, 6), P(12, 7), col, 1.2f * u);
  dl->AddRectFilled(P(7, 10), P(9, 14), col);
  dl->AddRectFilled(P(5, 15), P(11, 17), col);
}
}  // namespace

ImVec2 SizePs5(float s) { return ImVec2(kW * s, kH * s); }

void DrawPs5(ImDrawList* dl, const PopupDraw& d) {
  const float s = d.scale;
  const ach::PopupFrame& f = d.f;
  if (f.alpha <= 0.f) return;
  const float a = f.alpha;
  const ImVec2 o(d.pos.x + f.slide * ach::kPs5SlidePixels * s, d.pos.y);
  dl->AddRectFilled(o, ImVec2(o.x + kW * s, o.y + kH * s), Col(0x1F, 0x20, 0x23, 0.93f * a), 12.f * s);
  const ImVec2 i0(o.x + 20.f * s, o.y + (kH - kIcon) * 0.5f * s), i1(i0.x + kIcon * s, i0.y + kIcon * s);
  if (d.icon) {
    dl->AddImageRounded(d.icon, i0, i1, ImVec2(0, 0), ImVec2(1, 1), Col(255, 255, 255, a), 5.f * s);
  } else {
    dl->AddRectFilled(i0, i1, Col(0x2A, 0x2B, 0x2F, a), 5.f * s);
  }
  const float tx = o.x + 106.f * s, max_w = (kW - 106.f - 22.f) * s;
  const float cup_w = 30.f * s;
  Cup(dl, ImVec2(tx + 11.f * s, o.y + 38.f * s), 25.f * s, GradeCol(d.grade, a * f.text_alpha));
  Text(dl, false, 27.f * s, ImVec2(tx + cup_w, o.y + 22.f * s), Col(0xF2, 0xF2, 0xF2, a * f.text_alpha), d.title,
       max_w - cup_w);
  Text(dl, false, 23.f * s, ImVec2(tx, o.y + 59.f * s), Col(0xCF, 0xCF, 0xCF, a * f.text_alpha), d.sub, max_w);   // part 4: bigger, lighter
}

}  // namespace xenos::popup
