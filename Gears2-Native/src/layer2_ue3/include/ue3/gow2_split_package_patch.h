// GitHub #420 (task 6b): Gears 2 itself picks the two-player split side by side. Byte edits in Engine.xxx (the disc
// file, decompressed: 9,272,641 bytes, as its table-of-contents line says; Title Update 6 doesn't replace it, and its
// ScriptPatch_Engine.bin doesn't touch GameViewportClient), made with --gow2_split_vertical (config_install.cpp):
//  1. GameViewportClient.UpdateActiveSplitscreenType, case 2 players:
//       if (SplitType != eSST_2P_HORIZONTAL && SplitType != eSST_2P_VERTICAL) SplitType = Default2PSplitType;
//     SplitType starts as DesiredSplitscreenType, which GearPC.UpdateLocalCacheOfProfileSettings sets from the profile's
//     "Split-Screen Type" (GearProfileSettings.GetPreferredSplitscreenType, default horizontal) on every profile save -
//     the autosave that undid the old ini setting. The first constant (`24 01`) becomes 2, so only side by side is
//     kept and anything else takes Default2PSplitType. Same length, so no jump offset moves.
//  2. Default__GameViewportClient's Default2PSplitType tag (not a config property, so the ini can't set it): FName
//     eSST_2P_HORIZONTAL (0x1BB2) -> eSST_2P_VERTICAL (0x1BB3). Default__GearGameViewportClient doesn't set it.
//  3. HasLeftSafeZone / HasRightSafeZone (GearHUD_Base.CalculateDeadZone -> CalculateDeadZoneForAllSides ->
//     SafeZoneLeft / SafeZoneRight): for eSST_2P_VERTICAL the stock code gives player 0 no right safe zone and player 1
//     no left one, and UpdateWeaponIndicator / DrawWeaponInfo place the weapon widget from SafeZoneRight - the same
//     pattern that cut Gears 3's player-1 weapon HUD at the centre. The `case eSST_2P_HORIZONTAL: return true;` label
//     (24 01 04 27) becomes case 2, so side by side keeps every side's safe zone. Type 1 never happens for two players
//     with 1-2 on; one player (0) and 3 / 4 players (3-5) are other cases.
// GearGameViewportClient.UpdateActiveSplitscreenType still forces eSST_NONE (full screen) when
// ShouldForceFullscreenViewport() is true (cutscenes), before it calls this. Our code (MIT).
#pragma once

namespace gow2 {
inline constexpr const char* kSplitVerticalPackagePatch =
    "Engine.xxx@0x3863B5:2401>2402#GameViewportClient.UpdateActiveSplitscreenType: 2 players take Default2PSplitType;"
    "Engine.xxx@0x3893E5:00001bb2>00001bb3#GameViewportClient.Default2PSplitType = eSST_2P_VERTICAL;"
    "Engine.xxx@0x386B6F:24010427>24020427#GameViewportClient.HasLeftSafeZone: side by side keeps it;"
    "Engine.xxx@0x386C7A:24010427>24020427#GameViewportClient.HasRightSafeZone: side by side keeps it";
}  // namespace gow2
