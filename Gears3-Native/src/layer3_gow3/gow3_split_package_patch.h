// GitHub #420 (task 5b): Gears 3 itself picks the two-player split side by side. Byte edits in Engine.xxx (the
// decompressed package, 5,547,144 bytes, MD5 2e1684b518700b1ad55c0e9e706cd67e; the disc file, which Title Update 6
// doesn't replace - its ScriptPatch_Engine.bin doesn't touch GameViewportClient), served through --ue3_package_patches:
//  1. GameViewportClient.UpdateActiveSplitscreenType, case 2 players:
//       if (SplitType != eSST_2P_HORIZONTAL && SplitType != eSST_2P_VERTICAL) SplitType = Default2PSplitType;
//     The first constant (ByteConst 1, `24 01`) becomes 2, so only side by side is kept as asked and anything else
//     (the game's horizontal) takes Default2PSplitType. Same length, so no jump offset moves.
//  2. Default__GameViewportClient's Default2PSplitType property tag: value FName eSST_2P_HORIZONTAL (name 0x1DD4) ->
//     eSST_2P_VERTICAL (0x1DD5). GearGameViewportClient's defaults don't set it, so they inherit it.
//  3. GameViewportClient.HasLeftSafeZone / HasRightSafeZone (used by CalculateDeadZoneForAllSides -> GearHUD_Base's
//     SafeZoneLeft / SafeZoneRight): for eSST_2P_VERTICAL the stock code gives player 0 no right safe zone and player 1
//     no left one (the screen centre). The weapon widget (UpdateWeaponIndicator: ClipX - SafeZoneRight - UL) overhangs
//     its anchor by ~33 px, so player 0's was cut at the centre (x 478-673 on 1280 wide) while player 1's sat 128 px
//     (the 80% safe zone of 1280) inside its edge. The `case eSST_2P_HORIZONTAL: return true;` label (24 01) becomes
//     case 2, so side by side keeps every side's safe zone, as the full screen does. Type 1 never happens for two players
//     with patch 1-2 on; 3 / 4 players (types 3-5) are other cases. Player 0's widget then sits at x 350-545.
// One player, 3 / 4 players and the GearGame override's full-screen cases (ShouldForceFullscreenViewport,
// IsTransitionLevel -> eSST_NONE) are other branches and stay as they were. Our code (MIT).
#pragma once

namespace gow3 {
inline constexpr const char* kSplitVerticalPackagePatch =
    "Engine.xxx@0x368155:2401>2402#GameViewportClient.UpdateActiveSplitscreenType: 2 players take Default2PSplitType;"
    "Engine.xxx@0x36B577:00001dd4>00001dd5#GameViewportClient.Default2PSplitType = eSST_2P_VERTICAL;"
    "Engine.xxx@0x368867:24010427>24020427#GameViewportClient.HasLeftSafeZone: side by side keeps it;"
    "Engine.xxx@0x368941:24010427>24020427#GameViewportClient.HasRightSafeZone: side by side keeps it";
}  // namespace gow3
