// Unit tests for the screen-space AO camera maths (ao.h, notes/80): XeGTAO's constants from the game's view-projection
// XeGTAO ambient occlusion, upscaler motion-vector / depth fixes and SMAA T2x: PR #139 by gmihaic (https://github.com/gmihaic).
// and viewport. Game-free (one test uses numbers measured from Gears 2, notes/80 s.3). Our code (MIT).
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include "xenos/renderer/ao.h"

namespace ao = xenos::renderer::ao;
namespace up = xenos::renderer::upscale;
using Catch::Approx;

namespace {
// D3D-style left-handed perspective (row-major, clip = M x (x, y, z, 1)): w = view z, z/w in 0..1; zf <= 0: infinite.
up::Mat4 AoPerspective(float fov_y, float aspect, float zn, float zf) {
  const float ys = 1.0f / std::tan(fov_y * 0.5f), xs = ys / aspect;
  up::Mat4 p;
  p(0, 0) = xs;
  p(1, 1) = ys;
  p(2, 2) = zf > 0.0f ? zf / (zf - zn) : 1.0f;
  p(2, 3) = zf > 0.0f ? -zn * zf / (zf - zn) : -zn;
  p(3, 2) = 1.0f;
  p(3, 3) = 0.0f;
  return p;
}
// Projects a view-space point; returns (u, v) in 0..1 (v down) and the guest depth through `map`.
void AoProject(const up::Mat4& m, const ao::DepthMap& map, const float p[3], float& u, float& v, float& depth) {
  float clip[4];
  for (int r = 0; r < 4; ++r) clip[r] = m(r, 0) * p[0] + m(r, 1) * p[1] + m(r, 2) * p[2] + m(r, 3);
  u = (clip[0] / clip[3] + 1.0f) * 0.5f;
  v = (1.0f - clip[1] / clip[3]) * 0.5f;
  depth = map.scale * (clip[2] / clip[3]) + map.offset;
}
}  // namespace

TEST_CASE("ao: viewport depth map, normal and reversed", "[ao]") {
  xenos::renderer::ViewportState v;
  ao::DepthMap d = ao::ViewportDepthMap(v);
  CHECK(d.scale == 1.0f);
  CHECK(d.offset == 0.0f);
  // TranslateViewport's form of the 360's reversed range (ZSCALE -1, ZOFFSET 1, dx clip space): swapped to 0..1 with
  // the NDC z flipped.
  v.z_min = 0.0f;
  v.z_max = 1.0f;
  v.ndc_scale[2] = -1.0f;
  v.ndc_offset[2] = 1.0f;
  d = ao::ViewportDepthMap(v);
  CHECK(d.scale == -1.0f);
  CHECK(d.offset == 1.0f);
}

TEST_CASE("ao: view positions round-trip through the constants", "[ao]") {
  const ao::DepthMap maps[] = {{1.0f, 0.0f}, {-1.0f, 1.0f}};
  const float aspects[] = {16.0f / 9.0f, 2.3333f};  // 16:9 and the ultrawide view
  const float points[][3] = {{0.0f, 0.0f, 50.0f}, {-30.0f, 12.0f, 120.0f}, {200.0f, -90.0f, 900.0f}, {1.0f, 1.0f, 11.0f}};
  for (const ao::DepthMap& map : maps)
    for (float aspect : aspects)
      for (float zf : {0.0f, 20000.0f}) {
        const up::Mat4 m = AoPerspective(1.1f, aspect, 10.0f, zf);
        ao::GtaoConstants c;
        REQUIRE(ao::MakeGtaoConstants(m, map, 2560, uint32_t(2560 / aspect), c));
        CHECK(c.near_plane == Approx(10.0f).epsilon(1e-4));
        CHECK(c.tan_half_fov[1] == Approx(std::tan(0.55f)).epsilon(1e-5));
        CHECK(c.tan_half_fov[0] / c.tan_half_fov[1] == Approx(aspect).epsilon(1e-5));
        for (const auto& p : points) {
          float u, v, d, q[3];
          AoProject(m, map, p, u, v, d);
          ao::ViewPosition(c, u, v, d, q);
          CHECK(q[2] == Approx(p[2]).epsilon(1e-3));
          CHECK(q[0] == Approx(p[0]).epsilon(1e-3).margin(1e-3));
          CHECK(q[1] == Approx(p[1]).epsilon(1e-3).margin(1e-3));
        }
      }
}

