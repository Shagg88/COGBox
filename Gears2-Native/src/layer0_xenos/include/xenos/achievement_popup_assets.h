// Layer 0 (game-free): the achievement popups' fonts and sound (docs/achievements/2026-11-04-achievements-popups-design.md).
// Fonts come from the PC (Segoe UI; Tahoma or Arial when it is missing, as under Proton); none is shipped. The sound
// is <COGBox>\assets\sounds\achievement[-<style>].wav, a file the team can replace. Our code (MIT).
#pragma once

#include <filesystem>

struct ImFont;
struct ImFontAtlas;

// Call from the app's OnConfigureFonts. Adds the popup fonts (regular + semibold, 48 px; drawn scaled) to the atlas;
// Korean / Chinese characters found in the cached achievement lists are merged from the Windows CJK fonts.
void XenosConfigureAchievementFonts(ImFontAtlas* atlas);
// The popup font, or nullptr when none could be loaded (use the current font then).
ImFont* XenosAchievementFont(bool semibold);
// The size the popup fonts were rasterised at.
constexpr float kAchievementFontPixels = 48.f;

// Plays the style's sound file ("x360" / "xbox" / "ps5"), or achievement.wav; asynchronously; silent (one log line)
// when there is no file.
void XenosPlayAchievementSound(const char* style_name);

// The COGBox folder: --cogbox_dir, else two folders up from the exe (every port is <COGBox>\<Game>\<port>\).
std::filesystem::path XenosCogboxDir();
