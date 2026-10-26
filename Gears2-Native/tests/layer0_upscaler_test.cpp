// Unit tests for the SDK-independent upscaler pieces (upscaler.h, notes/64): the jitter sequence and the camera motion
// the motion-vector pass computes. Game-free. Our code (MIT).
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <vector>
#include <cstdint>

#include "xenos/renderer/upscaler.h"

namespace up = xenos::renderer::upscale;
using Catch::Approx;

namespace {
// D3D-style left-handed perspective (row-major, clip = M x (x, y, z, 1)): w = view z, z/w in 0..1.
up::Mat4 Perspective(float fov_y, float aspect, float zn, float zf) {
  const float ys = 1.0f / std::tan(fov_y * 0.5f), xs = ys / aspect;
  up::Mat4 p;
  p(0, 0) = xs;
  p(1, 1) = ys;
  p(2, 2) = zf / (zf - zn);
  p(2, 3) = -zn * zf / (zf - zn);
  p(3, 2) = 1.0f;
  p(3, 3) = 0.0f;
  return p;
}
// View for a camera at (cx, cy, cz) turned by `yaw` radians about +y (looking down +z at yaw 0).
up::Mat4 View(float cx, float cy, float cz, float yaw) {
  up::Mat4 t;
  t(0, 3) = -cx;
  t(1, 3) = -cy;
  t(2, 3) = -cz;
  up::Mat4 r;
  r(0, 0) = std::cos(yaw);
  r(0, 2) = -std::sin(yaw);
  r(2, 0) = std::sin(yaw);
  r(2, 2) = std::cos(yaw);
  return up::Multiply(r, t);
}
// World point -> (u, v, depth) as the rasteriser sees it (u right, v down).
bool Project(const up::Mat4& vp, float x, float y, float z, float& u, float& v, float& d) {
  const float in[4] = {x, y, z, 1.0f};
  float p[4];
  for (int r = 0; r < 4; ++r) p[r] = vp(r, 0) * in[0] + vp(r, 1) * in[1] + vp(r, 2) * in[2] + vp(r, 3) * in[3];
  if (p[3] <= 0.0f) return false;
  u = (p[0] / p[3] + 1.0f) * 0.5f;
  v = (1.0f - p[1] / p[3]) * 0.5f;
  d = p[2] / p[3];
  return true;
}
}  // namespace

TEST_CASE("Halton sequence and jitter", "[upscaler]") {
  CHECK(up::Halton(1, 2) == Approx(0.5f));
  CHECK(up::Halton(2, 2) == Approx(0.25f));
  CHECK(up::Halton(3, 2) == Approx(0.75f));
  CHECK(up::Halton(1, 3) == Approx(1.0f / 3.0f));
  CHECK(up::Halton(2, 3) == Approx(2.0f / 3.0f));
  CHECK(up::Halton(4, 3) == Approx(1.0f / 3.0f + 1.0f / 9.0f));
  CHECK(up::JitterPhaseCount(1280, 1280) == 8);
  CHECK(up::JitterPhaseCount(1280, 2560) == 32);
  CHECK(up::JitterPhaseCount(1706, 2560) == 19);  // 1.5x: 8 * 2.2518 = 18.01 -> 19
  for (uint64_t f = 0; f < 64; ++f) {
    const up::Jitter j = up::JitterOffset(f, 8);
    CHECK(j.x > -0.5f);
    CHECK(j.x < 0.5f);
    CHECK(j.y > -0.5f);
    CHECK(j.y < 0.5f);
  }
  CHECK(up::JitterOffset(0, 8).x == Approx(0.0f));            // Halton(1, 2) - 0.5
  CHECK(up::JitterOffset(8, 8).y == up::JitterOffset(0, 8).y);  // repeats after the phase count
}

TEST_CASE("matrix inverse", "[upscaler]") {
  const up::Mat4 vp = up::Multiply(Perspective(1.0f, 16.0f / 9.0f, 10.0f, 50000.0f), View(120.0f, 40.0f, -300.0f, 0.3f));
  up::Mat4 inv;
  REQUIRE(up::Invert(vp, inv));
  const up::Mat4 id = up::Multiply(vp, inv);
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) CHECK(id(r, c) == Approx(r == c ? 1.0f : 0.0f).margin(1e-4));
  up::Mat4 zero;
  for (float& x : zero.m) x = 0.0f;
  CHECK_FALSE(up::Invert(zero, inv));
}

