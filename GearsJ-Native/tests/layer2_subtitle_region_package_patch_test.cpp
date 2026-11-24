// ue3/gowj_subtitle_region_package_patch.h: side by side, subtitles near the bottom of the window, not across the
// middle (GitHub #420). Synthetic buffers only.
#include <catch2/catch_test_macros.hpp>

#include <ue3/gowj_subtitle_region_package_patch.h>
#include <ue3/package_patch.h>

namespace {
// Engine.xxx decompressed (5,771,693 bytes): GameViewportClient.GetSubtitleRegion's
// `MaxPos.Y = (GamePlayers.Length == 1) ? 0.9 : 0.5` - the two float constants (1E 3F666666, 1E 3F000000).
constexpr uint64_t kAt = 0x38F506;
std::vector<uint8_t> Original() {
  std::vector<uint8_t> d(5771693, 0xAA);
  const uint8_t b[11] = {0x3F, 0x66, 0x66, 0x66, 0x00, 0x05, 0x1E, 0x3F, 0x00, 0x00, 0x00};
  for (int i = 0; i < 11; ++i) d[kAt + i] = b[i];
  return d;
}
}  // namespace

TEST_CASE("gowj subtitle region patch: two players get the single-player 0.9", "[split]") {
  std::string err;
  const auto patches = ue3::ParsePackagePatches(gowj::kSubtitleRegionPackagePatch, &err);
  CHECK(err.empty());
  REQUIRE(patches.size() == 1);
  CHECK(patches[0].file == "Engine.xxx");
  std::vector<uint8_t> d = Original();
  CHECK(ue3::ApplyPackagePatches(d, "Engine.xxx", patches) == 1);
  CHECK(d[kAt + 7] == 0x3F);  // the 0.5 is now 0.9 (3F666666)
  CHECK(d[kAt + 8] == 0x66);
  CHECK(d[kAt + 9] == 0x66);
  CHECK(d[kAt + 10] == 0x66);
  CHECK(d[kAt + 3] == 0x66);  // the one-player 0.9 is unchanged
}

TEST_CASE("gowj subtitle region patch refuses a different Engine.xxx", "[split]") {
  const auto patches = ue3::ParsePackagePatches(gowj::kSubtitleRegionPackagePatch);
  std::vector<uint8_t> d = Original();
  d[kAt + 5] = 0x06;
  const std::vector<uint8_t> before = d;
  CHECK(ue3::ApplyPackagePatches(d, "Engine.xxx", patches) == -1);
  CHECK(d == before);
}

TEST_CASE("gowj subtitle region patch only side by side with two or more local players", "[split]") {
  CHECK_FALSE(gowj::SubtitleRegionWanted(false, 2));  // the game's own top / bottom keeps the middle band
  CHECK_FALSE(gowj::SubtitleRegionWanted(true, 1));
  CHECK(gowj::SubtitleRegionWanted(true, 2));
}
