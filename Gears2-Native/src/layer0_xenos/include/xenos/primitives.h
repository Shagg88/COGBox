// Layer 0 (game-free): turning Xenos primitive types the host APIs don't have into ones they do, and the depth-bias
// conversion. Plain CPU code; the shadow renderer uses the index conversions directly and mirrors the rectangle rule in
// its vertex/geometry stage.
//
// Behaviour follows Xenia Canary e6cd2e4 (src/xenia/gpu/primitive_processor.h, d3d12/pipeline_cache.cc rectangle-list
// geometry shader, draw_util.h GetD3D10IntegerPolygonOffset, xenos.h kPolygonOffsetScaleSubpixelUnit; BSD-3-Clause,
// Copyright (c) Ben Vanik and Xenia project contributors). Re-expressed here; checked against copies of the reference
// logic in tools/golden/gn_renderer_core_golden.cpp.
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace xenos::prim {

// ---- rectangle lists ----
// A rectangle is 3 vertices; the 4th is the mirror of one vertex across the longest edge (the diagonal).
// Result: the triangle-strip order of the 3 given vertices; strip vertex 3 = -v[order[0]] + v[order[1]] + v[order[2]]
// for the position and for every interpolator. Edge lengths use the position's x and y as written by the vertex
// shader (before the divide by w), compared exactly as the reference geometry shader does.
struct RectStrip {
  uint32_t order[3];
};
inline RectStrip RectListStripOrder(const float v0[2], const float v1[2], const float v2[2]) {
  // Squared lengths; each product and sum is rounded separately (DXBC dp2 is not assumed to be fused).
  auto len2 = [](const float a[2], const float b[2]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1];
    const float xx = dx * dx, yy = dy * dy;
    return xx + yy;
  };
  const float e12 = len2(v2, v1), e20 = len2(v0, v2), e01 = len2(v1, v0);
  if ((e20 < e12) && (e01 < e12)) return {{0, 1, 2}};  // 12 longest: strip 0 1 2 [3]
  if (e01 < e20) return {{1, 2, 0}};                   // 20 longest: strip 1 2 0 [3]
  return {{2, 0, 1}};                                  // 01 longest (or ties): strip 2 0 1 [3]
}
inline float RectFourth(float a_first, float b_second, float c_third) {
  // Reference: r1 = -v[order0] + v[order1]; r1 = r1 + v[order2].
  const float t = -a_first + b_second;
  return t + c_third;
}

// ---- index conversions (no primitive reset; the caller splits at reset indices first) ----
constexpr uint32_t TriangleFanListCount(uint32_t n) { return n > 2 ? (n - 2) * 3 : 0; }
constexpr uint32_t LineLoopStripCount(uint32_t n) { return n > 1 ? n + 1 : 0; }
constexpr uint32_t QuadListTriangleListCount(uint32_t n) { return (n / 4) * 6; }

template <typename Index>
void TriangleFanToList(Index* dest, const Index* src, uint32_t n) {
  if (n <= 2) return;
  const Index first = src[0];
  Index prev = src[1];
  for (uint32_t i = 2; i < n; ++i) {
    const Index cur = src[i];
    *dest++ = prev;  // (prev, cur, first) keeps the fan's winding
    *dest++ = cur;
    *dest++ = first;
    prev = cur;
  }
}
template <typename Index>
void LineLoopToStrip(Index* dest, const Index* src, uint32_t n) {
  if (n <= 1) return;
  for (uint32_t i = 0; i < n; ++i) dest[i] = src[i];
  dest[n] = src[0];
}
// Canary's index path splits each quad as (0 1 2)(0 2 3). That path is only Canary's FALLBACK (no geometry shaders, or
// the force_convert_quad_lists_to_triangle_lists cvar). Canary's normal path on both D3D12 (quad_lists_supported =
// true) and Vulkan (with geometryShader) is the geometry shader that emits the strip 0 1 3 2 - see
// QuadListToTriangleListGsOrder below. The two use different diagonals (0-2 vs 1-3): same pixels for a convex quad,
// different attribute interpolation and different coverage for a non-convex one. The reference itself marks the true
// order "TODO: find the correct order".
template <typename Index>
void QuadListToTriangleList(Index* dest, const Index* src, uint32_t n) {
  for (uint32_t q = 0; q < n / 4; ++q, src += 4) {
    *dest++ = src[0];
    *dest++ = src[1];
    *dest++ = src[2];
    *dest++ = src[0];
    *dest++ = src[2];
    *dest++ = src[3];
  }
}
// The same triangles (and provoking vertices) as Canary's quad-list geometry shader (d3d12/pipeline_cache.cc and
// vulkan/vulkan_pipeline_cache.cc, strip order i ^ (i >> 1) = 0 1 3 2): strip triangle 0 = (0 1 3), strip triangle 1 =
// (1 2 3) after the odd-triangle winding rule.
template <typename Index>
void QuadListToTriangleListGsOrder(Index* dest, const Index* src, uint32_t n) {
  for (uint32_t q = 0; q < n / 4; ++q, src += 4) {
    *dest++ = src[0];
    *dest++ = src[1];
    *dest++ = src[3];
    *dest++ = src[1];
    *dest++ = src[2];
    *dest++ = src[3];
  }
}

// ---- depth bias ----
// PA_SU_POLY_OFFSET_*_SCALE is per 1/16 subpixel; multiply by this for the per-pixel slope scale used by D3D/Vulkan.
constexpr float kPolygonOffsetScaleSubpixelUnit = 1.0f / 16.0f;
// D3D10+ integer DepthBias for a guest PA_SU_POLY_OFFSET_*_OFFSET. Unorm24: offset * (2^24 - 1). Float24 (20e4 stored
// in a host float32 buffer): ceil(offset * 2^21) then << 3, so it stays a whole number of 20e4 ULPs.
inline int32_t D3D10IntegerPolygonOffset(bool depth_is_float24, float polygon_offset) {
  constexpr float kUnorm24 = float((1u << 24) - 1);
  constexpr float kFloat24 = float(1u << (21 + 3));
  const float factor = depth_is_float24 ? kFloat24 * (1.0f / 8.0f) : kUnorm24;
  int32_t v = int32_t(std::ceil(std::abs(polygon_offset) * factor));
  if (depth_is_float24) v <<= 3;
  return polygon_offset < 0 ? -v : v;
}

}  // namespace xenos::prim
