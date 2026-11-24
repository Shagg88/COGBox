// ue3/split_layout.h: the two-player split forced side by side (GitHub #420). Game-free; every game's copy is the same.
// The split is decided in UnrealScript (GameViewportClient.UpdateActiveSplitscreenType / LayoutPlayers), so the game's
// frame tick writes the result of this back into the viewport client whenever it differs.
#pragma once

#include <cstdint>

namespace ue3 {

// ESplitScreenType's eSST_2P_VERTICAL (2 in Gears 1, 2, 3 and Judgment).
inline constexpr uint8_t kSplitVertical = 2;

// The split type to keep: side by side when the option is on, exactly two players are signed in and the game chose a
// two-player horizontal split (eSST_2P_HORIZONTAL = 1, or Judgment's eSST_2P_HORIZONTAL16x9 = 6). Anything else is the
// game's own choice and is kept: eSST_NONE (a full-screen cutscene, menu or transition), one player, 3 / 4 players, or
// the option off.
constexpr uint8_t ForcedSplitType(uint8_t game_type, int local_players, bool want_vertical) {
  return want_vertical && local_players == 2 && (game_type == 1 || game_type == 6) ? kSplitVertical : game_type;
}

}  // namespace ue3
