// GitHub #420 (task 13): in split screen Gears 2's pause menu and Options cover the whole window. Byte edits in
// GearGame.xxx (the decompressed package, 66,293,489 bytes, MD5 d359cf0aee507102dd5583d78f81e12b; Title Update 6's
// ScriptPatch_GearGame.bin doesn't touch these classes' defaults), served through the mods' folder view
// (config_install.cpp):
//  - Gears 2's menus are UIScenes. UIScene.SceneRenderMode (ESplitscreenRenderMode) picks where a scene draws:
//    SPLITRENDER_Fullscreen (0) the whole window, SPLITRENDER_PlayerOwner (1) its player's view only. Default__UIScene
//    (Engine.xxx) says PlayerOwner; GearGame's class defaults set it per class: GearUIScene_Base, the message box
//    (UIMessageBoxBase), the video options and the game-over pause say Fullscreen; GearUIScenePause_Base (the campaign
//    and versus pause menus), GearUISceneFEOptions_Base (Game / Audio / Controller / Versus settings) and
//    GearUISceneFETran_Options (the Options list) say PlayerOwner - the half-window pause and Game Settings.
//  - The value of those three SceneRenderMode tags (FName SPLITRENDER_PlayerOwner, 0x7A55) becomes
//    SPLITRENDER_Fullscreen (0x7A54), the value the video options already use. The scenes in Startup_*.xxx
//    (UI_FEOptions_*, UI_FETran_Options) carry no SceneRenderMode tag of their own, so they take the class default.
//    GearHUD_Base.PauseGame also gives everyone the pause post-process when the scene is Fullscreen.
// Same length (an FName index). Unpausing closes the scene; the game's split is untouched. One player: the player's
// view is the whole window anyway. Our code (MIT).
#pragma once

#include <cstdint>

namespace gow2 {
inline constexpr const char* kPauseFullscreenPackagePatch =
    "GearGame.xxx@0xAC68E3:00007a55>00007a54#GearUIScenePause_Base.SceneRenderMode = SPLITRENDER_Fullscreen;"
    "GearGame.xxx@0xA65D98:00007a55>00007a54#GearUISceneFEOptions_Base.SceneRenderMode = SPLITRENDER_Fullscreen;"
    "GearGame.xxx@0xAC4111:00007a55>00007a54#GearUISceneFETran_Options.SceneRenderMode = SPLITRENDER_Fullscreen";

// --gow2_pause_fullscreen and the local players (--local_players): only split screen needs it.
constexpr bool PauseFullscreenWanted(bool on, uint32_t local_players) { return on && local_players >= 2; }
}  // namespace gow2
