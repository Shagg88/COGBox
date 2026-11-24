// GitHub #420, Gears 3 side by side: the zoom overlay (ue3/canvas_tile_clip.h, shared with Gears 2 / Judgment) and the
// subtitle region patch (gow3_subtitle_region_package_patch.h).
#include <catch2/catch_test_macros.hpp>

#include <string>

#include <ue3/canvas_tile_clip.h>
#include "../src/layer3_gow3/gow3_subtitle_region_package_patch.h"

TEST_CASE("gow3: a full-view material tile on a 640x720 view samples U 0.5, V 2, centred", "[split]") {
  const ue3::TileUV t = ue3::FitFullViewTile16x9(0.0f, 640.0f, 720.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 640.0f, 720.0f, 2.0f);
  CHECK(t.u == 0.25f);
  CHECK(t.ul == 0.5f);
  CHECK(t.v == -0.5f);
  CHECK(t.vl == 2.0f);
}

TEST_CASE("gow3: the subtitle patch is one same-length edit, for two or more players side by side", "[split]") {
  const std::string s = gow3::kSubtitleRegionPackagePatch;
  const auto colon = s.find(':'), gt = s.find('>'), hash = s.find('#');
  REQUIRE(colon != std::string::npos);
  REQUIRE(gt != std::string::npos);
  REQUIRE(hash != std::string::npos);
  CHECK(gt - colon - 1 == hash - gt - 1);
  CHECK(gow3::SubtitleRegionWanted(true, 2));
  CHECK_FALSE(gow3::SubtitleRegionWanted(true, 1));
  CHECK_FALSE(gow3::SubtitleRegionWanted(false, 2));
}
