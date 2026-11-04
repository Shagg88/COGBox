// Layer 0 (game-free): the achievement popups' rules - see xenos/achievements_popup_logic.h. Standard C++17 only.
// Our code (MIT).
#include <xenos/achievements_popup_logic.h>

#include <cctype>

namespace xenos::ach {

TrophyGrade GradeFor(uint32_t title_id, uint32_t achievement_id, uint32_t gamerscore) {
  // Gears of War: Reloaded's grades for Gears 1 achievements that GP alone would make Bronze.
  if (title_id == 0x4D5307D5 && (achievement_id == 13 || achievement_id == 19)) return TrophyGrade::Gold;
  if (gamerscore >= 75) return TrophyGrade::Gold;
  if (gamerscore >= 31) return TrophyGrade::Silver;
  return TrophyGrade::Bronze;
}

bool IsRare(const Rarity& r) { return r.known && r.percent < 10.f; }

bool CompletesGame(const std::vector<std::pair<uint32_t, bool>>& states, uint32_t just_unlocked) {
  bool found = false;
  for (const auto& [id, unlocked] : states) {
    if (!unlocked) return false;
    if (id == just_unlocked) found = true;
  }
  return found;
}

const std::array<ColourPreset, 10> kColourPresets = {{
    {"Dark", {0x24, 0x26, 0x2B}, {0x3C, 0x3F, 0x46}},
    {"Red", {0x9E, 0x1B, 0x22}, {0x9E, 0x1B, 0x22}},
    {"Green", {0x10, 0x7C, 0x10}, {0x10, 0x7C, 0x10}},
    {"Blue", {0x0E, 0x5A, 0xA7}, {0x0E, 0x5A, 0xA7}},
    {"Purple", {0x6B, 0x2F, 0xA3}, {0x6B, 0x2F, 0xA3}},
    {"Orange", {0xC2, 0x5A, 0x0C}, {0xC2, 0x5A, 0x0C}},
    {"Pink", {0xB4, 0x24, 0x6E}, {0xB4, 0x24, 0x6E}},
    {"Teal", {0x0E, 0x7C, 0x7B}, {0x0E, 0x7C, 0x7B}},
    {"Grey", {0x4B, 0x4F, 0x56}, {0x4B, 0x4F, 0x56}},
    {"Gold", {0xA0, 0x7A, 0x12}, {0xA0, 0x7A, 0x12}},
}};

namespace {
std::string Lower(const std::string& s) {
  std::string out = s;
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}
}  // namespace

PopupStyle ParseStyle(const std::string& s) {
  const std::string l = Lower(s);
  if (l == "xbox") return PopupStyle::Xbox;
  if (l == "ps5") return PopupStyle::Ps5;
  return PopupStyle::X360;
}

const ColourPreset& ParseColour(const std::string& s) {
  const std::string l = Lower(s);
  for (const ColourPreset& p : kColourPresets) {
    if (Lower(p.name) == l) return p;
  }
  return kColourPresets[0];
}

PopupPosition ParsePosition(const std::string& s) {
  static const std::pair<const char*, PopupPosition> names[] = {
      {"top_left", PopupPosition::TopLeft},       {"top_centre", PopupPosition::TopCentre},
      {"top_right", PopupPosition::TopRight},     {"bottom_left", PopupPosition::BottomLeft},
      {"bottom_centre", PopupPosition::BottomCentre}, {"bottom_right", PopupPosition::BottomRight}};
  const std::string l = Lower(s);
  for (const auto& [name, pos] : names) {
    if (l == name) return pos;
  }
  return PopupPosition::Default;
}

PopupPosition ResolvePosition(PopupStyle style, PopupPosition p) {
  if (p != PopupPosition::Default) return p;
  return style == PopupStyle::Ps5 ? PopupPosition::TopRight : PopupPosition::BottomCentre;
}

std::string FitText(const std::string& text, float max_width, const std::function<float(const std::string&)>& measure,
                    float* scale_out) {
  constexpr float kSmaller = 0.9f;
  static const std::string kEllipsis = "\xE2\x80\xA6";
  if (measure(text) <= max_width) {
    *scale_out = 1.f;
    return text;
  }
  *scale_out = kSmaller;
  if (measure(text) * kSmaller <= max_width) return text;
  std::string kept = text;
  while (!kept.empty()) {
    // Drop one whole UTF-8 character from the end.
    size_t cut = kept.size() - 1;
    while (cut > 0 && (static_cast<unsigned char>(kept[cut]) & 0xC0) == 0x80) --cut;
    kept.erase(cut);
    if (measure(kept + kEllipsis) * kSmaller <= max_width) break;
  }
  return kept + kEllipsis;
}

namespace {
// 0 before a, 1 after b, linear between.
float Ramp(float t, float a, float b) {
  if (t <= a) return 0.f;
  if (t >= b) return 1.f;
  return (t - a) / (b - a);
}
float EaseOut(float x) { return x >= 1.f ? 1.f : 1.f - (1.f - x) * (1.f - x) * (1.f - x); }
float EaseIn(float x) { return x * x * x; }
// a -> b, exactly b at x = 1.
float Lerp(float a, float b, float x) { return x >= 1.f ? b : a + (b - a) * x; }
}  // namespace

PopupFrame PopupTimeline(PopupStyle style, float t) {
  PopupFrame f;
  if (t >= kPopupSeconds) {
    f.finished = true;
    return f;
  }
  if (t < 0.f) t = 0.f;
  switch (style) {
    case PopupStyle::X360:
      f.alpha = Ramp(t, 0.f, 0.15f) * (1.f - Ramp(t, 4.8f, 5.0f));
      f.icon_scale = EaseOut(Ramp(t, 0.f, 0.15f));
      f.bar = EaseOut(Ramp(t, 0.15f, 0.5f)) * (1.f - EaseIn(Ramp(t, 4.5f, 4.85f)));
      f.text_alpha = Ramp(t, 0.4f, 0.6f) * (1.f - Ramp(t, 4.5f, 4.65f));
      f.slide = 0.f;
      break;
    case PopupStyle::Xbox:
      f.alpha = Ramp(t, 0.f, 0.1f) * (1.f - Ramp(t, 4.75f, 5.0f));
      f.icon_scale = t < 0.18f ? 1.12f * EaseOut(Ramp(t, 0.f, 0.18f)) : Lerp(1.12f, 1.f, EaseOut(Ramp(t, 0.18f, 0.28f)));
      f.bar = EaseOut(Ramp(t, 0.2f, 0.5f)) * (1.f - EaseIn(Ramp(t, 4.5f, 4.8f)));
      f.text_alpha = Ramp(t, 0.4f, 0.65f) * (1.f - Ramp(t, 4.5f, 4.6f));
      f.shine = (t >= 0.7f && t <= 1.2f) ? Ramp(t, 0.7f, 1.2f) : -1.f;
      f.slide = 0.f;
      break;
    case PopupStyle::Ps5: {
      const float in = 1.f - EaseOut(Ramp(t, 0.f, 0.4f)), out = EaseIn(Ramp(t, 4.6f, 5.0f));
      f.slide = in > out ? in : out;
      f.alpha = Ramp(t, 0.f, 0.2f) * (1.f - Ramp(t, 4.6f, 5.0f));
      f.text_alpha = f.alpha;
      f.icon_scale = 1.f;
      f.bar = 1.f;
      break;
    }
  }
  return f;
}

}  // namespace xenos::ach
