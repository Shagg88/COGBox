// ue3/split_layout.h: the two-player split forced side by side (GitHub #420).
#include <catch2/catch_test_macros.hpp>

#include "ue3/split_layout.h"

using ue3::ForcedSplitType;
using ue3::kSplitVertical;

namespace {
constexpr uint8_t kHorizontal = 1;      // eSST_2P_HORIZONTAL
constexpr uint8_t kHorizontal16x9 = 6;  // Judgment's eSST_2P_HORIZONTAL16x9
}  // namespace

TEST_CASE("two players with the option on: side by side", "[split]") {
  CHECK(kSplitVertical == 2);
  CHECK(ForcedSplitType(kHorizontal, 2, true) == kSplitVertical);
  CHECK(ForcedSplitType(kHorizontal16x9, 2, true) == kSplitVertical);
  CHECK(ForcedSplitType(kSplitVertical, 2, true) == kSplitVertical);
}

TEST_CASE("other player counts or the option off: the game's type", "[split]") {
  CHECK(ForcedSplitType(kHorizontal, 3, true) == kHorizontal);
  CHECK(ForcedSplitType(kHorizontal, 2, false) == kHorizontal);
  CHECK(ForcedSplitType(kHorizontal16x9, 2, false) == kHorizontal16x9);
  CHECK(ForcedSplitType(0, 1, true) == 0);
  CHECK(ForcedSplitType(0, 2, true) == 0);  // eSST_NONE: a full-screen cutscene / menu is left alone
  CHECK(ForcedSplitType(5, 2, true) == 5);
  CHECK(ForcedSplitType(kHorizontal, 1, true) == kHorizontal);
}

TEST_CASE("Judgment: 16x9 horizontal goes side by side, eSST_NONE stays", "[split]") {
  CHECK(ForcedSplitType(6, 2, true) == 2);  // eSST_2P_HORIZONTAL16x9 -> eSST_2P_VERTICAL
  CHECK(ForcedSplitType(0, 2, true) == 0);  // eSST_NONE: full-screen cutscene / menu
  CHECK(ForcedSplitType(6, 4, true) == 6);  // 4 players: the game's own
}
