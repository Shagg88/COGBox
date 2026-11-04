// Layer 0 (game-free): the achievement popups' rules - see xenos/achievements_popup_logic.h. Standard C++17 only.
// Our code (MIT).
#include <xenos/achievements_popup_logic.h>

#include <initializer_list>

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
float EaseOutQuad(float x) { return x >= 1.f ? 1.f : 1.f - (1.f - x) * (1.f - x); }
// a -> b, exactly b at x = 1.
float Lerp(float a, float b, float x) { return x >= 1.f ? b : a + (b - a) * x; }
}  // namespace

float PopupDuration(PopupStyle style) {
  switch (style) {
    case PopupStyle::Xbox: return 7.3f;
    case PopupStyle::Ps5: return 5.7f;
    default: return 6.1f;
  }
}

PopupFrame PopupTimeline(PopupStyle style, float t) {
  PopupFrame f;
  if (t >= PopupDuration(style)) {
    f.finished = true;
    return f;
  }
  if (t < 0.f) t = 0.f;
  switch (style) {
    case PopupStyle::X360:
      // Measured at 60 fps: a white ghost of the orb flashes and the orb fades in at full size; the bar slides out from
      // behind it (no overshoot); the text comes with a small slide from the right; the sphere shrinks to uncover the
      // ring, then its green quarter switches on; the X turns into the achievement icon. Out: the icon back to the X, the text, the ring, the
      // orb shrinks away, then the bar fades while pulling in a little.
      f.glow = Ramp(t, 0.f, 0.06f) * (1.f - Ramp(t, 0.06f, 0.16f));
      f.alpha = Ramp(t, 0.03f, 0.15f) * (1.f - Ramp(t, 5.85f, 6.1f));
      f.icon_scale = 1.f - Ramp(t, 5.68f, 5.83f);
      f.bar = t < 3.f ? EaseOutQuad(Ramp(t, 0.12f, 0.36f)) : 1.f - 0.4f * Ramp(t, 5.8f, 6.1f);
      f.text_alpha = Ramp(t, 0.32f, 0.36f) * (1.f - Ramp(t, 5.6f, 5.66f));
      f.slide = t < 3.f ? 1.f - Ramp(t, 0.32f, 0.38f) : Ramp(t, 5.6f, 5.66f);
      // The ring is uncovered, not faded in: the sphere first fills the whole orb, then shrinks to its normal size
      // (about 4 frames) with the ring behind it at full strength; the green quarter switches on two frames later.
      f.cover = t < 0.3f ? 1.f : 1.f - EaseOutQuad(Ramp(t, 0.3f, 0.37f));
      f.ring = Ramp(t, 0.29f, 0.3f) * (1.f - Ramp(t, 5.64f, 5.68f));
      f.light = Ramp(t, 0.38f, 0.40f) * (1.f - Ramp(t, 5.64f, 5.68f));
      f.swap = Ramp(t, 0.7f, 0.9f) * (1.f - Ramp(t, 5.2f, 5.5f));
      f.glyph_scale = 1.f;
      break;
    case PopupStyle::Xbox:
      // A small ring grows, fills, the achievement icon grows from a dot, the badge flips to the trophy while the
      // pill opens from the centre; title page, then the description page. Out: the same, backwards.
      f.alpha = Ramp(t, 0.f, 0.1f) * (1.f - Ramp(t, 7.1f, 7.3f));
      f.icon_scale = Lerp(0.3f, 1.f, EaseOut(Ramp(t, 0.f, 0.35f))) * (1.f - 0.7f * EaseIn(Ramp(t, 7.0f, 7.3f)));
      f.ring = Ramp(t, 0.35f, 0.45f) * (1.f - Ramp(t, 6.85f, 7.0f));
      f.glyph_scale = EaseOut(Ramp(t, 0.5f, 0.75f)) * (1.f - Ramp(t, 6.7f, 6.85f));
      f.swap = Ramp(t, 1.2f, 1.4f) * (1.f - Ramp(t, 6.5f, 6.7f));
      f.bar = EaseOut(Ramp(t, 1.2f, 1.55f)) * (1.f - EaseIn(Ramp(t, 6.25f, 6.5f)));
      f.text_alpha = Ramp(t, 1.55f, 1.75f) * (1.f - Ramp(t, 6.1f, 6.25f));
      f.page = Ramp(t, 4.2f, 4.4f);
      f.slide = 0.f;
      f.light = 1.f;
      break;
    case PopupStyle::Ps5:
      // Fades in with a short slide from the right, holds, fades out the same way.
      f.alpha = Ramp(t, 0.f, 0.15f) * (1.f - Ramp(t, 5.5f, 5.7f));
      f.slide = t < 2.f ? 1.f - EaseOut(Ramp(t, 0.f, 0.15f)) : EaseIn(Ramp(t, 5.5f, 5.7f));
      f.text_alpha = f.alpha;
      f.icon_scale = 1.f;
      f.ring = 1.f;
      f.glyph_scale = 1.f;
      f.swap = 1.f;
      f.bar = 1.f;
      f.light = 1.f;
      break;
  }
  return f;
}

