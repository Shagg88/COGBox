// Layer 0 (game-free): the achievement popups' rules (docs/achievements/2026-11-04-achievements-popups-design.md):
// trophy grade, rare, "completes the game", the settings' names, text fitting and the animation timeline. Standard
// C++17 only, unit-tested; the drawing (achievement_popup.cpp) only reads these. Our code (MIT).
#pragma once

#include <xenos/achievements_data.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
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
// Text wrapped at spaces into at most max_lines lines no wider than max_width (a word wider than a line breaks inside it,
// on a UTF-8 character boundary); what does not fit on the last line is cut with an ellipsis. Empty text: no lines.
std::vector<std::string> WrapText(const std::string& text, float max_width,
                                  const std::function<float(const std::string&)>& measure, int max_lines);

// The animation, measured from reference clips of the real popups (docs/achievements/...-popups-design.md): where
// each moving part of a popup is, `seconds` after it started. The styles draw only from these numbers.
float PopupDuration(PopupStyle style);  // X360 6.1 s, Xbox 7.3 s, PS5 5.7 s
constexpr float kPs5SlidePixels = 10.f;  // the PS5 box's short slide (at 1080p)
struct PopupFrame {
  float alpha = 0;        // the whole popup, 0-1
  float icon_scale = 0;   // the orb / badge size, 0-1
  float ring = 0;         // X360: the four-part ring's opacity; Xbox: hollow ring (0) -> filled badge (1)
  float light = 0;        // X360: the ring's green quarter lit, 0-1 (comes on a moment after the ring)
  float cover = 0;        // X360: 1 = the sphere fills the whole orb (hides the ring), 0 = its normal size
  float glow = 0;         // X360: the white ghost of the orb that flashes before it fades in, 0-1
  float glyph_scale = 0;  // Xbox: the icon inside the badge, grown from a dot
  float swap = 0;         // X360: 0 the X, 1 the achievement icon (dark between); Xbox: the coin flip from the
                          // achievement icon (0) to the trophy / diamond (1), drawn squeezed by |cos(pi * swap)|
  float bar = 0;          // pill / box width, 0-1 (360: slides out from behind the orb); Xbox opens from the centre
  float slide = 1;        // PS5: the box, X360: the text; 0 in place, 1 = kPs5SlidePixels to the right
  float text_alpha = 0;
  float page = 0;         // Xbox: 0 the title page, 1 the description page (cross-fade between)
  bool finished = false;
};
PopupFrame PopupTimeline(PopupStyle style, float seconds);

// The launcher's popup options (their ids and choice indices, as the in-game menu gets them) as the game's cvar and value:
// the menu sets these at once - the popups read them on every unlock - so they need no restart. nullopt for any other
// option. An index out of range gives the first choice, as the launcher does.
std::optional<std::pair<std::string, std::string>> LiveCvarFor(const std::string& option_id, double value);

}  // namespace xenos::ach
