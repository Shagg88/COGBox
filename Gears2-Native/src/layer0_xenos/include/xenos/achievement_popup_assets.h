// Layer 0 (game-free): the achievement popups' fonts and sound (docs/achievements/README.md).
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
// A second, small raster of the same fonts: text drawn at kAchievementSmallFontMax px or less uses it (a 48 px raster
// shrunk to 13-20 px reads thin and blurry - the browser at 720p).
constexpr float kAchievementSmallFontPixels = 22.f;
constexpr float kAchievementSmallFontMax = 24.f;
// The font to draw text `px` tall with (small or large raster), or nullptr when none could be loaded.
ImFont* XenosAchievementFontFor(bool semibold, float px);

// Plays the style's sound file ("x360" / "xbox" / "ps5"), or achievement.wav; asynchronously; silent (one log line)
// when there is no file.
void XenosPlayAchievementSound(const char* style_name);

// The COGBox folder: --cogbox_dir, else two folders up from the exe (every port is <COGBox>\<Game>\<port>\).
std::filesystem::path XenosCogboxDir();
