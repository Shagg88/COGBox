// ue3/gow2_pause_package_patch.h: in split screen Gears 2's pause menu and Options scenes cover the whole window (GitHub
// #420, task 13). Synthetic buffers only - no game data in the tests.
#include <catch2/catch_test_macros.hpp>

#include <ue3/gow2_pause_package_patch.h>
#include <ue3/package_patch.h>

namespace {
// GearGame.xxx decompressed (66,293,489 bytes): the SceneRenderMode tag values (FName SPLITRENDER_PlayerOwner = 0x7A55)
// of Default__GearUIScenePause_Base, Default__GearUISceneFEOptions_Base and Default__GearUISceneFETran_Options.
constexpr uint64_t kPause = 0xAC68E3, kOptions = 0xA65D98, kOptionsTran = 0xAC4111;
std::vector<uint8_t> Original() {
  std::vector<uint8_t> d(66293489, 0xAA);
  for (uint64_t at : {kPause, kOptions, kOptionsTran}) { d[at] = 0; d[at + 1] = 0; d[at + 2] = 0x7A; d[at + 3] = 0x55; }
  return d;
}
}  // namespace

TEST_CASE("Gears 2 pause package patch parses into the three GearGame.xxx edits", "[split]") {
  std::string err;
  const auto patches = ue3::ParsePackagePatches(gow2::kPauseFullscreenPackagePatch, &err);
  CHECK(err.empty());
  REQUIRE(patches.size() == 3);
  for (const auto& p : patches) CHECK(p.file == "GearGame.xxx");
}

TEST_CASE("Gears 2 pause package patch makes the pause and Options scenes SPLITRENDER_Fullscreen", "[split]") {
  const auto patches = ue3::ParsePackagePatches(gow2::kPauseFullscreenPackagePatch);
  std::vector<uint8_t> d = Original();
  CHECK(ue3::ApplyPackagePatches(d, "GearGame.xxx", patches) == 3);
  for (uint64_t at : {kPause, kOptions, kOptionsTran}) CHECK(d[at + 3] == 0x54);   // FName SPLITRENDER_Fullscreen
  CHECK(ue3::ApplyPackagePatches(d, "Engine.xxx", patches) == 0);  // other packages untouched
}

TEST_CASE("Gears 2 pause package patch refuses a different GearGame.xxx", "[split]") {
  const auto patches = ue3::ParsePackagePatches(gow2::kPauseFullscreenPackagePatch);
  std::vector<uint8_t> d = Original();
  d[kOptionsTran + 3] = 0x54;   // already full screen there: not the file we know
  const std::vector<uint8_t> before = d;
  CHECK(ue3::ApplyPackagePatches(d, "GearGame.xxx", patches) == -1);
  CHECK(d == before);
}

TEST_CASE("Gears 2 pause full window only with the switch on and two or more local players", "[split]") {
  CHECK_FALSE(gow2::PauseFullscreenWanted(false, 2));
  CHECK_FALSE(gow2::PauseFullscreenWanted(true, 1));
  CHECK(gow2::PauseFullscreenWanted(true, 2));
  CHECK(gow2::PauseFullscreenWanted(true, 3));
}
