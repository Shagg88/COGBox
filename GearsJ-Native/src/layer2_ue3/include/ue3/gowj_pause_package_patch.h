// GitHub #420 (task 13): in split screen Judgment's pause menu covers the whole window. Byte edits in GearGame.xxx (the
// decompressed package, 94,780,700 bytes, MD5 f00785c7dfd08ff3782a70af0f5ea3ce; Title Update 4's ScriptPatch_GearGame.bin
// doesn't touch UIScene_Pause or UIScene_MessageBox), served through the mods' folder view (config_install.cpp):
//  - The pause menu is a Scaleform movie (UIScene_Pause extends GearGFxMoviePlayer). Its movie always covers the
//    window; UIScene_Pause.WidgetInitialized squeezes it into the pausing player's view only in
//      case 'PauseMenuSP': SplitscreenLayoutObject = Widget; UpdateSplitscreenLayout();
//    UIScene_Options has no such case, which is why Options already covered the whole window.
//  - UIScene_MessageBox.WidgetInitialized does the same with `case 'LayoutPanel':` (the pause menu's confirmations and
//    every other message box).
// The FName of both case labels becomes 'None' (0xB10A, never a widget's name), so SplitscreenLayoutObject stays None:
// the native layout has nothing to move and the menu keeps its full-window, one-player layout. WidgetInitialized then
// returns false for that widget (the "not handled" default). Same length: no jump offset moves. Unpausing closes the
// movie; the game's split is untouched. One player: the layout never applied. Same lever as Gears 3. Our code (MIT).
// Task 15 B (the two edits above were not enough: the user still saw the pause menu in player 1's half): GFxUI's
// GFxMoviePlayer.bForceFullViewport makes a movie use the whole window instead of its owner's view (the native
// split layout); Default__UIScene_PathChoice / ChunkResultCard / Discoverable set it, Default__UIScene_Pause doesn't.
// Its bHideLesserMovies tag (BoolProperty, value true) is renamed to bForceFullViewport (FName 0x1BE4, already in
// GearGame's name table), same length. Cost: the lesser movies (the HUD) are no longer hidden behind the pause menu.
#pragma once

#include <cstdint>

namespace gowj {
inline constexpr const char* kPauseFullscreenPackagePatch =
    "GearGame.xxx@0x1647E59:0000bcba>0000b10a#UIScene_Pause.WidgetInitialized: no split layout for PauseMenuSP;"
    "GearGame.xxx@0x165DEEF:00009b56>0000b10a#UIScene_MessageBox.WidgetInitialized: no split layout for LayoutPanel;"
    "GearGame.xxx@0x164ADC6:00001d6000000000000024cb>00001be400000000000024cb#Default__UIScene_Pause: bForceFullViewport";

// --gowj_pause_fullscreen and the local players (--local_players): only split screen needs it.
constexpr bool PauseFullscreenWanted(bool on, uint32_t local_players) { return on && local_players >= 2; }
}  // namespace gowj
