// GitHub #420 (task 8b): Gears 1 (TU5) itself picks the two-player split side by side. Byte edits in Engine.xxx (the disc
// file, decompressed: 7,503,548 bytes; TU5 doesn't replace it), made with --gow1_split_vertical (config_install.cpp).
// Read from the package's bytecode (2006 UE3, licensee version 0x18):
//  - GameViewportClient.LayoutPlayers is `SetSplitscreenConfiguration(SplitscreenType)`, and SetSplit(n) passes n; no
//    WarfareGame script calls either (WarPC.InitializeSplitscreen is empty), so the engine's native player add / remove
//    is what lays the players out. SplitscreenType starts as eSST_NOVALUE (7, Default__GameViewportClient).
//  - SetSplitscreenConfiguration: SplitscreenType = SplitType; for two players
//      if (SplitscreenType != eSST_2P_HORIZONTAL && SplitscreenType != eSST_2P_VERTICAL) SplitscreenType = Default2PSplitType;
//    and Default2PSplitType is eSST_2P_HORIZONTAL (1) in Default__GameViewportClient. WarGameViewportClient's override
//    keeps full screen for a cinematic (bCinematicMode) and otherwise calls this.
//  - The game's menu option WarGameSettings.Splitscreen (Coalesced.ini: "Vertical" / "Horizontal") is read by no script
//    at all, so its value can't make the choice: the package is patched instead.
// The edits (offsets in the decompressed file; each same length, so no jump offset moves):
//  1. 0x21C41C, the case-2 test's first constant `24 01` -> `24 02`: only side by side is kept, anything else takes
//     Default2PSplitType (as Gears 2's patch).
//  2. 0x21F1B5, Default__GameViewportClient's Default2PSplitType tag (size 1, index 0, value 1) -> value 2. That
//     export's data is at 0x21E7F8 (2,535 bytes); the tag (FName Default2PSplitType 0xB1C, type ByteProperty 0x773)
//     starts at +2477, so its size field is at 0x21F1B5 and the value byte at 0x21F1BD.
//  3. 0x21C9D0 / 0x21CACD, HasLeftSafeZone / HasRightSafeZone's `case eSST_2P_HORIZONTAL: return true;` (24 01 04 27)
//     -> case 2: side by side keeps both safe zones (stock: player 0 has no right one and player 1 no left one, which
//     puts WarHUD's SafeZoneRight widgets on the centre line; Gears 2 / 3 / Judgment have the same fix).
//  4. 0x21C6C0, GameViewportClient.GetSubtitleRegion (export at 0x21C663, 122 bytes): `MaxPos.Y = (GamePlayers.Length
//     == 1 ? 0.9 : 0.5)` - the two-player 0.5 (`1E 3F000000`, after the 0.9) becomes 0.9, so side by side subtitles sit
//     near the bottom as with one player instead of the screen's middle (made for a top / bottom split). Same fix as
//     Gears 2 / 3 / Judgment; one player never takes that branch.
// Our code (MIT).
#pragma once

namespace gow1 {
inline constexpr const char* kSplitVerticalPackagePatch =
    "Engine.xxx@0x21C41C:24011618>24021618#GameViewportClient.SetSplitscreenConfiguration: 2 players take Default2PSplitType;"
    "Engine.xxx@0x21F1B5:000000010000000001>000000010000000002#GameViewportClient.Default2PSplitType = eSST_2P_VERTICAL;"
    "Engine.xxx@0x21C9D0:24010427>24020427#GameViewportClient.HasLeftSafeZone: side by side keeps it;"
    "Engine.xxx@0x21CACD:24010427>24020427#GameViewportClient.HasRightSafeZone: side by side keeps it;"
    "Engine.xxx@0x21C6C0:3F66666600051E3F000000>3F66666600051E3F666666#GameViewportClient.GetSubtitleRegion: side by "
    "side, subtitles near the bottom (0.9) as with one player";
}  // namespace gow1
