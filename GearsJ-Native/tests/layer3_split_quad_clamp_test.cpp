// ue3/split_quad_clamp.h: a side-by-side view's post-process quad reads only what its own pass wrote (GitHub #420,
// Judgment glow). The values are the calls logged in a two-player side-by-side game (split-20261123-115736.log).
#include <catch2/catch_test_macros.hpp>

#include "ue3/split_quad_clamp.h"

using ue3::ClampQuadToViewHalf;
using ue3::QuadSource;

TEST_CASE("the bordered blur reading 2 texels past the half is cut (the glow)", "[split]") {
  // x 1 +162 target 322x182 u 1 +162 texture 322x182 (the previous pass wrote x 1 +160). The blur's taps reach one
  // texel past the quad's end, so the read ends at half - 1 = 160.
  QuadSource q = ClampQuadToViewHalf(1.0f, 162.0f, 1.0f, 162.0f, 322, 322);
  CHECK(q.u == 1.0f);
  CHECK(q.size_u == 159.0f);
}

TEST_CASE("the side-by-side calls that read only what was written are left alone", "[split]") {
  auto same = [](float x, float sx, float u, float su, uint32_t tw, uint32_t sw) {
    const QuadSource q = ClampQuadToViewHalf(x, sx, u, su, tw, sw);
    return q.u == u && q.size_u == su;
  };
  CHECK(same(0, 160, 0, 160, 320, 320));
  CHECK(same(320, 320, 640, 640, 640, 1280));  // view 2's full-resolution half
  CHECK(same(1, 160, 320, 320, 322, 640));     // view 2 into the bordered buffer's left part
  CHECK(same(1, 160, 0, 320, 322, 640));
  CHECK(same(0, 80, 0, 160, 160, 320));
  CHECK(same(0, 160, 320, 320, 320, 640));
  CHECK(same(0, 16, 0, 16, 16, 16));
  CHECK(same(0, 32, 320, 320, 32, 640));
}

TEST_CASE("one player's full-width quads are left alone", "[split]") {
  QuadSource q = ClampQuadToViewHalf(1.0f, 322.0f, 1.0f, 322.0f, 322, 322);
  CHECK(q.size_u == 322.0f);
  q = ClampQuadToViewHalf(0.0f, 640.0f, 0.0f, 1280.0f, 640, 1280);
  CHECK(q.size_u == 1280.0f);
}

TEST_CASE("a source spanning both halves is left alone", "[split]") {
  QuadSource q = ClampQuadToViewHalf(0.0f, 160.0f, 0.0f, 300.0f, 320, 320);
  CHECK(q.size_u == 300.0f);
}

TEST_CASE("bad sizes are left alone", "[split]") {
  QuadSource q = ClampQuadToViewHalf(1.0f, 162.0f, 1.0f, 162.0f, 0, 322);
  CHECK(q.size_u == 162.0f);
  q = ClampQuadToViewHalf(1.0f, 162.0f, 1.0f, 162.0f, 322, 0);
  CHECK(q.size_u == 162.0f);
}
