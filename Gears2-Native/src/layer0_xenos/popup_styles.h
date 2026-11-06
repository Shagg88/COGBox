// Layer 0 (game-free), internal to the achievement popups: the three styles' drawing (popup_x360.cpp, popup_xbox.cpp,
// popup_ps5.cpp) and the shapes they share (popup_common.cpp). Every size is at 1080p and multiplied by `scale`.
// Our own drawing: no Microsoft / Sony artwork. Our code (MIT).
#pragma once

#include <xenos/achievements_popup_logic.h>

#include <imgui.h>
#include <imgui_internal.h>  // IM_PI, ShadeVertsLinearColorGradientKeepAlpha

#include <string>

namespace xenos::popup {

struct PopupDraw {
  ImVec2 pos;                 // the popup's top-left corner on screen
  float scale = 1.f;
  ach::PopupFrame f;
  std::string title;          // 360 / Xbox line 1 ("Achievement unlocked"); PS5: the achievement name
  std::string name;           // the achievement's name
  std::string description;    // Xbox page 2
  std::string sub;            // PS5 line 2 ("Trophy earned!" / "Platinum trophy earned!")
  uint32_t gamerscore = 0;
  ach::TrophyGrade grade = ach::TrophyGrade::Bronze;
  bool rare = false;
  ach::ColourPreset colour{};
  ImTextureID icon = 0;       // the achievement's own icon (0 = none)
};

// The popup's full size (at the given scale) - for placing it on screen.
ImVec2 SizeX360(float scale);
ImVec2 SizeXbox(float scale);
ImVec2 SizePs5(float scale);

void DrawX360(ImDrawList* dl, const PopupDraw& d);
void DrawXbox(ImDrawList* dl, const PopupDraw& d);
void DrawPs5(ImDrawList* dl, const PopupDraw& d);

// --- shared (popup_common.cpp) ---
ImU32 Col(uint8_t r, uint8_t g, uint8_t b, float alpha);
ImU32 Col(const ach::Rgb& c, float alpha, float shade = 1.f);  // shade < 1 darker, > 1 lighter
// Text fitted into max_width (ach::FitText): one line, at `size` (or 90 %), cut with an ellipsis if still too long.
void Text(ImDrawList* dl, bool semibold, float size, ImVec2 pos, ImU32 col, const std::string& text, float max_width);
float TextWidth(bool semibold, float size, const std::string& text);
// White trophy cup centred on c, `h` tall; squeeze_x 0-1 for the Xbox coin flip.
void Trophy(ImDrawList* dl, ImVec2 c, float h, ImU32 col, float squeeze_x = 1.f);
// Diamond with laurels (Xbox "Rare").
void Diamond(ImDrawList* dl, ImVec2 c, float h, ImU32 col, float squeeze_x = 1.f);
// The achievement icon clipped to a circle of radius r (squeezed horizontally by squeeze_x).
void RoundIcon(ImDrawList* dl, ImTextureID icon, ImVec2 c, float r, float alpha, float squeeze_x = 1.f);
// The white "G" coin.
void GCoin(ImDrawList* dl, ImVec2 c, float r, float alpha);
// PS5 grade colour.
ImU32 GradeCol(ach::TrophyGrade g, float alpha);

}  // namespace xenos::popup
