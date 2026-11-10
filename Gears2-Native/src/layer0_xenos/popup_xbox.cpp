// Layer 0 (game-free): the modern Xbox style popup - a pill in the chosen colour with a round badge; the badge grows,
// fills, shows the achievement's icon and flips to a trophy (diamond when rare); the pill opens from the centre;
// title page, then the description (approved animation v4). Our code (MIT).
#include "popup_styles.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace xenos::popup {

namespace {
constexpr float kW = 470.f, kH = 70.f, kBadge = 32.f;
}

ImVec2 SizeXbox(float s) { return ImVec2(kW * s, kH * s); }

void DrawXbox(ImDrawList* dl, const PopupDraw& d) {
  const float s = d.scale;
  const ach::PopupFrame& f = d.f;
  if (f.alpha <= 0.f) return;
  const float a = f.alpha;
  const float cx = d.pos.x + kW * 0.5f * s;
  const float pill_w = kH * s + f.bar * (kW - kH) * s;
  const ImVec2 p0(cx - pill_w * 0.5f, d.pos.y), p1(cx + pill_w * 0.5f, d.pos.y + kH * s);

  // The pill (only once it starts opening; before that only the badge shows).
  if (f.bar > 0.f) {
    dl->AddRectFilled(p0, p1, Col(d.colour.fill, a), kH * 0.5f * s);
    dl->AddRect(p0, p1, Col(d.colour.border, a), kH * 0.5f * s, 0, 1.f * s);
  }
  // The text, clipped to the pill: page 1 (title + G coin, GP, name), cross-fading to page 2 (the description).
  if (f.text_alpha > 0.f) {
    dl->PushClipRect(p0, p1, true);
    const float tx = p0.x + 80.f * s, max_w = (kW - 80.f - 24.f) * s;
    const float a1 = a * f.text_alpha * (1.f - f.page), a2 = a * f.text_alpha * f.page;
    if (a1 > 0.f) {
      Text(dl, true, 17.f * s, ImVec2(tx, d.pos.y + 11.f * s), Col(255, 255, 255, a1), d.title, max_w);
      GCoin(dl, ImVec2(tx + 9.5f * s, d.pos.y + 46.f * s), 9.5f * s, a1);
      Text(dl, false, 16.f * s, ImVec2(tx + 26.f * s, d.pos.y + 36.f * s), Col(255, 255, 255, a1),
           std::to_string(d.gamerscore) + " - " + d.name, max_w - 26.f * s);
    }
    if (a2 > 0.f) Text(dl, false, 16.f * s, ImVec2(tx, d.pos.y + 25.f * s), Col(255, 255, 255, a2), d.description, max_w);
    dl->PopClipRect();
  }

  // The badge: a hollow ring that grows and fills, then the icon / trophy coin flip.
  const ImVec2 cb(p0.x + kH * 0.5f * s, d.pos.y + kH * 0.5f * s);
  const float R = kBadge * s * f.icon_scale;
  if (R <= 0.f) return;
  if (f.ring < 1.f) {
    const float th = 4.f * s + (R - 4.f * s) * f.ring;
    dl->AddCircle(cb, R - th * 0.5f, Col(d.colour.fill, a, 0.75f), 40, th);
  } else {
    dl->AddCircleFilled(cb, R, Col(d.colour.fill, a, 0.72f), 40);
    dl->AddCircle(cb, R - 1.f * s, Col(d.colour.fill, a, 1.45f), 40, 2.f * s);
  }
  const float sq = std::fabs(std::cos(IM_PI * f.swap));  // the coin flip squeezes it to nothing and back
  const float g = f.glyph_scale * f.icon_scale;
  if (g <= 0.f) return;
  if (d.icon && f.swap < 0.5f) {
    RoundIcon(dl, d.icon, cb, 24.f * s * g, a, sq);
  } else if (d.rare) {
    Diamond(dl, cb, 34.f * s * g, Col(255, 255, 255, a), sq);
  } else {
    Trophy(dl, cb, 34.f * s * g, Col(0xEE, 0xEE, 0xEE, a), sq);
  }
}

}  // namespace xenos::popup