TEST_CASE("constants as matrix rows", "[upscaler]") {
  float c[4 * 8] = {};
  for (int i = 0; i < 32; ++i) c[i] = float(i);
  const up::Mat4 m = up::FromConstants(c, 4);  // c4..c7
  CHECK(m(0, 0) == 16.0f);
  CHECK(m(1, 2) == 22.0f);
  CHECK(m(3, 3) == 31.0f);
}

TEST_CASE("camera motion: a still camera gives no motion", "[upscaler]") {
  const up::Mat4 vp = up::Multiply(Perspective(1.0f, 16.0f / 9.0f, 10.0f, 50000.0f), View(0, 100, 0, 0.2f));
  up::Mat4 rep;
  REQUIRE(up::ReprojectionMatrix(vp, vp, 1.0f, rep));
  for (float u : {0.1f, 0.5f, 0.93f})
    for (float d : {0.2f, 0.9f, 0.9999f}) {
      float mx = 1, my = 1;
      CHECK(up::CameraMotion(rep, u, 0.37f, d, 1280, 720, mx, my));
      CHECK(mx == Approx(0.0f).margin(1e-2));
      CHECK(my == Approx(0.0f).margin(1e-2));
    }
}

TEST_CASE("camera motion matches projecting the point with last frame's camera", "[upscaler]") {
  const up::Mat4 proj = Perspective(1.0f, 16.0f / 9.0f, 10.0f, 50000.0f);
  const up::Mat4 cur = up::Multiply(proj, View(10.0f, 100.0f, 0.0f, 0.05f));
  const up::Mat4 prev = up::Multiply(proj, View(0.0f, 100.0f, -20.0f, 0.0f));  // moved right and forward, turned
  up::Mat4 rep;
  REQUIRE(up::ReprojectionMatrix(cur, prev, 1.0f, rep));
  const float pts[][3] = {{0, 100, 500}, {150, 60, 900}, {-300, 180, 2500}, {40, 90, 120}};
  for (const auto& p : pts) {
    float u, v, d, pu, pv, pd;
    REQUIRE(Project(cur, p[0], p[1], p[2], u, v, d));
    REQUIRE(Project(prev, p[0], p[1], p[2], pu, pv, pd));
    float mx, my;
    REQUIRE(up::CameraMotion(rep, u, v, d, 1280, 720, mx, my));
    CHECK(mx == Approx((pu - u) * 1280.0f).margin(0.05));
    CHECK(my == Approx((pv - v) * 720.0f).margin(0.05));
  }
  // The camera moved right: a point straight ahead was further right last frame (positive motion).
  float u, v, d, mx, my;
  REQUIRE(Project(cur, 10.0f, 100.0f, 1000.0f, u, v, d));
  REQUIRE(up::CameraMotion(rep, u, v, d, 1280, 720, mx, my));
  CHECK(mx > 0.0f);
}

TEST_CASE("camera motion with a depth remap (host stores guest depth halved)", "[upscaler]") {
  const up::Mat4 proj = Perspective(1.0f, 16.0f / 9.0f, 10.0f, 50000.0f);
  const up::Mat4 cur = up::Multiply(proj, View(0, 0, 0, 0.1f));
  const up::Mat4 prev = up::Multiply(proj, View(0, 0, 0, 0.0f));
  up::Mat4 rep, rep_half;
  REQUIRE(up::ReprojectionMatrix(cur, prev, 1.0f, rep));
  REQUIRE(up::ReprojectionMatrix(cur, prev, 2.0f, rep_half));
  float a, b, c, e;
  REQUIRE(up::CameraMotion(rep, 0.3f, 0.6f, 0.98f, 1280, 720, a, b));
  REQUIRE(up::CameraMotion(rep_half, 0.3f, 0.6f, 0.49f, 1280, 720, c, e));
  CHECK(a == Approx(c).margin(1e-3));
  CHECK(b == Approx(e).margin(1e-3));
}

TEST_CASE("names and ratios", "[upscaler]") {
  CHECK(up::ParseKind("dlss") == up::Kind::kDlss);
  CHECK(up::ParseKind("fsr") == up::Kind::kFsr);
  CHECK(up::ParseKind("xess") == up::Kind::kXess);
  CHECK(up::ParseKind("smaa") == up::Kind::kNone);
  CHECK(std::string(up::KindName(up::Kind::kXess)) == "XeSS");
  CHECK_FALSE(up::BuiltIn(up::Kind::kXess));  // the test build compiles no SDK in
  CHECK(up::QualityRatio(up::Quality::kNativeAA) == 1.0f);
  CHECK(up::QualityRatio(up::Quality::kPerformance) == 2.0f);
}

