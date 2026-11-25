// gow3_split_package_patch.h: the Engine.xxx script patch that makes Gears 3 itself pick the two-player split side by
// side (GitHub #420, task 5b). Synthetic buffers only - no game data in the tests.
#include <catch2/catch_test_macros.hpp>

#include <ue3/package_patch.h>

#include "../src/layer3_gow3/gow3_split_package_patch.h"

namespace {
// Engine.xxx decompressed (5,547,144 bytes): UpdateActiveSplitscreenType's case-2 `SplitType != eSST_2P_HORIZONTAL`
// byte constant, and Default__GameViewportClient's Default2PSplitType value (FName index of eSST_2P_HORIZONTAL).
constexpr uint64_t kCondByte = 0x368155, kDefaultName = 0x36B577;
// HasLeftSafeZone / HasRightSafeZone: `case eSST_2P_HORIZONTAL: return true;` (24 01 04 27).
constexpr uint64_t kLeftCase = 0x368867, kRightCase = 0x368941;
std::vector<uint8_t> Original() {
  std::vector<uint8_t> d(5547144, 0xAA);
  d[kCondByte] = 0x24; d[kCondByte + 1] = 0x01;
  const uint8_t name[4] = {0x00, 0x00, 0x1D, 0xD4};
  for (int i = 0; i < 4; ++i) d[kDefaultName + i] = name[i];
  for (uint64_t at : {kLeftCase, kRightCase}) { d[at] = 0x24; d[at + 1] = 0x01; d[at + 2] = 0x04; d[at + 3] = 0x27; }
  return d;
}
}  // namespace

TEST_CASE("split package patch parses into the four Engine.xxx edits", "[split]") {
  std::string err;
  const auto patches = ue3::ParsePackagePatches(gow3::kSplitVerticalPackagePatch, &err);
  CHECK(err.empty());
  REQUIRE(patches.size() == 4);
  for (const auto& p : patches) CHECK(p.file == "Engine.xxx");
}

TEST_CASE("split package patch turns the 2P choice into eSST_2P_VERTICAL", "[split]") {
  const auto patches = ue3::ParsePackagePatches(gow3::kSplitVerticalPackagePatch);
  std::vector<uint8_t> d = Original();
  CHECK(ue3::ApplyPackagePatches(d, "Engine.xxx", patches) == 4);
  CHECK(d[kLeftCase + 1] == 0x02);         // side by side: both players keep a left and a right safe zone
  CHECK(d[kRightCase + 1] == 0x02);
  CHECK(d[kCondByte] == 0x24);
  CHECK(d[kCondByte + 1] == 0x02);         // only eSST_2P_VERTICAL is kept as desired; anything else -> Default2PSplitType
  CHECK(d[kDefaultName + 3] == 0xD5);      // Default2PSplitType = eSST_2P_VERTICAL
  CHECK(ue3::ApplyPackagePatches(d, "GearGame.xxx", patches) == 0);  // other packages untouched
}

TEST_CASE("split package patch refuses a different Engine.xxx", "[split]") {
  const auto patches = ue3::ParsePackagePatches(gow3::kSplitVerticalPackagePatch);
  std::vector<uint8_t> d = Original();
  d[kCondByte + 1] = 0x03;
  const std::vector<uint8_t> before = d;
  CHECK(ue3::ApplyPackagePatches(d, "Engine.xxx", patches) == -1);
  CHECK(d == before);
}

TEST_CASE("only the hashes of our patched copies are recognised", "[split]") {
  uint8_t ours[20], other[20];
  for (int i = 0; i < 20; ++i) { ours[i] = uint8_t(0x40 + i); other[i] = uint8_t(0x90 + i); }
  CHECK_FALSE(ue3::IsPatchedPackageHash(ours));
  ue3::RememberPatchedPackageHash(ours);
  CHECK(ue3::IsPatchedPackageHash(ours));
  CHECK_FALSE(ue3::IsPatchedPackageHash(other));
}
