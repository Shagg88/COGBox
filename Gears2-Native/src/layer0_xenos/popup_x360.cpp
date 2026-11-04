// Layer 0 (game-free): the Xbox 360 style popup - our own drawing of the bar and the ringed orb (approved mockup
// popup-x360-v2 / animation v4). Our code (MIT).
#include "popup_styles.h"

#include <imgui_internal.h>  // ShadeVertsLinearColorGradientKeepAlpha

#include <algorithm>
#include <string>

namespace xenos::popup {

namespace {
constexpr float kW = 500.f, kOrb = 93.f, kBarTop = 4.f, kBarH = 85.f;

// One lens of the X (the 0-100 orb box), mirrored for the other.
void XLens(ImDrawList* dl, ImVec2 c, float u, bool mirror, float alpha) {
  auto P = [&](float x, float y) { return ImVec2(c.x + ((mirror ? 100.f - x : x) - 50.f) * u, c.y + (y - 50.f) * u); };
  const int v0 = dl->VtxBuffer.Size;
  dl->PathLineTo(P(30, 30));
  dl->PathBezierQuadraticCurveTo(P(55, 40), P(71, 70), 12);
  dl->PathBezierQuadraticCurveTo(P(45, 59), P(30, 30), 12);
  dl->PathFillConvex(Col(0x5D, 0xBB, 0x1F, alpha));
  ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, P(30, 30), P(71, 70),
                                                Col(0xB7, 0xF0, 0x5A, alpha), Col(0x2F, 0x7D, 0x0D, alpha));
}
}  // namespace

ImVec2 SizeX360(float s) { return ImVec2(kW * s, kOrb * s); }

void DrawX360(ImDrawList* dl, const PopupDraw& d) {
  const float s = d.scale;
  const ach::PopupFrame& f = d.f;
  const ImVec2 o = d.pos;

  // The bar, sliding out from behind the orb.
  const float bar_w = kBarH * s + f.bar * (kW - kBarH) * s;
  const ImVec2 b0(o.x, o.y + kBarTop * s), b1(o.x + bar_w, o.y + (kBarTop + kBarH) * s);
  if (f.alpha > 0.f && f.bar > 0.f) {
    dl->AddRectFilled(b0, b1, Col(0x4B, 0x4B, 0x4B, f.alpha), kBarH * 0.5f * s);
    dl->AddRect(b0, b1, Col(0xFF, 0xFF, 0xFF, 0.19f * f.alpha), kBarH * 0.5f * s, 0, 2.f * s);  // the faint rim
  }
  // The text, clipped to the bar while it opens.
  if (f.text_alpha > 0.f) {
    dl->PushClipRect(b0, b1, true);
    const float tx = o.x + (104.f + f.slide * 12.f) * s, max_w = (kW - 104.f - 30.f) * s;
    Text(dl, false, 27.f * s, ImVec2(tx, o.y + 12.f * s), Col(255, 255, 255, f.text_alpha * f.alpha), d.title, max_w);
    Text(dl, false, 21.f * s, ImVec2(tx, o.y + 46.f * s), Col(255, 255, 255, f.text_alpha * f.alpha),
         std::to_string(d.gamerscore) + "G - " + d.name, max_w);
    dl->PopClipRect();
  }

  // The orb: 0-100 box units, shrinking with icon_scale in the outro.
  const ImVec2 c(o.x + kOrb * 0.5f * s, o.y + kOrb * 0.5f * s);
  const float u = kOrb / 100.f * s * f.icon_scale;
  if (f.glow > 0.f) dl->AddCircle(c, 38.f * u, Col(255, 255, 255, 0.8f * f.glow), 48, 5.f * u);
  if (u <= 0.f || f.alpha <= 0.f) return;
  const float a = f.alpha;
  if (f.ring > 0.f) {
    const float ra = a * f.ring;
    dl->AddCircleFilled(c, 46.f * u, Col(0x2A, 0x2A, 0x2A, ra), 48);
    if (f.light > 0.f) {  // the lit player-1 quarter, top left
      dl->PathLineTo(c);
      dl->PathArcTo(c, 44.f * u, IM_PI, IM_PI * 1.5f, 16);
      dl->PathFillConvex(Col(0x6E, 0xA8, 0x21, ra * f.light));
    }
    dl->AddRectFilled(ImVec2(c.x - 4.f * u, c.y - 48.f * u), ImVec2(c.x + 4.f * u, c.y + 48.f * u), Col(0x1B, 0x1B, 0x1B, ra));
    dl->AddRectFilled(ImVec2(c.x - 48.f * u, c.y - 4.f * u), ImVec2(c.x + 48.f * u, c.y + 4.f * u), Col(0x1B, 0x1B, 0x1B, ra));
    dl->AddCircleFilled(c, 33.f * u, Col(0x1B, 0x1B, 0x1B, ra), 48);
  }
  // The glossy sphere: it fills the whole orb until it shrinks to uncover the ring (cover 1 -> 0).
  const float rs = (30.f + 16.f * f.cover) * u;
  for (int i = 0; i <= 11; ++i) {  // a radial gradient from concentric discs, lit from the top left
    const float t = i / 11.f, k = t * t;
    const ImVec2 cc(c.x - 0.22f * rs * t, c.y - 0.28f * rs * t);
    const auto v = static_cast<uint8_t>(0x4E + (0xFF - 0x4E) * k);
    dl->AddCircleFilled(cc, rs * (1.f - 0.82f * t), Col(v, v, v, a), 40);
  }
  const float to_icon = d.icon ? std::clamp(f.swap * 2.f, 0.f, 1.f) : 0.f;   // the X dims...
  const float icon_a = d.icon ? std::clamp((f.swap - 0.5f) * 2.f, 0.f, 1.f) : 0.f;  // ...then the icon fades in
  const float xu = rs / 30.f;  // the X follows the sphere's size
  XLens(dl, c, xu, false, a * (1.f - to_icon));
  XLens(dl, c, xu, true, a * (1.f - to_icon));
  if (to_icon > 0.f) dl->AddCircleFilled(c, rs, Col(0x15, 0x15, 0x15, a * to_icon), 40);
  RoundIcon(dl, d.icon, c, rs * 0.8f, a * icon_a);
  dl->AddEllipseFilled(ImVec2(c.x - 7.f * xu, c.y - 16.f * xu), ImVec2(15.f * xu, 7.f * xu),
                       Col(255, 255, 255, 0.3f * a * (1.f - 0.6f * icon_a)), 0.f, 24);
}

}  // namespace xenos::popup
