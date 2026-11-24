// gow3_pause_package_patch.h: in split screen the pause menu (and the message boxes it opens) cover the whole window
// (GitHub #420, task 13). Synthetic buffers only - no game data in the tests.
#include <catch2/catch_test_macros.hpp>

#include <ue3/package_patch.h>

#include "../src/layer3_gow3/gow3_pause_package_patch.h"

namespace {
// GearGame.xxx decompressed (86,443,270 bytes): the FName index of the `case 'PauseMenuSP':` label in
// UIScene_Pause.WidgetInitialized and of `case 'LayoutPanel':` in UIScene_MessageBox.WidgetInitialized; the
// bHideLesserMovies tag of Default__UIScene_Pause (name, then type BoolProperty).
constexpr uint64_t kPauseCase = 0x125937A, kMessageBoxCase = 0x1261A68, kPauseTag = 0x125C5C0, kBoxTag = 0x1261FDE;
std::vector<uint8_t> Original() {
  std::vector<uint8_t> d(86443270, 0xAA);
  const uint8_t pause[4] = {0x00, 0x00, 0x9E, 0x51}, box[4] = {0x00, 0x00, 0x81, 0xF3};
  for (int i = 0; i < 4; ++i) { d[kPauseCase + i] = pause[i]; d[kMessageBoxCase + i] = box[i]; }
  const uint8_t tag[12] = {0x00, 0x00, 0x18, 0x7A, 0, 0, 0, 0, 0x00, 0x00, 0x1E, 0x65};
  for (int i = 0; i < 12; ++i) d[kPauseTag + i] = tag[i];
  const uint8_t boxtag[12] = {0x00, 0x00, 0x12, 0x9C, 0, 0, 0, 0, 0x00, 0x00, 0x1E, 0x65};
  for (int i = 0; i < 12; ++i) d[kBoxTag + i] = boxtag[i];
  return d;
}
}  // namespace

TEST_CASE("pause package patch parses into the four GearGame.xxx edits", "[split]") {
  std::string err;
  const auto patches = ue3::ParsePackagePatches(gow3::kPauseFullscreenPackagePatch, &err);
  CHECK(err.empty());
  REQUIRE(patches.size() == 4);
  for (const auto& p : patches) CHECK(p.file == "GearGame.xxx");
}

TEST_CASE("pause package patch turns both split-layout case labels into 'None'", "[split]") {
  const auto patches = ue3::ParsePackagePatches(gow3::kPauseFullscreenPackagePatch);
  std::vector<uint8_t> d = Original();
  CHECK(ue3::ApplyPackagePatches(d, "GearGame.xxx", patches) == 4);
  CHECK(d[kBoxTag + 2] == 0x17);  // the message box defaults' tag too
  CHECK(d[kBoxTag + 3] == 0x57);
  CHECK(d[kPauseTag + 2] == 0x17);  // the pause defaults' tag is now bForceFullViewport (0x1757)
  CHECK(d[kPauseTag + 3] == 0x57);
  CHECK(d[kPauseCase + 2] == 0x94);  // FName 'None' (0x94A1)
  CHECK(d[kPauseCase + 3] == 0xA1);
  CHECK(d[kMessageBoxCase + 2] == 0x94);
  CHECK(d[kMessageBoxCase + 3] == 0xA1);
  CHECK(ue3::ApplyPackagePatches(d, "Engine.xxx", patches) == 0);  // other packages untouched
}

TEST_CASE("pause package patch refuses a different GearGame.xxx", "[split]") {
  const auto patches = ue3::ParsePackagePatches(gow3::kPauseFullscreenPackagePatch);
  std::vector<uint8_t> d = Original();
  d[kMessageBoxCase + 3] = 0xF4;
  const std::vector<uint8_t> before = d;
  CHECK(ue3::ApplyPackagePatches(d, "GearGame.xxx", patches) == -1);
  CHECK(d == before);
}

TEST_CASE("pause full window only with the switch on and two or more local players", "[split]") {
  CHECK_FALSE(gow3::PauseFullscreenWanted(false, 2));
  CHECK_FALSE(gow3::PauseFullscreenWanted(true, 0));
  CHECK_FALSE(gow3::PauseFullscreenWanted(true, 1));
  CHECK(gow3::PauseFullscreenWanted(true, 2));
  CHECK(gow3::PauseFullscreenWanted(true, 4));
}
