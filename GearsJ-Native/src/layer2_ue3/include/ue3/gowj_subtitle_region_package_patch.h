// GitHub #420: side by side, subtitles near the bottom of the window. One float constant in Engine.xxx (decompressed,
// 5,771,693 bytes), served through the mods' folder view (config_install.cpp):
//   GameViewportClient.GetSubtitleRegion(out MinPos, out MaxPos):
//     MaxPos.X = 1;
//     MaxPos.Y = (GamePlayers.Length == 1) ? 0.9 : 0.5;
//   (GearGameViewportClient's override calls it for gameplay, one player or two; only full-screen cutscenes -
//   ShouldForceFullscreenViewport - set their own 0.9.) 0.5 ends the subtitles at the middle: right for the game's
//   top / bottom split (the band between the views), across the middle of both side-by-side views. The 0.5 (3F000000)
//   becomes 0.9 (3F666666), the one-player value. Only with --gowj_split_vertical and two or more local players: the
//   game's own top / bottom keeps its middle band. Our code (MIT).
#pragma once

#include <cstdint>

namespace gowj {
inline constexpr const char* kSubtitleRegionPackagePatch =
    "Engine.xxx@0x38F506:3F66666600051E3F000000>3F66666600051E3F666666#GameViewportClient.GetSubtitleRegion: side by "
    "side, subtitles near the bottom (0.9) as with one player";

constexpr bool SubtitleRegionWanted(bool split_vertical, uint32_t local_players) {
  return split_vertical && local_players >= 2;
}
}  // namespace gowj
