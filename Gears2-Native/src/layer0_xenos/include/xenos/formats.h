// Layer 0 (game-free): Xbox 360 GPU (Xenos) number formats and the 2D tiled address, as plain CPU code for our own
// renderer and for tests. Behaviour follows Xenia Canary (src/xenia/gpu: xenos.cc, texture_address.h, the 7e3 shader
// emitters and pixel_formats.xesli; BSD-3-Clause, Copyright (c) Ben Vanik and Xenia project contributors). Written fresh
// from the algorithm descriptions in notes/31; not copied. Note: xenia-project master's Float7e3To32 has a mantissa bug
// (fixed in Canary f3e42609a); Canary/Edge/rexglue are the reference.
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace xenos::formats {

// 7e3: unsigned 10-bit float (3-bit exponent, 7-bit mantissa), used by k_2_10_10_10_FLOAT render targets. [0, 31.875].
inline float Float7e3To32(uint32_t f10) {
  f10 &= 0x3FF;
  const uint32_t mantissa = f10 & 0x7F;
  const uint32_t exponent = f10 >> 7;
  if (exponent == 0) {
    return std::ldexp(float(mantissa) / 128.0f, -2);  // denormal: 0.m * 2^-2
  }
  return std::ldexp(1.0f + float(mantissa) / 128.0f, int(exponent) - 3);
}

// float32 -> 7e3 with clamping to [0, 31.875] (NaN -> 0) and round to nearest even, as the shader translators do.
inline uint32_t Float32To7e3(float f) {
  if (!(f > 0.0f)) return 0;  // also NaN and negative zero
  f = std::min(f, 31.875f);
  const uint32_t bits = std::bit_cast<uint32_t>(f);
  uint32_t b;
  if (f < 0.25f) {
    b = ((bits & 0x7FFFFF) | 0x800000) >> std::min<uint32_t>(125 - (bits >> 23), 24);
  } else {
    b = bits - (124u << 23);
  }
  return ((b + 0x7FFF + ((b >> 16) & 1)) >> 16) & 0x3FF;
}

// 20e4: unsigned 24-bit float depth (4-bit exponent, 20-bit mantissa). [0, 2).
inline uint32_t Float32To20e4(float f, bool round_to_nearest_even) {
  if (!(f > 0.0f)) return 0;  // also NaN and negative zero
  uint32_t bits = std::bit_cast<uint32_t>(f);
  if (bits >= 0x3FFFFFF8u) return 0xFFFFFF;
  uint32_t v;
  if (bits < 0x38800000u) {  // below 2^-14: denormal
    const uint32_t exp = bits >> 23;
    v = (0x800000 | (bits & 0x7FFFFF)) >> std::min<uint32_t>(113 - exp, 24);
  } else {
    v = bits + 0xC8000000u;  // rebias the exponent by -112
  }
  if (round_to_nearest_even) v += 3 + ((v >> 3) & 1);
  return (v >> 3) & 0xFFFFFF;
}

inline float Float20e4To32(uint32_t f24) {
  f24 &= 0xFFFFFF;
  const uint32_t mantissa = f24 & 0xFFFFF;
  const uint32_t exponent = f24 >> 20;
  if (exponent == 0) {
    return std::ldexp(float(mantissa), -34);  // denormal: 0.m * 2^-14
  }
  return std::bit_cast<float>(((exponent + 112) << 23) | (mantissa << 3));
}

struct Rgba {
  float r, g, b, a;
};

// k_2_10_10_10 (UNorm) and k_2_10_10_10_FLOAT (7e3 RGB): R bits 0-9, G 10-19, B 20-29, A 30-31.
inline Rgba UnpackR10G10B10A2UNorm(uint32_t p) {
  return {float(p & 0x3FF) / 1023.0f, float((p >> 10) & 0x3FF) / 1023.0f, float((p >> 20) & 0x3FF) / 1023.0f,
          float(p >> 30) / 3.0f};
}
inline Rgba UnpackR10G10B10A2Float(uint32_t p) {
  return {Float7e3To32(p), Float7e3To32(p >> 10), Float7e3To32(p >> 20), float(p >> 30) / 3.0f};
}

// Byte offset of block (x, y) in a 2D tiled surface. x, y in blocks; pitch in blocks, aligned to 32.
inline int32_t Tiled2D(int32_t x, int32_t y, uint32_t pitch_aligned, unsigned bytes_per_block_log2) {
  const uint32_t outer = ((uint32_t(y) >> 5) * (pitch_aligned >> 5) + (uint32_t(x) >> 5)) << 6;
  const uint32_t inner = (((uint32_t(y) >> 1) & 7) << 3) | (uint32_t(x) & 7);
  const uint32_t oib = (outer | inner) << bytes_per_block_log2;
  const uint32_t bank = (uint32_t(y) >> 4) & 1;
  const uint32_t pipe = ((uint32_t(x) >> 3) & 3) ^ (((uint32_t(y) >> 3) & 1) << 1);
  return int32_t(((uint32_t(y) & 1) << 4) | (pipe << 6) | (bank << 11) | (oib & 0xF) | (((oib >> 4) & 1) << 5) |
                 (((oib >> 5) & 7) << 8) | ((oib >> 8) << 12));
}

}  // namespace xenos::formats
