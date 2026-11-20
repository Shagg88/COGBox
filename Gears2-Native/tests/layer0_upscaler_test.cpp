// Unit tests for the SDK-independent upscaler pieces (upscaler.h, notes/64): the jitter sequence and the camera motion
// the motion-vector pass computes. Game-free. Our code (MIT).
#include <initializer_list>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <vector>
#include <cstdint>
#include <utility>

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
  CHECK(up::ParseKind("xess") == up::Kind::kNone);  // dropped in 0.6.6
  CHECK(up::ParseKind("smaa") == up::Kind::kNone);
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

TEST_CASE("camera motion with a reversed-Z viewport (Gears 2, notes/80 s.6)", "[upscaler]") {
  // The same camera move, but the depth the motion pass reads is the viewport's 1 - z/w (Gears 2). With the viewport's
  // depth map passed in, the motion matches the exact projection; read as plain z/w (the old code), it is far off.
  const up::Mat4 proj = Perspective(1.0f, 16.0f / 9.0f, 10.0f, 50000.0f);
  const float cam_now[3] = {130.0f, 100.0f, 40.0f}, cam_then[3] = {100.0f, 100.0f, 0.0f};
  const up::Mat4 cur = up::Multiply(proj, View(0, 0, 0, 0.05f));
  const up::Mat4 prev = up::Multiply(proj, View(0, 0, 0, 0.0f));
  const float move[3] = {cam_now[0] - cam_then[0], cam_now[1] - cam_then[1], cam_now[2] - cam_then[2]};
  up::Mat4 rep, rep_unmapped;
  REQUIRE(up::ReprojectionMatrix(cur, prev, move, 1.0f, rep, -1.0f, 1.0f));
  REQUIRE(up::ReprojectionMatrix(cur, prev, move, 1.0f, rep_unmapped));
  const float pts[][3] = {{150, 100, 600}, {300, 60, 1500}, {-200, 180, 3000}, {120, 90, 200}};
  float worst_unmapped = 0.0f;
  for (const auto& p : pts) {
    float u, v, d, pu, pv, pd;
    REQUIRE(Project(cur, p[0] - cam_now[0], p[1] - cam_now[1], p[2] - cam_now[2], u, v, d));
    REQUIRE(Project(prev, p[0] - cam_then[0], p[1] - cam_then[1], p[2] - cam_then[2], pu, pv, pd));
    const float reversed = 1.0f - d;  // what Gears 2's depth buffer holds
    float mx, my;
    REQUIRE(up::CameraMotion(rep, u, v, reversed, 1280, 720, mx, my));
    CHECK(mx == Approx((pu - u) * 1280.0f).margin(0.05));
    CHECK(my == Approx((pv - v) * 720.0f).margin(0.05));
    if (up::CameraMotion(rep_unmapped, u, v, reversed, 1280, 720, mx, my))
      worst_unmapped = std::max(worst_unmapped, std::fabs(mx - (pu - u) * 1280.0f));
    else
      worst_unmapped = 1e9f;
  }
  CHECK(worst_unmapped > 20.0f);  // the bug: reversed depth read as plain z/w
  // The plain 0..1 map (1 / 0) is the old behaviour exactly.
  up::Mat4 rep_plain;
  REQUIRE(up::ReprojectionMatrix(cur, prev, move, 1.0f, rep_plain, 1.0f, 0.0f));
  for (int i = 0; i < 16; ++i) CHECK(rep_plain.m[i] == Approx(rep_unmapped.m[i]));
}