TEST_CASE("UE3's camera constants (job 252, ch02 frame 600): transposed, fov, near, infinite far", "[upscaler]") {
  const float c[16] = {0.087215f, 0, -0.996321f, -0.997319f,  -1.18856f, 0, -0.0731089f, -0.0731821f,
                       0, 2.11867f, 0, 0,                      0, 0, -9.99195f, -0.00195312f};
  const up::Mat4 m = up::FromConstantsTransposed(c, 0);
  CHECK(m(1, 2) == 2.11867f);  // clip y = 2.11867 * z (UE3 z up)
  CHECK(m(3, 0) == -0.997319f);
  float fov = 0, zn = 0;
  bool inf = false;
  REQUIRE(up::PerspectiveParams(m, fov, zn, inf));
  CHECK(fov == Approx(2.0 * std::atan(1.0 / 2.11867)).epsilon(1e-3));
  CHECK(zn == Approx(9.99).epsilon(1e-3));
  CHECK(inf);
  // A point 1000 units ahead of the camera along its view axis is inside the frustum, depth near 0.989.
  const float fx = 0.997319f, fy = 0.0731821f;  // forward = -(w row) direction... w = -(0.997 x + 0.073 y)
  float u, v, d;
  const float in[4] = {-fx * 1000.0f, -fy * 1000.0f, 0.0f, 1.0f};
  float p[4];
  for (int r = 0; r < 4; ++r) p[r] = m(r, 0) * in[0] + m(r, 1) * in[1] + m(r, 2) * in[2] + m(r, 3) * in[3];
  REQUIRE(p[3] > 0.0f);
  u = (p[0] / p[3] + 1.0f) * 0.5f;
  v = (1.0f - p[1] / p[3]) * 0.5f;
  d = p[2] / p[3];
  CHECK(u == Approx(0.5f).margin(0.01));
  CHECK(v == Approx(0.5f).margin(0.01));
  CHECK(d == Approx(0.989f).margin(0.002));
}

TEST_CASE("camera motion when rendering camera-relative (UE3): translation passed separately", "[upscaler]") {
  const up::Mat4 proj = Perspective(1.0f, 16.0f / 9.0f, 10.0f, 50000.0f);
  const float cam_now[3] = {130.0f, 100.0f, 40.0f}, cam_then[3] = {100.0f, 100.0f, 0.0f};
  const up::Mat4 cur = up::Multiply(proj, View(0, 0, 0, 0.05f));  // rotation only: positions are camera-relative
  const up::Mat4 prev = up::Multiply(proj, View(0, 0, 0, 0.0f));
  const float move[3] = {cam_now[0] - cam_then[0], cam_now[1] - cam_then[1], cam_now[2] - cam_then[2]};
  up::Mat4 rep, rep_rot_only;
  REQUIRE(up::ReprojectionMatrix(cur, prev, move, 1.0f, rep));
  REQUIRE(up::ReprojectionMatrix(cur, prev, 1.0f, rep_rot_only));
  const float pts[][3] = {{150, 100, 600}, {300, 60, 1500}, {-200, 180, 3000}, {120, 90, 200}};
  float worst_rot_only = 0.0f;
  for (const auto& p : pts) {
    float u, v, d, pu, pv, pd;
    REQUIRE(Project(cur, p[0] - cam_now[0], p[1] - cam_now[1], p[2] - cam_now[2], u, v, d));
    REQUIRE(Project(prev, p[0] - cam_then[0], p[1] - cam_then[1], p[2] - cam_then[2], pu, pv, pd));
    float mx, my;
    REQUIRE(up::CameraMotion(rep, u, v, d, 1280, 720, mx, my));
    CHECK(mx == Approx((pu - u) * 1280.0f).margin(0.05));
    CHECK(my == Approx((pv - v) * 720.0f).margin(0.05));
    REQUIRE(up::CameraMotion(rep_rot_only, u, v, d, 1280, 720, mx, my));
    worst_rot_only = std::max(worst_rot_only, std::fabs(mx - (pu - u) * 1280.0f));
  }
  CHECK(worst_rot_only > 5.0f);  // without the translation the near points are off by many pixels
}

