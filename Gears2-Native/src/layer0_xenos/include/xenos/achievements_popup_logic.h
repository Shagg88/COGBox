// Layer 0 (game-free): the achievement popups' rules (docs/achievements/2026-11-04-achievements-popups-design.md):
// trophy grade, rare, "completes the game", the settings' names, text fitting and the animation timeline. Standard
// C++17 only, unit-tested; the drawing (achievement_popup.cpp) only reads these. Our code (MIT).
#pragma once

#include <xenos/achievements_data.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace xenos::ach {

// PS5 style: the trophy grade for an achievement. GP <= 30 Bronze, 31-74 Silver, >= 75 Gold; Gears 1's Soldier (13) and
// Commando (19) are Gold, as in Gears of War: Reloaded.
enum class TrophyGrade { Bronze, Silver, Gold, Platinum };
TrophyGrade GradeFor(uint32_t title_id, uint32_t achievement_id, uint32_t gamerscore);

// Modern Xbox style: "Rare achievement unlocked" when fewer than 10 % of players have it (never while rarity is unknown).
bool IsRare(const Rarity& r);

// (id, unlocked) for the game's whole list, read after the SDK recorded `just_unlocked`: true when that unlock was the
// last one, which makes the game 100 % (PS5 style: the Platinum popup).
bool CompletesGame(const std::vector<std::pair<uint32_t, bool>>& states, uint32_t just_unlocked);

enum class PopupStyle { X360, Xbox, Ps5 };
enum class PopupPosition { Default, TopLeft, TopCentre, TopRight, BottomLeft, BottomCentre, BottomRight };
struct Rgb {
  uint8_t r, g, b;
};
struct ColourPreset {
  const char* name;
  Rgb fill;
  Rgb border;  // the same as fill except Dark's
};
extern const std::array<ColourPreset, 10> kColourPresets;  // the launcher's choice order

PopupStyle ParseStyle(const std::string& s);           // x360 / xbox / ps5; anything else: X360
const ColourPreset& ParseColour(const std::string& s);  // a preset name, any case; anything else: Dark
PopupPosition ParsePosition(const std::string& s);     // default / top_left / ... / bottom_right; else Default
PopupPosition ResolvePosition(PopupStyle style, PopupPosition p);  // Default: PS5 top right, the others bottom centre

// Fits a line into max_width: as it is (scale 1), else smaller (scale 0.9), else at 0.9 with whole UTF-8 characters
// dropped from the end and an ellipsis added. `measure` gives a text's width at scale 1.
std::string FitText(const std::string& text, float max_width, const std::function<float(const std::string&)>& measure,
                    float* scale_out);

// The animation: where each moving part of a popup is, `seconds` after it started. 5 s per popup: 0.5 s intro, hold,
// 0.5 s outro. The styles draw only from these numbers.
constexpr float kPopupSeconds = 5.0f;
struct PopupFrame {
  float alpha = 0;       // the whole popup, 0-1
  float icon_scale = 0;  // orb / badge (the Xbox badge overshoots to 1.12)
  float bar = 0;         // pill / box width, 0-1 (0 = only the icon)
  float slide = 1;       // 0 = in place, 1 = fully off screen (PS5: to the right)
  float text_alpha = 0;
  float shine = -1;      // Xbox trophy shine position 0-1, -1 = none
  bool finished = false;
};
PopupFrame PopupTimeline(PopupStyle style, float seconds);

}  // namespace xenos::ach
