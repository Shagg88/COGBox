// GitHub #420: Gears 2 side by side, the achievement-progress / unlock toast (GearUIScene_Toast) was cut at the window's
// centre in player 1's view. GearUIScene_Toast.ShowAlert plays SlideIn_1280[Slot] when UIScene.CurrentViewportSize.X >
// 960, else SlideIn_960[Slot]; their position track is the panel's left edge in absolute pixels: 1300 -> 950 -> 878 (hold
// 6 s) -> 1300 at 1280, 1000 -> 630 -> 558 -> 1000 at 960 - in both 402 px left of the view's right edge. A 640-wide
// side-by-side view takes the 960 animation, so the panel stopped at 558 and ran 160+ px past the view's edge. With two
// players side by side the 960 keys move 320 px left (402 px from a 640 view's right edge, as the game's own layouts):
// 680 -> 310 -> 238 -> 680. Default__GearUIScene_Toast.SlideIn_960_Template_0..2 (exports 79903-79905 of GearGame.xxx,
// the disc file decompressed; the five keyframe values of each slot). Same length; chained after the pause patch.
// One player keeps the stock values (the patch is only made for two players side by side). Our code (MIT).
#pragma once

#include <cstdint>

namespace gow2 {
inline constexpr const char* kToastSlidePackagePatch =
    "GearGame.xxx@0x3EABE0A:447a0000>442a0000#GearUIScene_Toast.SlideIn_960_Template_0: X 1000 -> 680;"
    "GearGame.xxx@0x3EABF9E:441d8000>439b0000#GearUIScene_Toast.SlideIn_960_Template_0: X 630 -> 310;"
    "GearGame.xxx@0x3EAC132:440b8000>436e0000#GearUIScene_Toast.SlideIn_960_Template_0: X 558 -> 238;"
    "GearGame.xxx@0x3EAC2C6:440b8000>436e0000#GearUIScene_Toast.SlideIn_960_Template_0: X 558 -> 238;"
    "GearGame.xxx@0x3EAC45A:447a0000>442a0000#GearUIScene_Toast.SlideIn_960_Template_0: X 1000 -> 680;"
    "GearGame.xxx@0x3EAC9E6:447a0000>442a0000#GearUIScene_Toast.SlideIn_960_Template_1: X 1000 -> 680;"
    "GearGame.xxx@0x3EACB7A:441d8000>439b0000#GearUIScene_Toast.SlideIn_960_Template_1: X 630 -> 310;"
    "GearGame.xxx@0x3EACD0E:440b8000>436e0000#GearUIScene_Toast.SlideIn_960_Template_1: X 558 -> 238;"
    "GearGame.xxx@0x3EACEA2:440b8000>436e0000#GearUIScene_Toast.SlideIn_960_Template_1: X 558 -> 238;"
    "GearGame.xxx@0x3EAD036:447a0000>442a0000#GearUIScene_Toast.SlideIn_960_Template_1: X 1000 -> 680;"
    "GearGame.xxx@0x3EAD5C2:447a0000>442a0000#GearUIScene_Toast.SlideIn_960_Template_2: X 1000 -> 680;"
    "GearGame.xxx@0x3EAD756:441d8000>439b0000#GearUIScene_Toast.SlideIn_960_Template_2: X 630 -> 310;"
    "GearGame.xxx@0x3EAD8EA:440b8000>436e0000#GearUIScene_Toast.SlideIn_960_Template_2: X 558 -> 238;"
    "GearGame.xxx@0x3EADA7E:440b8000>436e0000#GearUIScene_Toast.SlideIn_960_Template_2: X 558 -> 238;"
    "GearGame.xxx@0x3EADC12:447a0000>442a0000#GearUIScene_Toast.SlideIn_960_Template_2: X 1000 -> 680";
constexpr bool ToastSlideWanted(bool split_vertical, uint32_t local_players) { return split_vertical && local_players == 2; }
}  // namespace gow2