TEST_CASE("camera move voted from objects' camera-relative translations", "[upscaler]") {
  const float move[3] = {-2.009f, -0.033f, -3.132f};  // job 253 probe, ch02
  std::vector<up::ObjectSample> prev, cur;
  for (uint64_t k = 1; k <= 10; ++k) {  // static objects: translation = position - camera
    const float pos[3] = {1000.0f * float(k), -500.0f + 37.0f * float(k), 3000.0f - 211.0f * float(k)};
    up::ObjectSample a{k, {pos[0], pos[1], pos[2]}}, b{k, {pos[0] - move[0], pos[1] - move[1], pos[2] - move[2]}};
    prev.push_back(a);
    cur.push_back(b);
  }
  for (uint64_t k = 100; k < 103; ++k) {  // three moving characters
    prev.push_back({k, {0, 0, 0}});
    cur.push_back({k, {float(k), 5.0f, -9.0f}});
  }
  prev.push_back({500, {1, 1, 1}});  // a key drawn twice this frame (e.g. instances): ignored
  cur.push_back({500, {7, 7, 7}});
  cur.push_back({500, {9, 9, 9}});
  cur.push_back({777, {1, 2, 3}});   // new this frame: unmatched
  float got[3];
  uint32_t votes = 0, matched = 0;
  REQUIRE(up::CameraMoveFromObjects(prev, cur, got, &votes, &matched));
  CHECK(votes == 10);
  CHECK(matched == 13);
  for (int i = 0; i < 3; ++i) CHECK(got[i] == Approx(move[i]).margin(1e-3));
  // A still camera with nothing moving: zero, found.
  REQUIRE(up::CameraMoveFromObjects(prev, prev, got, &votes, &matched));
  CHECK(got[0] == 0.0f);
  // Too few objects agree: not found.
  std::vector<up::ObjectSample> p2(prev.begin(), prev.begin() + 2), c2(cur.begin(), cur.begin() + 2);
  CHECK_FALSE(up::CameraMoveFromObjects(p2, c2, got));
  CHECK_FALSE(up::CameraMoveFromObjects({}, cur, got));
}

// De-jittered depth resolves (Gears 3 job 058, --gow2_upscale_dejitter_depth; kDepthResolveCs does the same
// arithmetic). Jittered texel k holds the surface at k + 0.5 - j; the resolve writes, for texel q, the depth at q + j.
TEST_CASE("Dejittered depth: a plane comes back unshifted, an edge takes the nearest tap", "[upscaler]") {
  // A plane's depth (a - b / w) is linear in screen space, so the bilinear resample is exact on it.
  const auto z = [](float x) { return 0.95f + 0.0004f * x; };
  for (const float j : {0.25f, -0.25f, 0.4f, -0.45f}) {
    for (int q = 2; q < 8; ++q) {
      const float p = float(q) + j;
      const int i0 = int(std::floor(p));
      const float f = p - float(i0);
      const float taps[4] = {z(i0 + 0.5f - j), z(i0 + 1.5f - j), z(i0 + 0.5f - j), z(i0 + 1.5f - j)};
      int tap = 9;
      CHECK(up::DejitterDepth(taps, f, 0.0f, 1.0f, 0.05f, &tap) == Approx(z(q + 0.5f)).margin(1e-6));
      CHECK(tap == -1);
    }
  }
  // A step from a near surface (w ~ 10) to a far one (w ~ 1000): never an in-between depth.
  const float edge[4] = {0.90f, 0.999f, 0.90f, 0.999f};
  int tap = 9;
  CHECK(up::DejitterDepth(edge, 0.25f, 0.0f, 1.0f, 0.05f, &tap) == 0.90f);
  CHECK(tap == 0);
  CHECK(up::DejitterDepth(edge, 0.75f, 0.6f, 1.0f, 0.05f, &tap) == 0.999f);
  CHECK(tap == 3);
  // No shift (fraction 0): the texel itself.
  const float flat[4] = {0.97f, 0.971f, 0.972f, 0.973f};
  CHECK(up::DejitterDepth(flat, 0.0f, 0.0f, 1.0f, 0.05f) == 0.97f);
}

TEST_CASE("Depth at infinity of a perspective matrix", "[upscaler]") {
  // Gears 3 ch8's camera (job 058 probe, c0-c3 as UE3 passes them): depth = 0.999 - b / w.
  const float c[16] = {-1.69415f, -0.0654293f, 0.61226f, 0.612873f, 1.31483f, -0.0843055f, 0.788896f, 0.789686f,
                       0.0f, 3.81097f, 0.0279635f, 0.0279915f, -0.00837698f, -0.000465388f, -9.9861f, 0.00390625f};
  CHECK(up::DepthAtInfinity(up::FromConstantsTransposed(c, 0)) == Approx(0.999f).margin(1e-4));
  // A D3D-style perspective with a far plane: zf / (zf - zn).
  CHECK(up::DepthAtInfinity(Perspective(1.0f, 16.0f / 9.0f, 10.0f, 10000.0f)) == Approx(10000.0f / 9990.0f).margin(1e-5));
  // Not a perspective matrix: 1.
  CHECK(up::DepthAtInfinity(up::Mat4{}) == 1.0f);
}
