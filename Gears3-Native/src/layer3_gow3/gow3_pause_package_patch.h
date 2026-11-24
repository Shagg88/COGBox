// GitHub #420 (task 13): in split screen the pause menu covers the whole window. Byte edits in GearGame.xxx (the
// decompressed package, 86,443,270 bytes, MD5 dca12dda3cc43f2c415552054af12677; Title Update 6's
// ScriptPatch_GearGame.bin only replaces UIScene_Pause.ShouldOptionBeDisabled of these classes), served through
// --ue3_package_patches:
//  - The pause menu is a Scaleform movie (UIScene_Pause extends GearGFxMoviePlayer). Its movie always covers the
//    window; UIScene_Pause.WidgetInitialized squeezes it into the pausing player's view only in
//      case 'PauseMenuSP': SplitscreenLayoutObject = Widget; UpdateSplitscreenLayout();
//    UIScene_Options has no such case, which is why Options already covered the whole window.
//  - UIScene_MessageBox.WidgetInitialized does the same with `case 'LayoutPanel':` (the pause menu's Load checkpoint /
//    Restart / Quit confirmations and every other message box).
// The FName of both case labels becomes 'None' (0x94A1, never a widget's name), so SplitscreenLayoutObject stays None:
// the native layout has nothing to move and the menu keeps its full-window, one-player layout. WidgetInitialized then
// returns false for that widget (the "not handled" default, only a log line). Same length: no jump offset moves.
// Unpausing closes the movie; the game's split is untouched. One player: the layout never applied.
// 2026-11-24 (the two edits above were not enough: the user still saw each player's pause menu laid out in their own
// half, unscaled): as Judgment's task 15 B, GFxUI's GFxMoviePlayer.bForceFullViewport makes a movie use the whole
// window instead of its owner's view; Default__UIScene_PathChoice / Discoverable set it, Default__UIScene_Pause doesn't.
// Its bHideLesserMovies tag (BoolProperty, value true; file 0x125C5C0) is renamed to bForceFullViewport (FName 0x1757,
// already in GearGame's name table), same length. Cost: the lesser movies (the HUD) are no longer hidden behind the
// pause menu.
// The pause menu's confirmations (UIScene_MessageBox: Reload Last Checkpoint, Reset Option Defaults...) still showed in
// the player's half (user screenshot): its bBlurLesserMovies tag (true; file 0x1261FDE) becomes bForceFullViewport the
// same way. Cost: the HUD behind a message box is no longer blurred. Our code (MIT).
#pragma once

#include <cstdint>

namespace gow3 {
inline constexpr const char* kPauseFullscreenPackagePatch =
    "GearGame.xxx@0x125937A:00009e51>000094a1#UIScene_Pause.WidgetInitialized: no split layout for PauseMenuSP;"
    "GearGame.xxx@0x1261A68:000081f3>000094a1#UIScene_MessageBox.WidgetInitialized: no split layout for LayoutPanel;"
    "GearGame.xxx@0x125C5C0:0000187a0000000000001e65>000017570000000000001e65#Default__UIScene_Pause: bForceFullViewport;"
    "GearGame.xxx@0x1261FDE:0000129c0000000000001e65>000017570000000000001e65#Default__UIScene_MessageBox: bForceFullViewport";

// --gow3_pause_fullscreen and the local players (--local_players): only split screen needs it.
constexpr bool PauseFullscreenWanted(bool on, uint32_t local_players) { return on && local_players >= 2; }
}  // namespace gow3
