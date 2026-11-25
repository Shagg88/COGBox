// GitHub #420, Gears 2 side by side: the Longshot overlay (ue3/canvas_tile_clip.h, shared with Judgment) and the
// subtitle region patch (ue3/gow2_subtitle_region_package_patch.h).
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "ue3/canvas_tile_clip.h"
#include "ue3/gow2_subtitle_region_package_patch.h"
#include "ue3/gow2_toast_slide_package_patch.h"

TEST_CASE("gow2: a full-view material tile on a 640x720 view samples U 0.5, V 2, centred", "[split]") {
  const ue3::TileUV t = ue3::FitFullViewTile16x9(640.0f, 640.0f, 720.0f, 0.0f, 1.0f, 0.0f, 1.0f, 640.0f, 640.0f, 720.0f, 2.0f);
  CHECK(t.u == 0.25f);
  CHECK(t.ul == 0.5f);
  CHECK(t.v == -0.5f);
  CHECK(t.vl == 2.0f);
}

TEST_CASE("gow2: partial material tiles are left alone", "[split]") {
  const ue3::TileUV t = ue3::FitFullViewTile16x9(10.0f, 100.0f, 100.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 640.0f, 720.0f, 2.0f);
  CHECK(t.ul == 1.0f);
  CHECK(t.vl == 1.0f);
}

TEST_CASE("gow2: the subtitle patch is one same-length edit, only for two or more players side by side", "[split]") {
  const std::string s = gow2::kSubtitleRegionPackagePatch;
  const auto colon = s.find(':'), gt = s.find('>'), hash = s.find('#');
  REQUIRE(colon != std::string::npos);
  REQUIRE(gt != std::string::npos);
  REQUIRE(hash != std::string::npos);
  CHECK(gt - colon - 1 == hash - gt - 1);
  CHECK(s.find(';') == std::string::npos);
  CHECK(gow2::SubtitleRegionWanted(true, 2));
  CHECK_FALSE(gow2::SubtitleRegionWanted(true, 1));
  CHECK_FALSE(gow2::SubtitleRegionWanted(false, 2));
}

TEST_CASE("gow2: the toast patch moves 15 keyframes, each a same-length 4-byte edit, for two players side by side", "[split]") {
  const std::string s = gow2::kToastSlidePackagePatch;
  size_t n = 0, pos = 0;
  while (pos < s.size()) {
    size_t end = s.find(';', pos);
    if (end == std::string::npos) end = s.size();
    const std::string e = s.substr(pos, end - pos);
    const auto colon = e.find(':'), gt = e.find('>'), hash = e.find('#');
    REQUIRE(colon != std::string::npos);
    REQUIRE(gt != std::string::npos);
    REQUIRE(hash != std::string::npos);
    CHECK(gt - colon - 1 == 8);
    CHECK(hash - gt - 1 == 8);
    ++n;
    pos = end + 1;
  }
  CHECK(n == 15);
  CHECK(gow2::ToastSlideWanted(true, 2));
  CHECK_FALSE(gow2::ToastSlideWanted(true, 1));
  CHECK_FALSE(gow2::ToastSlideWanted(false, 2));
}
