// ue3/gowj_pause_package_patch.h: in split screen Judgment's pause menu (and the message boxes it opens) cover the whole
// window (GitHub #420, task 13), applied with Gears 2's package patch code (ue3/package_patch.h). Synthetic buffers only.
#include <catch2/catch_test_macros.hpp>

#include <ue3/gowj_pause_package_patch.h>
#include <ue3/package_patch.h>

namespace {
// GearGame.xxx decompressed (94,780,700 bytes): the FName index of `case 'PauseMenuSP':` in
// UIScene_Pause.WidgetInitialized and of `case 'LayoutPanel':` in UIScene_MessageBox.WidgetInitialized.
constexpr uint64_t kPauseCase = 0x1647E59, kMessageBoxCase = 0x165DEEF;
// Default__UIScene_Pause's bHideLesserMovies tag (name 0x1D60, BoolProperty 0x24CB) - task 15 B.
constexpr uint64_t kPauseTag = 0x164ADC6;
std::vector<uint8_t> Original() {
  std::vector<uint8_t> d(94780700, 0xAA);
  const uint8_t pause[4] = {0x00, 0x00, 0xBC, 0xBA}, box[4] = {0x00, 0x00, 0x9B, 0x56};
  for (int i = 0; i < 4; ++i) { d[kPauseCase + i] = pause[i]; d[kMessageBoxCase + i] = box[i]; }
  const uint8_t tag[12] = {0, 0, 0x1D, 0x60, 0, 0, 0, 0, 0, 0, 0x24, 0xCB};
  for (int i = 0; i < 12; ++i) d[kPauseTag + i] = tag[i];
  return d;
}
}  // namespace

TEST_CASE("Judgment pause package patch parses into the three GearGame.xxx edits", "[split]") {
  std::string err;
  const auto patches = ue3::ParsePackagePatches(gowj::kPauseFullscreenPackagePatch, &err);
  CHECK(err.empty());
  REQUIRE(patches.size() == 3);
  for (const auto& p : patches) CHECK(p.file == "GearGame.xxx");
}

TEST_CASE("Judgment pause package patch turns both split-layout case labels into 'None'", "[split]") {
  const auto patches = ue3::ParsePackagePatches(gowj::kPauseFullscreenPackagePatch);
  std::vector<uint8_t> d = Original();
  CHECK(ue3::ApplyPackagePatches(d, "geargame.xxx", patches) == 3);   // the name is matched case-insensitively
  CHECK(d[kPauseCase + 2] == 0xB1);  // FName 'None' (0xB10A)
  CHECK(d[kPauseCase + 3] == 0x0A);
  CHECK(d[kMessageBoxCase + 2] == 0xB1);
  CHECK(d[kMessageBoxCase + 3] == 0x0A);
  CHECK(d[kPauseTag + 2] == 0x1B);  // the tag is now bForceFullViewport (0x1BE4) = true
  CHECK(d[kPauseTag + 3] == 0xE4);
  CHECK(ue3::ApplyPackagePatches(d, "Engine.xxx", patches) == 0);  // other packages untouched
}

TEST_CASE("Judgment pause package patch refuses a different or short GearGame.xxx", "[split]") {
  const auto patches = ue3::ParsePackagePatches(gowj::kPauseFullscreenPackagePatch);
  std::vector<uint8_t> d = Original();
  d[kPauseCase + 3] = 0xBB;
  const std::vector<uint8_t> before = d;
  CHECK(ue3::ApplyPackagePatches(d, "GearGame.xxx", patches) == -1);
  CHECK(d == before);
  std::vector<uint8_t> shorter(kMessageBoxCase, 0xAA);
  CHECK(ue3::ApplyPackagePatches(shorter, "GearGame.xxx", patches) == -1);
}

TEST_CASE("Judgment pause full window only with the switch on and two or more local players", "[split]") {
  CHECK_FALSE(gowj::PauseFullscreenWanted(false, 2));
  CHECK_FALSE(gowj::PauseFullscreenWanted(true, 1));
  CHECK(gowj::PauseFullscreenWanted(true, 2));
  CHECK(gowj::PauseFullscreenWanted(true, 4));
}
