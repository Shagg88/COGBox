// GitHub #420: Gears 2 side by side, subtitles in the middle of the screen. GearGameViewportClient.GetSubtitleRegion
// calls Engine's GameViewportClient.GetSubtitleRegion outside cutscenes, which sets
// MaxPos.Y = (GamePlayers.Length == 1 ? 0.9 : 0.5) - with two players the subtitles end at the screen's middle, made
// for a top / bottom split. Side by side each view is the window's full height, so the two-player value becomes 0.9
// too (subtitles near the bottom, as with one player). Engine.xxx export GetSubtitleRegion (file 0x3867C0, 135 bytes):
// the float constant 0.5 (`1E 3F000000`) after the 0.9 one. Same length. Same fix as Judgment's
// (GearsJ-Native ue3/gowj_subtitle_region_package_patch.h). Our code (MIT).
#pragma once

#include <cstdint>

namespace gow2 {
inline constexpr const char* kSubtitleRegionPackagePatch =
    "Engine.xxx@0x38682A:3F66666600051E3F000000>3F66666600051E3F666666#GameViewportClient.GetSubtitleRegion: side by "
    "side, subtitles near the bottom (0.9) as with one player";
constexpr bool SubtitleRegionWanted(bool split_vertical, uint32_t local_players) { return split_vertical && local_players >= 2; }
}  // namespace gow2
