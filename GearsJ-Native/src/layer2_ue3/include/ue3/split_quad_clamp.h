// Layer 2 (Unreal Engine 3): GitHub #420, Judgment's side-by-side "glow". With two players side by side, each view's
// post-process chain draws into one half of shared, full-width quarter-resolution buffers (bloom / streak), but the
// engine's DrawDenormalizedQuad (sub_827DA688 in TU4) is given a source range that runs a few texels past the view's
// half (the 1-texel border plus rounding; RenderDoc: U 0.00305 .. 0.5062 of a 322-wide buffer). The blur then pulls the
// other view's columns into this view's inner edge - the white (green while aiming) bar. The game never shipped side
// by side, so nothing clamps it; in its top / bottom split each view spans the whole buffer width and the read past
// the edge is clamped by the texture's own edge. This keeps a quad that draws into one half of its target reading only
// the same half of its source. Our code (MIT).
#pragma once

#include <algorithm>
#include <cstdint>

namespace ue3 {

struct QuadSource {
  float u;
  float size_u;
};

// x / size_x: the destination rectangle in target pixels; u / size_u: the source rectangle in texture pixels;
// target_w / texture_w: the two sizes. Returns the source range, cut at the half when the quad draws into one half of
// the target and its source overshoots that half by a few texels only (a read that spans both halves is left alone).
inline QuadSource ClampQuadToViewHalf(float x, float size_x, float u, float size_u, uint32_t target_w,
                                      uint32_t texture_w) {
  QuadSource out{u, size_u};
  if (target_w == 0 || texture_w == 0 || size_x <= 0.0f || size_u <= 0.0f) return out;
  const float half_t = float(target_w) * 0.5f, half_s = float(texture_w) * 0.5f;
  const float limit_t = std::max(4.0f, half_t * 0.05f);  // the border + rounding overshoot, never a real cross read
  const float limit = std::max(4.0f, half_s * 0.05f);
  // Logged side by side (split-20261123-115736.log): both views use the LEFT part of the shared quarter-resolution
  // buffers, one after the other; the bordered blur then draws and reads x / u 1 +162 of 322 where the pass before
  // wrote x 1 +160 - the destination overshoots the half as much as the source.
  const bool left = x < half_t && x + size_x <= half_t + limit_t;
  const bool right = x >= half_t - 1.0f;
  if (left && !right) {
    const float end = u + size_u, over = end - half_s;
    // Cut one texel short of the half: the blur's taps reach one texel past the quad's end (RenderDoc).
    if (u < half_s - 1.0f && over > 0.0f && over <= limit) out.size_u = (half_s - 1.0f) - u;
  } else if (right && !left) {
    const float under = half_s - u;
    if (u + size_u > half_s && under > 0.0f && under <= limit) {
      out.u = half_s;
      out.size_u = size_u - under;
    }
  }
  return out;
}

}  // namespace ue3
