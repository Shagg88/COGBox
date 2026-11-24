// GitHub #420 (task 15b): in split screen player 2's bottom action prompts ("X + icon + arrow") sit at player 1's
// height. One byte in GearGame.xxx (decompressed, 94,780,700 bytes; Title Update 4's ScriptPatch_GearGame.bin doesn't
// touch GearHUD_Base.DrawActionInfo), served through the mods' folder view (config_install.cpp):
//   GearHUD_Base.DrawActionInfo, bottom actions (DrawY = Canvas.ClipY - StartY - TextH):
//     if (!PC.IsPrimaryPlayer() && PC.IsCoop()) StartY = SafeZoneBottom + 0;            // player 2
//     else { GetSubtitleSize(SubTitleW, SubTitleH); StartY = SafeZoneBottom + SubTitleH * ... + ...; }   // player 1
//     StartY += YOffset;
//   so player 2's prompts sat lower than player 1's. Only side by side needs the fix: in top / bottom player 1's
//   prompts are raised for the subtitles in the middle of the screen and player 2's low ones are right (Flippage). The
//   JumpIfNot (07 01 97) becomes a Jump (06 01 97) to the same else branch: every player takes player 1's branch. The
//   skipped condition bytes are never run; nothing moves. One local player is always primary, so this changes nothing
//   there - the patch is only added side by side with two or more local players (online coop clients are primary on
//   their own console). Our code (MIT).
#pragma once

#include <cstdint>

namespace gowj {
inline constexpr const char* kPromptHeightPackagePatch =
    "GearGame.xxx@0xEAEBF6:070197828119>060197828119#GearHUD_Base.DrawActionInfo: player 2's prompts at player 1's height";

// Side by side only (top / bottom keeps the game's heights); only split screen has a non-primary local player.
constexpr bool PromptHeightWanted(bool split_vertical, uint32_t local_players) { return split_vertical && local_players >= 2; }
}  // namespace gowj