TEST_CASE("characters move with their own transforms (Gears 2 velocity pass, notes/80 s.6)", "[upscaler]") {
  // The camera strafes and turns; the player's character moves with it (camera-relative pose unchanged), another
  // stands still in the world. Object rows as UE3 gives them: now relative to the camera, then in absolute world.
  const up::Mat4 proj = Perspective(1.0f, 16.0f / 9.0f, 10.0f, 50000.0f);
  const float cam_then[3] = {20000.0f, -10000.0f, 150.0f}, cam_now[3] = {20009.0f, -10000.0f, 150.0f};
  const up::Mat4 cur = up::Multiply(proj, View(0, 0, 0, 0.03f)), prev = up::Multiply(proj, View(0, 0, 0, 0.0f));
  auto at = [](float x, float y, float z, float yaw) {
    up::Mat4 m;
    m(0, 0) = std::cos(yaw), m(0, 2) = std::sin(yaw), m(2, 0) = -std::sin(yaw), m(2, 2) = std::cos(yaw);
    m(0, 3) = x, m(1, 3) = y, m(2, 3) = z;
    return m;
  };
  up::MovingObject player_then, player_now, npc_then, npc_now;
  player_then.key = player_now.key = 1;
  npc_then.key = npc_now.key = 2;
  // The player: 200 ahead of the camera both frames (in the camera's frame, which turned by 0.03).
  player_then.now = at(0, -90, 200, 0.0f);
  player_then.then = at(cam_then[0] - 9, cam_then[1] - 90, cam_then[2] + 200, 0.0f);  // the frame before
  const up::Mat4 turn = View(0, 0, 0, -0.03f);  // camera-relative -> world axes of this frame
  player_now.now = up::Multiply(turn, at(0, -90, 200, 0.0f));
  player_now.then = at(cam_then[0], cam_then[1] - 90, cam_then[2] + 200, 0.0f);
  // The NPC: fixed in the world.
  const float npc[3] = {20100.0f, -10050.0f, 900.0f};
  npc_then.now = at(npc[0] - cam_then[0], npc[1] - cam_then[1], npc[2] - cam_then[2], 0.4f);
  npc_then.then = at(npc[0], npc[1], npc[2], 0.4f);
  npc_now.now = at(npc[0] - cam_now[0], npc[1] - cam_now[1], npc[2] - cam_now[2], 0.4f);
  npc_now.then = at(npc[0], npc[1], npc[2], 0.4f);
  // Last frame's camera from the two frames' objects (both agree).
  float cam[3];
  REQUIRE(up::PreviousCameraFromObjects({player_then, npc_then}, {player_now, npc_now}, cam));
  CHECK(cam[0] == Approx(cam_then[0]));
  CHECK(cam[1] == Approx(cam_then[1]));
  CHECK(cam[2] == Approx(cam_then[2]));
  CHECK_FALSE(up::PreviousCameraFromObjects({npc_then}, {player_now}, cam));  // nothing matched
  const float move[3] = {cam_now[0] - cam_then[0], cam_now[1] - cam_then[1], cam_now[2] - cam_then[2]};
  up::Mat4 rep_player, rep_npc, rep_camera;
  REQUIRE(up::ObjectReprojectionMatrix(cur, prev, player_now, cam_then, 1.0f, rep_player, -1.0f, 1.0f));
  REQUIRE(up::ObjectReprojectionMatrix(cur, prev, npc_now, cam_then, 1.0f, rep_npc, -1.0f, 1.0f));
  REQUIRE(up::ReprojectionMatrix(cur, prev, move, 1.0f, rep_camera, -1.0f, 1.0f));
  auto motion = [&](const up::Mat4& rep, const up::Mat4& obj_now, float lx, float ly, float lz, float& mx, float& my) {
    const float p[3] = {obj_now(0, 0) * lx + obj_now(0, 1) * ly + obj_now(0, 2) * lz + obj_now(0, 3),
                        obj_now(1, 0) * lx + obj_now(1, 1) * ly + obj_now(1, 2) * lz + obj_now(1, 3),
                        obj_now(2, 0) * lx + obj_now(2, 1) * ly + obj_now(2, 2) * lz + obj_now(2, 3)};
    float u, v, d;
    REQUIRE(Project(cur, p[0], p[1], p[2], u, v, d));
    return up::CameraMotion(rep, u, v, 1.0f - d, 1280, 720, mx, my);
  };
  float mx, my;
  // A point on the player: on screen where it was (the camera follows him), whatever the camera did.
  REQUIRE(motion(rep_player, player_now.now, 20, 60, 0, mx, my));
  CHECK(mx == Approx(0.0f).margin(0.05));
  CHECK(my == Approx(0.0f).margin(0.05));
  // The camera-only vectors were wrong on him by many pixels.
  REQUIRE(motion(rep_camera, player_now.now, 20, 60, 0, mx, my));
  CHECK(std::fabs(mx) > 20.0f);
  // A point on the NPC: its own transform gives the camera's motion (it stands still in the world).
  float cx, cy;
  REQUIRE(motion(rep_npc, npc_now.now, 30, 40, 10, mx, my));
  REQUIRE(motion(rep_camera, npc_now.now, 30, 40, 10, cx, cy));
  CHECK(mx == Approx(cx).margin(0.05));
  CHECK(my == Approx(cy).margin(0.05));
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

TEST_CASE("upscale: the frame's last scene copy is learned (notes/80 s.11)", "[upscale]") {
  namespace up = xenos::renderer::upscale;
  // Which copy of a frame with n scene copies is taken first: as the renderer asks, stopping after the first taken.
  const auto frame = [](up::SceneCopyPicker& p, int n) {
    int taken = -1;
    for (int i = 0; i < n; ++i)
      if (p.Take() && taken < 0) taken = i;
    p.EndFrame();
    return taken;
  };
  up::SceneCopyPicker first;  // off: always the first
  CHECK(frame(first, 4) == 0);
  CHECK(frame(first, 4) == 0);
  up::SceneCopyPicker last;
  last.last_only = true;
  CHECK(frame(last, 4) == 0);   // nothing learned yet
  CHECK(frame(last, 4) == 3);   // every copy was counted, also those after the one taken
  CHECK(frame(last, 4) == 3);
  CHECK(frame(last, 0) == -1);  // a movie frame keeps the count
  CHECK(frame(last, 4) == 3);
  CHECK(frame(last, 5) == 3);   // more copies: one frame early
  CHECK(frame(last, 5) == 4);
  CHECK(frame(last, 2) == -1);  // fewer: none this frame
  CHECK(frame(last, 2) == 1);
  CHECK(frame(last, 1) == -1);
  CHECK(frame(last, 1) == 0);
}

TEST_CASE("upscale: the last scene copy keeps its format (Judgment Blood Drive: DLSS re-created every other frame)", "[upscale]") {
  namespace up = xenos::renderer::upscale;
  // A frame's scene copies by format; returns the format of the copy taken first (0 = none), as the renderer asks.
  const auto frame = [](up::SceneCopyPicker& p, std::initializer_list<uint32_t> formats) {
    uint32_t taken = 0;
    for (uint32_t f : formats)
      if (p.Take(f) && !taken) taken = f;
    p.EndFrame();
    return taken;
  };
  constexpr uint32_t kF16 = 1, kR10 = 2;   // a mid-frame 16-bit float copy, the finished 10:10:10:2 scene
  up::SceneCopyPicker p;
  p.last_only = true;
  CHECK(frame(p, {kF16, kR10}) == kF16);   // nothing learned: the first, as before
  CHECK(frame(p, {kF16, kR10}) == kR10);   // the last: its format is the scene's from now on
  // Blood Drive: frames with and without the mid-frame copy alternate. Counting every copy took the first (F16) after a
  // one-copy frame, so the format - DLSS's HDR flag - flipped and DLSS was re-created (~33 ms) every other frame.
  for (int i = 0; i < 6; ++i) {
    CHECK(frame(p, {kR10}) == kR10);
    CHECK(frame(p, {kF16, kR10}) == kR10);
  }
  // A real change of the scene's format (only the other one, for a while) is followed.
  for (int i = 0; i < 60; ++i) frame(p, {kF16});
  CHECK(frame(p, {kF16}) == kF16);
  // Off (Gears 2 / 3): format plays no part.
  up::SceneCopyPicker first;
  CHECK(frame(first, {kF16, kR10}) == kF16);
  CHECK(frame(first, {kR10}) == kR10);
}

TEST_CASE("upscale: a scene copy format learned from a menu frame is learned again in play", "[upscale]") {
  namespace up = xenos::renderer::upscale;
  const auto frame = [](up::SceneCopyPicker& p, std::initializer_list<uint32_t> formats) {
    uint32_t taken = 0;
    for (uint32_t f : formats)
      if (p.Take(f) && !taken) taken = f;
    p.EndFrame();
    return taken;
  };
  constexpr uint32_t kF16 = 1, kR10 = 2;
  up::SceneCopyPicker p;
  p.last_only = true;
  // the first frames have only the mid-frame copy (a menu, a transition): its format is learned
  frame(p, {kF16});
  frame(p, {kF16});
  // play: every frame still has a copy of that format, then the finished scene. For a while the old choice holds
  // (no rebuild on a one-off frame) ...
  for (int i = 0; i < 59; ++i) CHECK(frame(p, {kF16, kR10}) == kF16);
  // ... then, kFormatPatience frames in a row ending in the other format, it learns again: the finished scene from now on
  frame(p, {kF16, kR10});
  frame(p, {kF16, kR10});
  for (int i = 0; i < 100; ++i) CHECK(frame(p, {kF16, kR10}) == kR10);
  // and the alternating Blood Drive frames still keep the finished scene
  for (int i = 0; i < 100; ++i) CHECK(frame(p, i % 2 ? std::initializer_list<uint32_t>{kR10} : std::initializer_list<uint32_t>{kF16, kR10}) == kR10);
  // a frame with no scene copies at all changes nothing
  frame(p, {});
  CHECK(frame(p, {kF16, kR10}) == kR10);
}

TEST_CASE("upscale: the last scene copy is found in a frame with more than 16 copies", "[upscale]") {
  namespace up = xenos::renderer::upscale;
  up::SceneCopyPicker p;
  p.last_only = true;
  for (int f = 0; f < 3; ++f) {
    int taken = -1;
    for (int i = 0; i < 20; ++i)
      if (p.Take(2) && taken < 0) taken = i;
    p.EndFrame();
    if (f > 0) CHECK(taken == 19);   // the first frame learns (it takes the first, as before)
  }
}

TEST_CASE("upscale: one creation size for a scene that changes width (GitHub #323)", "[upscale]") {
  namespace up = xenos::renderer::upscale;
  uint32_t cw = 0, ch = 0;
  CHECK(up::FitCreationSize(1280, 720, 1280, 720, cw, ch));  // first frame: its own size
  CHECK((cw == 1280 && ch == 720));
  // Gears 2 at Nexus: 1024..1280 in 64-pixel steps, many times a second - no re-creation.
  for (uint32_t w : {1024u, 1088u, 1216u, 1280u, 1152u, 1024u}) {
    CHECK_FALSE(up::FitCreationSize(w, 720, 1280, 720, cw, ch));
    CHECK((cw == 1280 && ch == 720));
  }
  // A first frame narrower than the full scene, then wider: once more, straight to the full width.
  cw = ch = 0;
  CHECK(up::FitCreationSize(1024, 720, 1280, 720, cw, ch));
  CHECK(cw == 1024);
  CHECK(up::FitCreationSize(1088, 720, 1280, 720, cw, ch));
  CHECK((cw == 1280 && ch == 720));
  CHECK_FALSE(up::FitCreationSize(1216, 720, 1280, 720, cw, ch));
  // A game always below the nominal size keeps its own (never padded); past the nominal: the frame's size.
  cw = ch = 0;
  CHECK(up::FitCreationSize(1280, 704, 1280, 720, cw, ch));
  CHECK_FALSE(up::FitCreationSize(1280, 704, 1280, 720, cw, ch));
  CHECK(ch == 704);
  CHECK(up::FitCreationSize(1920, 704, 1280, 720, cw, ch));
  CHECK((cw == 1920 && ch == 704));
}

TEST_CASE("upscale: motion re-expressed for the last frame's width (GitHub #323)", "[upscale]") {
  namespace up = xenos::renderer::upscale;
  CHECK(up::ResizedMotion(3.5f, 100.5f, 2560, 2560) == 3.5f);  // same size: unchanged
  CHECK(up::ResizedMotion(3.5f, 100.5f, 2560, 0) == 3.5f);     // no last frame
  // A still camera: the scene's centre (u = 0.5) was drawn at 1280 of 2560 last frame and is at 1024 of 2048 now.
  CHECK(up::ResizedMotion(0.0f, 1024.0f, 2048, 2560) == Approx(256.0f));
  CHECK(up::ResizedMotion(0.0f, 1280.0f, 2560, 2048) == Approx(-256.0f));
  // A moving camera: previous position u_prev * prev_size, whatever the motion within the frame.
  const float w = 2048.0f, prev = 2560.0f, pos = 300.5f, m = -12.25f;
  const float u_prev = (pos + m) / w;
  CHECK(pos + up::ResizedMotion(m, pos, 2048, 2560) == Approx(u_prev * prev));
}

TEST_CASE("upscale: the creation size shrinks slowly, without oscillating (GitHub #323, r83 L2)", "[upscale]") {
  namespace up = xenos::renderer::upscale;
  using C = up::CreationChange;
  constexpr uint32_t kAfter = 600;
  auto feed = [](up::CreationSize& s, uint32_t w, uint32_t h, uint32_t n) {  // n frames; the changes seen
    uint32_t grows = 0, shrinks = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const C c = up::UpdateCreationSize(s, w, h, 1280, 720, kAfter);
      grows += c == C::kGrow, shrinks += c == C::kShrink;
    }
    return std::pair{grows, shrinks};
  };
  // Ultrawide 1280 x 550 after one 720-high frame: the shrink comes on the 600th fitting frame, once.
  up::CreationSize s;
  CHECK(up::UpdateCreationSize(s, 1280, 720, 1280, 720, kAfter) == C::kGrow);
  CHECK(feed(s, 1280, 550, kAfter - 1) == std::pair{0u, 0u});
  CHECK((s.w == 1280 && s.h == 720));
  CHECK(up::UpdateCreationSize(s, 1280, 550, 1280, 720, kAfter) == C::kShrink);
  CHECK((s.w == 1280 && s.h == 550));
  CHECK(feed(s, 1280, 550, 10 * kAfter) == std::pair{0u, 0u});  // at the creation size: nothing more
  // Undone by a 720-high frame: the next shrink needs twice the frames, the one after four times.
  CHECK(up::UpdateCreationSize(s, 1280, 720, 1280, 720, kAfter) == C::kGrow);
  CHECK(feed(s, 1280, 550, 2 * kAfter - 1) == std::pair{0u, 0u});
  CHECK(up::UpdateCreationSize(s, 1280, 550, 1280, 720, kAfter) == C::kShrink);
  CHECK(up::UpdateCreationSize(s, 1280, 720, 1280, 720, kAfter) == C::kGrow);
  CHECK(feed(s, 1280, 550, 4 * kAfter - 1) == std::pair{0u, 0u});
  CHECK(feed(s, 1280, 550, 1) == std::pair{0u, 1u});
  // Staying at the shrunk size changes nothing; the wait stays doubled twice.
  CHECK(feed(s, 1280, 550, 8 * kAfter) == std::pair{0u, 0u});
  CHECK(s.wait == 4 * kAfter);

  // Gears 2 at Nexus: 1024..1280 wide, a full-width frame now and then - never shrinks.
  s = {};
  CHECK(up::UpdateCreationSize(s, 1280, 720, 1280, 720, kAfter) == C::kGrow);
  uint32_t changes = 0;
  for (uint32_t i = 0; i < 20 * kAfter; ++i) {
    const uint32_t w = i % 100 == 99 ? 1280u : 1024u + 64u * (i % 4);
    changes += up::UpdateCreationSize(s, w, 720, 1280, 720, kAfter) != C::kNone;
  }
  CHECK(changes == 0);
  // 1216 wide (5% less area) for a long time: too small a saving to re-create for.
  CHECK(feed(s, 1216, 720, 10 * kAfter) == std::pair{0u, 0u});
  // One full-width frame starts the count again.
  CHECK(feed(s, 1024, 720, kAfter - 1) == std::pair{0u, 0u});
  CHECK(feed(s, 1280, 720, 1) == std::pair{0u, 0u});
  CHECK(feed(s, 1024, 720, kAfter - 1) == std::pair{0u, 0u});
  // A run that widens but stays 1/8 smaller keeps counting, and shrinks to the widest of its frames.
  CHECK(feed(s, 1088, 720, 1) == std::pair{0u, 1u});
  CHECK((s.w == 1088 && s.h == 720));
  // A run that becomes too wide restarts with the frame that still fits a smaller box.
  s = {};
  CHECK(up::UpdateCreationSize(s, 1280, 720, 1280, 720, kAfter) == C::kGrow);
  CHECK(feed(s, 1024, 720, kAfter - 1) == std::pair{0u, 0u});
  CHECK(feed(s, 1216, 720, 1) == std::pair{0u, 0u});  // 1216 x 720 is not 1/8 smaller: the run ends
  CHECK(s.run == 0);
  CHECK(feed(s, 1024, 720, kAfter) == std::pair{0u, 1u});
  // shrink_after = 0: FitCreationSize alone.
  s = {};
  CHECK(up::UpdateCreationSize(s, 1280, 720, 1280, 720, 0) == C::kGrow);
  uint32_t none = 0;
  for (int i = 0; i < 5000; ++i) none += up::UpdateCreationSize(s, 1024, 550, 1280, 720, 0) == C::kNone;
  CHECK(none == 5000);
}

