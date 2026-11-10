// Layer 0 (game-free): the Xenos piecewise-linear (PWL) gamma curve used by k_8_8_8_8_GAMMA render targets and gamma
// textures. Plain CPU code; the same arithmetic our translated shaders must use (float32, no fused multiply-add).
//
// Behaviour follows Xenia Canary e6cd2e4 src/xenia/gpu/xenos.cc PWLGammaToLinear / LinearToPWLGamma, identical to rexglue
// SDK 0.10.0 src/graphics/xenos.cpp (BSD-3-Clause, Copyright (c) Ben Vanik and Xenia project contributors), which follow
// the Source Engine's X360 gamma code and the vrfiz truncation seen in Direct3D 9 disassembly. Re-expressed here and
// checked bit for bit against both over every float32 bit pattern by tools/golden/gn_renderer_core_golden.cpp.
#pragma once

#include <cmath>

namespace xenos::gamma {

// Saturate exactly as rex::saturate / xe::saturate: NaN -> +0, -0 -> +0.
inline float Saturate(float v) {
  const float lo = v > 0.0f ? v : 0.0f;
  return lo < 1.0f ? lo : 1.0f;
}

// 8-bit-style gamma value in [0, 1] -> linear in [0, 1]. Four pieces, breakpoints at 64/255, 96/255, 192/255.
inline float PWLGammaToLinear(float gamma) {
  gamma = Saturate(gamma);
  float scale, offset;
  if (gamma >= 96.0f / 255.0f) {
    if (gamma >= 192.0f / 255.0f) {
      scale = 8.0f / 1024.0f;
      offset = -1024.0f;
    } else {
      scale = 4.0f / 1024.0f;
      offset = -256.0f;
    }
  } else {
    if (gamma >= 64.0f / 255.0f) {
      scale = 2.0f / 1024.0f;
      offset = -64.0f;
    } else {
      scale = 1.0f / 1024.0f;
      offset = 0.0f;
    }
  }
  // Written as separate statements so no compiler contracts them into an FMA (keep -ffp-contract=off / /fp:precise).
  float linear = gamma * ((255.0f * 1024.0f) * scale);
  linear = linear + offset;
  linear += std::trunc(linear * scale);
  linear *= 1.0f / 1023.0f;
  return linear;
}

// Linear in [0, 1] -> gamma in [0, 1]. Breakpoints at 64/1023, 128/1023, 512/1023.
inline float LinearToPWLGamma(float linear) {
  linear = Saturate(linear);
  float scale, offset;
  if (linear >= 128.0f / 1023.0f) {
    if (linear >= 512.0f / 1023.0f) {
      scale = 1023.0f / 8.0f;
      offset = 128.0f / 255.0f;
    } else {
      scale = 1023.0f / 4.0f;
      offset = 64.0f / 255.0f;
    }
  } else {
    if (linear >= 64.0f / 1023.0f) {
      scale = 1023.0f / 2.0f;
      offset = 32.0f / 255.0f;
    } else {
      scale = 1023.0f;
      offset = 0.0f;
    }
  }
  float g = std::trunc(linear * scale) * (1.0f / 255.0f);
  return g + offset;
}

}  // namespace xenos::gamma
