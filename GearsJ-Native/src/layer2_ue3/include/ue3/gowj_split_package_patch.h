// GitHub #420 (task 7b): Judgment itself picks the two-player split side by side. Byte edits in Engine.xxx (the disc
// file, decompressed: 5,771,693 bytes, as its table-of-contents line says; Title Update 4 doesn't replace it, and its
// ScriptPatch_Engine.bin only patches ForceFlushAsyncLoading / Camera.PlayWorldCameraShake /
// PlayerController.ClientStartOnlineGame), made with --gowj_split_vertical (config_install.cpp):
//  1. GameViewportClient.UpdateActiveSplitscreenType, case 2 players:
//       if (SplitType != eSST_2P_HORIZONTAL && SplitType != eSST_2P_VERTICAL && SplitType != eSST_2P_HORIZONTAL16x9)
//           SplitType = Default2PSplitType;
//     SplitType starts as DesiredSplitscreenType, which GearPC.UpdateLocalCacheOfProfileSettings sets to a literal
//     eSST_2P_HORIZONTAL (SetSplitscreenConfiguration(1)) on every profile update / save. Both constants 1 and 6
//     (`24 01`, `24 06`) become 2, so only side by side is kept and anything else takes Default2PSplitType. Same
//     length, so no jump offset moves.
//  2. Default__GameViewportClient's Default2PSplitType tag (not a config property): FName eSST_2P_HORIZONTAL (0x1F44)
//     -> eSST_2P_VERTICAL (0x1F46). GearGame.xxx has no eSST_* names, so Default__GearGameViewportClient doesn't set it.
//  3. HasLeftSafeZone / HasRightSafeZone (GearHUD_Base.CalculateDeadZone -> CalculateDeadZoneForAllSides ->
//     GearHUD_Base.SafeZoneLeft / SafeZoneRight, used by the weapon / skill gauge widgets): eSST_2P_VERTICAL shares
//     eSST_2P_HORIZONTAL16x9's case, which gives player 0 no right safe zone and player 1 no left one - Gears 3's
//     cut-at-the-centre pattern. The `case eSST_2P_HORIZONTAL: return true;` label (24 01 04 27) becomes case 2, so
//     side by side keeps every side's safe zone. Type 1 no longer happens for two players with 1 on.
//  4. GetPixelSizeOfScreen, case eSST_2P_VERTICAL: `out_Width = Canvas.ClipX * float(2)` (the full window), which
//     CalculateSafeZoneValues turns into out_Horizontal = Width * (1 - TitleSafeZone.MaxPercentX 0.9) / 2 = 64 px at 1280
//     on each side of each half. The constant 2 (`38 3F 2C 02`) becomes 1: the half's own width, so 32 px (task 15
//     D). Its only script caller is CalculateSafeZoneValues; the vertical size (ClipY) is unchanged.
// GearGameViewportClient.UpdateActiveSplitscreenType still forces eSST_NONE (full screen) when
// ShouldForceFullscreenViewport() (cutscenes) is true, before it calls this. Our code (MIT).
#pragma once

namespace gowj {
inline constexpr const char* kSplitVerticalPackagePatch =
    "Engine.xxx@0x38EE25:2401>2402#GameViewportClient.UpdateActiveSplitscreenType: 2P horizontal takes Default2PSplitType;"
    "Engine.xxx@0x38EE46:2406>2402#GameViewportClient.UpdateActiveSplitscreenType: 2P horizontal 16x9 takes it too;"
    "Engine.xxx@0x392D59:00001f44>00001f46#GameViewportClient.Default2PSplitType = eSST_2P_VERTICAL;"
    "Engine.xxx@0x38F792:24010427>24020427#GameViewportClient.HasLeftSafeZone: side by side keeps it;"
    "Engine.xxx@0x38F871:24010427>24020427#GameViewportClient.HasRightSafeZone: side by side keeps it;"
    "Engine.xxx@0x38FA15:383f2c0216>383f2c0116#GameViewportClient.GetPixelSizeOfScreen: side by side width = the half's";
}  // namespace gowj