TEST_CASE("upscale: the scene copy comes after the anchor pass (Judgment: a mid-frame copy before the fog on some frames)",
          "[upscale]") {
  xenos::renderer::upscale::SceneCopyPicker p;
  p.last_only = true;
  p.anchored = true;
  constexpr uint32_t kF16 = 1;
  const auto frame = [&](std::initializer_list<bool> copies_after_fog, bool fog) {  // the copies' "after the fog" flags
    int taken = -1, i = 0;
    for (bool after : copies_after_fog) {
      if (p.Take(kF16, after) && taken < 0) taken = i;
      ++i;
    }
    p.EndFrame(fog);
    return taken;
  };
  // Not anchored yet (start-up): the first copy is taken by count, as before; the frame shows the fog with a copy after it.
  REQUIRE(frame({false, true}, true) == 0);
  // Anchored from then on: the copy before the fog is neither taken nor counted.
  REQUIRE(frame({false, true}, true) == 1);
  REQUIRE(frame({true}, true) == 0);           // only the copy after the fog
  REQUIRE(frame({false, true}, true) == 1);    // the mid-frame copy comes back: still the one after the fog
  // One frame without the fog (a skipped fog draw): no copy that frame; the next frame with it still skips the early copy.
  REQUIRE(frame({false}, false) == -1);
  REQUIRE(frame({false, true}, true) == 1);
  REQUIRE(frame({true}, true) == 0);
  // kNoAnchorFrames frames in a row without the fog (a pause or menu over the world): it lets go, copies are taken by
  // count from the next frame, and the anchor comes back with the fog.
  for (uint32_t k = 0; k < xenos::renderer::upscale::SceneCopyPicker::kNoAnchorFrames; ++k) frame({false}, false);
  REQUIRE(frame({false}, false) == 0);
  REQUIRE(frame({false, true}, true) == 0);    // the fog is back: this frame still counts every copy...
  REQUIRE(frame({false, true}, true) == 1);    // ...and from the next the early copy is skipped again
  // The fog drawn but no copy after it for kAnchorPatience frames: the anchor lets go and copies are taken by count again.
  for (uint32_t k = 0; k < xenos::renderer::upscale::SceneCopyPicker::kAnchorPatience; ++k) frame({false}, true);
  REQUIRE(frame({false}, true) == 0);
  // Not anchored (every other game): copies are taken by count whatever the fog does, as before.
  xenos::renderer::upscale::SceneCopyPicker q;
  q.last_only = true;
  REQUIRE(q.Take(kF16, false));
  REQUIRE(q.Take(kF16, true));
  q.EndFrame(true);
  REQUIRE(!q.Take(kF16, false));   // last_only: the second of two copies, by count
  REQUIRE(q.Take(kF16, true));
  q.EndFrame(true);
  REQUIRE(!q.Take(kF16, true));
  REQUIRE(q.Take(kF16, false));    // the fog makes no difference
  q.EndFrame(false);
}
