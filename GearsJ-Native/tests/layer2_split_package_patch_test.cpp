// ue3/gowj_split_package_patch.h: the Engine.xxx script patch that makes Judgment itself pick the two-player split side
// by side (GitHub #420, task 7b), applied with Gears 2's package patch code (ue3/package_patch.h). Synthetic buffers only.
#include <catch2/catch_test_macros.hpp>

#include <ue3/gowj_split_package_patch.h>
#include <ue3/package_patch.h>

namespace {
// Engine.xxx decompressed (5,771,693 bytes): UpdateActiveSplitscreenType's case-2 constants `24 01` and `24 06`,
// Default__GameViewportClient's Default2PSplitType value (FName eSST_2P_HORIZONTAL = 0x1F44), and HasLeftSafeZone /
// HasRightSafeZone's `case eSST_2P_HORIZONTAL: return true;` (24 01 04 27).
constexpr uint64_t kCondH = 0x38EE25, kCond16x9 = 0x38EE46, kDefaultName = 0x392D59, kLeftCase = 0x38F792,
                   kRightCase = 0x38F871,
                   kPixelWidth = 0x38FA15;
std::vector<uint8_t> Original() {
  std::vector<uint8_t> d(5771693, 0xAA);
  d[kCondH] = 0x24; d[kCondH + 1] = 0x01;
  d[kCond16x9] = 0x24; d[kCond16x9 + 1] = 0x06;
  const uint8_t name[4] = {0x00, 0x00, 0x1F, 0x44};
  for (int i = 0; i < 4; ++i) d[kDefaultName + i] = name[i];
  d[kPixelWidth] = 0x38; d[kPixelWidth + 1] = 0x3F; d[kPixelWidth + 2] = 0x2C; d[kPixelWidth + 3] = 0x02;
  d[kPixelWidth + 4] = 0x16;
  for (uint64_t at : {kLeftCase, kRightCase}) { d[at] = 0x24; d[at + 1] = 0x01; d[at + 2] = 0x04; d[at + 3] = 0x27; }
  return d;
}
}  // namespace

TEST_CASE("gowj split package patch parses into the six Engine.xxx edits", "[split]") {
  std::string err;
  const auto patches = ue3::ParsePackagePatches(gowj::kSplitVerticalPackagePatch, &err);
  CHECK(err.empty());
  REQUIRE(patches.size() == 6);
  for (const auto& p : patches) CHECK(p.file == "Engine.xxx");
}

TEST_CASE("gowj split package patch: 1 and 6 both end up eSST_2P_VERTICAL, both safe zones kept", "[split]") {
  const auto patches = ue3::ParsePackagePatches(gowj::kSplitVerticalPackagePatch);
  std::vector<uint8_t> d = Original();
  CHECK(ue3::ApplyPackagePatches(d, "engine.XXX", patches) == 6);   // names match case-insensitively
  CHECK(d[kCondH + 1] == 0x02);       // horizontal no longer kept: takes Default2PSplitType
  CHECK(d[kCond16x9 + 1] == 0x02);    // eSST_2P_HORIZONTAL16x9 neither
  CHECK(d[kDefaultName + 3] == 0x46); // eSST_2P_VERTICAL
  CHECK(d[kLeftCase + 1] == 0x02);
  CHECK(d[kRightCase + 1] == 0x02);
  CHECK(d[kPixelWidth + 3] == 0x01);   // side by side: safe zones from each half's width
  CHECK(ue3::ApplyPackagePatches(d, "GearGame.xxx", patches) == 0);
}

TEST_CASE("gowj split package patch refuses a different Engine.xxx", "[split]") {
  const auto patches = ue3::ParsePackagePatches(gowj::kSplitVerticalPackagePatch);
  std::vector<uint8_t> d = Original();
  d[kCond16x9 + 1] = 0x03;
  const std::vector<uint8_t> before = d;
  CHECK(ue3::ApplyPackagePatches(d, "Engine.xxx", patches) == -1);
  CHECK(d == before);
  std::vector<uint8_t> shorter(kCondH + 1, 0);
  CHECK(ue3::ApplyPackagePatches(shorter, "Engine.xxx", patches) == -1);
}

TEST_CASE("gowj: only the hashes of our patched copies are recognised", "[split]") {
  uint8_t ours[20], other[20];
  for (int i = 0; i < 20; ++i) { ours[i] = uint8_t(0x41 + i); other[i] = uint8_t(0x91 + i); }
  CHECK_FALSE(ue3::IsPatchedPackageHash(ours));
  ue3::RememberPatchedPackageHash(ours);
  CHECK(ue3::IsPatchedPackageHash(ours));
  CHECK_FALSE(ue3::IsPatchedPackageHash(other));
}
