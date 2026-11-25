// ue3/gow2_split_package_patch.h: the Engine.xxx script patch that makes Gears 2 itself pick the two-player split side
// by side (GitHub #420, task 6b), and the package patch machinery (ue3/package_patch.h). Synthetic buffers only.
#include <catch2/catch_test_macros.hpp>

#include <ue3/gow2_split_package_patch.h>
#include <ue3/package_patch.h>

namespace {
// Engine.xxx decompressed (9,272,641 bytes): UpdateActiveSplitscreenType's case-2 `SplitType != eSST_2P_HORIZONTAL`
// byte constant, Default__GameViewportClient's Default2PSplitType value (FName eSST_2P_HORIZONTAL = 0x1BB2), and
// HasLeftSafeZone / HasRightSafeZone's `case eSST_2P_HORIZONTAL: return true;` (24 01 04 27).
constexpr uint64_t kCondByte = 0x3863B5, kDefaultName = 0x3893E5, kLeftCase = 0x386B6F, kRightCase = 0x386C7A;
std::vector<uint8_t> Original() {
  std::vector<uint8_t> d(9272641, 0xAA);
  d[kCondByte] = 0x24; d[kCondByte + 1] = 0x01;
  const uint8_t name[4] = {0x00, 0x00, 0x1B, 0xB2};
  for (int i = 0; i < 4; ++i) d[kDefaultName + i] = name[i];
  for (uint64_t at : {kLeftCase, kRightCase}) { d[at] = 0x24; d[at + 1] = 0x01; d[at + 2] = 0x04; d[at + 3] = 0x27; }
  return d;
}
}  // namespace

TEST_CASE("gow2 split package patch parses into the four Engine.xxx edits", "[split]") {
  std::string err;
  const auto patches = ue3::ParsePackagePatches(gow2::kSplitVerticalPackagePatch, &err);
  CHECK(err.empty());
  REQUIRE(patches.size() == 4);
  for (const auto& p : patches) CHECK(p.file == "Engine.xxx");
}

TEST_CASE("gow2 split package patch makes the 2P choice eSST_2P_VERTICAL with both safe zones", "[split]") {
  const auto patches = ue3::ParsePackagePatches(gow2::kSplitVerticalPackagePatch);
  std::vector<uint8_t> d = Original();
  CHECK(ue3::ApplyPackagePatches(d, "engine.XXX", patches) == 4);   // names match case-insensitively
  CHECK(d[kCondByte + 1] == 0x02);
  CHECK(d[kDefaultName + 3] == 0xB3);   // eSST_2P_VERTICAL
  CHECK(d[kLeftCase + 1] == 0x02);
  CHECK(d[kRightCase + 1] == 0x02);
  CHECK(ue3::ApplyPackagePatches(d, "GearGame.xxx", patches) == 0);
}

TEST_CASE("gow2 split package patch refuses a different Engine.xxx", "[split]") {
  const auto patches = ue3::ParsePackagePatches(gow2::kSplitVerticalPackagePatch);
  std::vector<uint8_t> d = Original();
  d[kRightCase + 1] = 0x03;
  const std::vector<uint8_t> before = d;
  CHECK(ue3::ApplyPackagePatches(d, "Engine.xxx", patches) == -1);
  CHECK(d == before);
  std::vector<uint8_t> shorter(kCondByte + 1, 0);
  CHECK(ue3::ApplyPackagePatches(shorter, "Engine.xxx", patches) == -1);
}

TEST_CASE("a bad package patch item is reported", "[split]") {
  std::string err;
  CHECK(ue3::ParsePackagePatches("Engine.xxx@0x10:2401>24", &err).empty());
  CHECK_FALSE(err.empty());
}

TEST_CASE("gow2: only the hashes of our patched copies are recognised", "[split]") {
  uint8_t ours[20], other[20];
  for (int i = 0; i < 20; ++i) { ours[i] = uint8_t(0x41 + i); other[i] = uint8_t(0x91 + i); }
  CHECK_FALSE(ue3::IsPatchedPackageHash(ours));
  ue3::RememberPatchedPackageHash(ours);
  CHECK(ue3::IsPatchedPackageHash(ours));
  CHECK_FALSE(ue3::IsPatchedPackageHash(other));
}