std::optional<std::pair<std::string, std::string>> LiveCvarFor(const std::string& option_id, double value) {
  const int i = static_cast<int>(value + 0.5);
  auto pick = [i](std::initializer_list<const char*> names) {
    const int n = static_cast<int>(names.size());
    return std::string(*(names.begin() + (i >= 0 && i < n ? i : 0)));
  };
  if (option_id == "AchievementStyle") return std::make_pair(std::string("cogbox_ach_style"), pick({"x360", "xbox", "ps5"}));
  if (option_id == "AchievementColour")
    return std::make_pair(std::string("cogbox_ach_colour"),
                          pick({"dark", "red", "green", "blue", "purple", "orange", "pink", "teal", "grey", "gold"}));
  if (option_id == "AchievementPosition")
    return std::make_pair(std::string("cogbox_ach_position"),
                          pick({"default", "top_left", "top_centre", "top_right", "bottom_left", "bottom_centre", "bottom_right"}));
  if (option_id == "AchievementSound") return std::make_pair(std::string("cogbox_ach_sound"), std::string(i ? "true" : "false"));
  if (option_id == "HideAchievementPopups") return std::make_pair(std::string("cogbox_ach_hide"), std::string(i ? "true" : "false"));
  return std::nullopt;
}

namespace {
size_t NextChar(const std::string& s, size_t i) {  // the index after the UTF-8 character at i
  ++i;
  while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
  return i;
}
std::string TrimSpaces(const std::string& s) {
  const size_t a = s.find_first_not_of(' ');
  if (a == std::string::npos) return {};
  return s.substr(a, s.find_last_not_of(' ') - a + 1);
}
}  // namespace

std::vector<std::string> WrapText(const std::string& text, float max_width,
                                  const std::function<float(const std::string&)>& measure, int max_lines) {
  static const std::string kEllipsis = "\xE2\x80\xA6";
  std::vector<std::string> lines;
  std::string rest = TrimSpaces(text);
  while (!rest.empty() && static_cast<int>(lines.size()) < max_lines) {
    if (measure(rest) <= max_width) {
      lines.push_back(rest);
      break;
    }
    if (static_cast<int>(lines.size()) + 1 == max_lines) {  // the last line: cut with the ellipsis
      size_t keep = 0;
      for (size_t i = 0; i < rest.size(); i = NextChar(rest, i)) {
        const size_t next = NextChar(rest, i);
        if (measure(TrimSpaces(rest.substr(0, next)) + kEllipsis) > max_width) break;
        keep = next;
      }
      lines.push_back(TrimSpaces(rest.substr(0, keep)) + kEllipsis);
      break;
    }
    // the longest run of whole words that fits; else the longest run of characters (at least one)
    size_t cut = 0;
    for (size_t sp = rest.find(' '); sp != std::string::npos; sp = rest.find(' ', sp + 1)) {
      if (measure(rest.substr(0, sp)) > max_width) break;
      cut = sp;
    }
    if (cut == 0) {
      cut = NextChar(rest, 0);
      for (size_t i = cut; i < rest.size(); i = NextChar(rest, i)) {
        const size_t next = NextChar(rest, i);
        if (measure(rest.substr(0, next)) > max_width) break;
        cut = next;
      }
    }
    lines.push_back(TrimSpaces(rest.substr(0, cut)));
    rest = TrimSpaces(rest.substr(cut));
  }
  return lines;
}

}  // namespace xenos::ach