TEST_CASE("ao: Gears 2's measured camera (notes/80 s.3)", "[ao]") {
  // Ch2 frame 1500, c0-c3 as the game stores them (columns of clip = x*c0 + y*c1 + z*c2 + c3), reversed-Z viewport.
  const float c[16] = {0.067059f, 0.0f, -0.996321f, -0.997319f, -0.913874f, 0.0f, -0.0731089f, -0.0731821f,
                       0.0f, 1.62903f, 0.0f, 0.0f, 0.0f, -2.48571e-05f, -9.99195f, -0.00195312f};
  const up::Mat4 m = up::FromConstantsTransposed(c, 0);
  ao::GtaoConstants k;
  REQUIRE(ao::MakeGtaoConstants(m, {-1.0f, 1.0f}, 1280, 720, k));
  CHECK(k.near_plane == Approx(10.0f).epsilon(1e-3));
  CHECK(k.tan_half_fov[0] / k.tan_half_fov[1] == Approx(16.0f / 9.0f).epsilon(1e-3));
  // Depth / the game's own linear depth (copy 6, y) pairs read from the same frame's buffer dump.
  const float pairs[][2] = {{0.010994f, 999.5f}, {0.016098f, 662.0f}, {0.023041f, 453.75f}, {0.066833f, 151.75f}};
  for (const auto& p : pairs) CHECK(ao::ViewDepth(k, p[0]) == Approx(p[1]).epsilon(5e-3));
}

TEST_CASE("ao: a planar-reflection camera is not the scene camera (Gears 3 Raven's Nest, notes/80 s.3)", "[ao]") {
  // The first camera Gears 3 draws into the scene depth there, c0-c3 as logged: perspective along -y, but its depth row
  // is a world plane (z = 142, an oblique near plane), not the distance. Refused - the renderer then takes the next
  // camera (the view). Gears 2's measured camera above passes the same test.
  const float c[16] = {1.19175f, 3.70441e-07f, -1.79805e-07f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f,
                       2.08373e-07f, -2.11867f, 2.05673f, 0.0f, 0.0f, -0.000116098f, -292.056f, 0.0f};
  ao::GtaoConstants k;
  CHECK_FALSE(ao::MakeGtaoConstants(up::FromConstantsTransposed(c, 0), {-1.0f, 1.0f}, 1280, 720, k));
}

TEST_CASE("ao: matrices without a perspective divide are refused", "[ao]") {
  up::Mat4 ortho;  // identity: clip w = 1 for every point (full-screen passes, UI)
  ao::GtaoConstants c;
  CHECK_FALSE(ao::MakeGtaoConstants(ortho, {1.0f, 0.0f}, 1280, 720, c));
  CHECK_FALSE(ao::MakeGtaoConstants(AoPerspective(1.0f, 1.5f, 10.0f, 0.0f), {0.0f, 0.5f}, 1280, 720, c));  // no depth range
  CHECK_FALSE(ao::MakeGtaoConstants(AoPerspective(1.0f, 1.5f, 10.0f, 0.0f), {1.0f, 0.0f}, 0, 720, c));
}

TEST_CASE("ao: mode names", "[ao]") {
  CHECK(ao::ParseMode("gtao") == ao::Mode::kGtao);
  CHECK(ao::ParseMode("off") == ao::Mode::kOff);
  CHECK(ao::ParseMode("game") == ao::Mode::kGame);
  CHECK(ao::ParseMode("") == ao::Mode::kGame);
  CHECK(ao::ParseMode("hbao") == ao::Mode::kGame);
}
