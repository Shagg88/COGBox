// Unit test (upl-n108, Brief 15 / notes/48): the EDRAM word <-> host depth conversions our transfer shaders use
// (builtin_shaders.h kTransferPs, kWordTransferPs, kDepthResolveCs), ported line by line to C++, are bit-exact over
// all 2^24 depth patterns and agree with Xenia Canary e6cd2e4bc's xenos::Float20e4To32 / Float32To20e4
// (src/xenia/gpu/xenos.cc:126-166). Also the words <-> UINT-view channel packing of the transfer bits path.
// Game-free: synthetic values only.
//
// Canary's two functions below are copied from xenos.cc (BSD licence):
//   Copyright 2020 Ben Vanik. All rights reserved. Released under the BSD license.
// Our code (MIT) otherwise.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

#include "xenos/formats.h"

namespace {

// ---- Canary e6cd2e4bc xenos.cc:126-166 (BSD) ----
uint32_t CanaryFloat32To20e4(float f32, bool round_to_nearest_even) {
  if (!(f32 > 0.0f)) return 0;
  uint32_t f32u32 = std::bit_cast<uint32_t>(f32);
  if (f32u32 >= 0x3FFFFFF8) return 0xFFFFFF;
  if (f32u32 < 0x38800000) {
    uint32_t shift = std::min(uint32_t(113 - (f32u32 >> 23)), uint32_t(24));
    f32u32 = (0x800000 | (f32u32 & 0x7FFFFF)) >> shift;
  } else {
    f32u32 += 0xC8000000u;
  }
  if (round_to_nearest_even) f32u32 += 3 + ((f32u32 >> 3) & 1);
  return (f32u32 >> 3) & 0xFFFFFF;
}
float CanaryFloat20e4To32(uint32_t f24) {
  f24 &= 0xFFFFFF;
  if (!f24) return 0.0f;
  uint32_t mantissa = f24 & 0xFFFFF;
  uint32_t exponent = f24 >> 20;
  if (!exponent) {
    uint32_t mantissa_lzcnt = uint32_t(std::countl_zero(mantissa)) - (32 - 21);
    exponent = uint32_t(1 - int32_t(mantissa_lzcnt));
    mantissa = (mantissa << mantissa_lzcnt) & 0xFFFFF;
  }
  return std::bit_cast<float>(uint32_t(((exponent + 112) << 23) | (mantissa << 3)));
}

// ---- Ports of our HLSL (builtin_shaders.h) ----
uint32_t FirstBitHigh(uint32_t v) { return 31u - uint32_t(std::countl_zero(v)); }
// kTransferPs XeFloat20e4To32.
float ShaderFloat20e4To32(uint32_t f24u32) {
  uint32_t mantissa = f24u32 & 0xFFFFFu;
  uint32_t exponent = f24u32 >> 20u;
  const bool denormal = exponent == 0u;
  const uint32_t lz = mantissa ? 20u - FirstBitHigh(mantissa) : 0u;  // firstbithigh(0) = -1: the value is 0 anyway
  exponent = denormal ? (1u - lz) : exponent;
  mantissa = denormal ? ((mantissa << lz) & 0xFFFFFu) : mantissa;
  return std::bit_cast<float>(f24u32 != 0u ? (((exponent + 112u) << 23u) | (mantissa << 3u)) : 0u);
}
// kTransferPs / kWordTransferPs XeD24FromHost / kDepthResolveCs (round off): XeFloat32To20e4.
uint32_t ShaderFloat32To20e4(uint32_t f32u32) {
  f32u32 = std::min((f32u32 <= 0x7FFFFFFFu) ? f32u32 : 0u, 0x3FFFFFF8u);
  const uint32_t denormalized = ((f32u32 & 0x7FFFFFu) | 0x800000u) >> std::min(113u - (f32u32 >> 23u), 24u);
  const uint32_t f24u32 = (f32u32 < 0x38800000u) ? denormalized : (f32u32 + 0xC8000000u);
  return (f24u32 >> 3u) & 0xFFFFFFu;
}
// D24FS8 with the x0.5 remap: write = decode * 0.5 (into D32_FLOAT, stored exactly), read = d * 2 -> encode.
uint32_t ShaderD24fs8RoundTrip(uint32_t d24, bool remap) {
  const float stored = remap ? ShaderFloat20e4To32(d24) * 0.5f : ShaderFloat20e4To32(d24);
  const float guest = remap ? stored * 2.0f : stored;
  return ShaderFloat32To20e4(std::bit_cast<uint32_t>(guest));
}
// D24S8: write (n + (n >> 23)) * 2^-24; D3D's float -> UNORM24 rounds f * (2^24 - 1) to nearest; the SRV returns
// n / (2^24 - 1); read = round(saturate(d) * 16777215).
uint32_t ShaderD24s8RoundTrip(uint32_t d24) {
  const float written = float(d24 + (d24 >> 23u)) * (1.0f / 16777216.0f);
  const uint32_t unorm = uint32_t(std::lround(double(written) * 16777215.0));
  const float sampled = float(double(unorm) / 16777215.0);
  const float scaled = std::clamp(sampled, 0.0f, 1.0f) * 16777215.0f;
  return std::min(uint32_t(std::lround(scaled)), 0xFFFFFFu);
}
// kHlslHostWords XeBitsToGuestWords / XeGuestWordsToBits (formats 4-7, 14, 15).
void BitsToWords(const uint32_t c[4], uint32_t fmt, uint32_t w[2]) {
  switch (fmt) {
    case 4: case 6: w[0] = (c[0] & 0xFFFFu) | (c[1] << 16u); w[1] = 0; break;
    case 5: case 7: w[0] = (c[0] & 0xFFFFu) | (c[1] << 16u); w[1] = (c[2] & 0xFFFFu) | (c[3] << 16u); break;
    case 15: w[0] = c[0]; w[1] = c[1]; break;
    default: w[0] = c[0]; w[1] = 0; break;
  }
}
void WordsToBits(const uint32_t w[2], uint32_t fmt, uint32_t c[4]) {
  switch (fmt) {
    case 4: case 6: c[0] = w[0] & 0xFFFFu; c[1] = w[0] >> 16u; c[2] = c[3] = 0; break;
    case 5: case 7: c[0] = w[0] & 0xFFFFu; c[1] = w[0] >> 16u; c[2] = w[1] & 0xFFFFu; c[3] = w[1] >> 16u; break;
    case 15: c[0] = w[0]; c[1] = w[1]; c[2] = c[3] = 0; break;
    default: c[0] = w[0]; c[1] = c[2] = c[3] = 0; break;
  }
}

}  // namespace

TEST_CASE("20e4 decode matches Canary bit for bit and every pattern round-trips", "[transfer][20e4]") {
  uint32_t decode_mismatch = 0, canary_trip = 0, ours_trip = 0, remap_trip = 0, cpu_trip = 0;
  for (uint32_t n = 0; n < (1u << 24); ++n) {
    const float ours = ShaderFloat20e4To32(n);
    const float canary = CanaryFloat20e4To32(n);
    decode_mismatch += std::bit_cast<uint32_t>(ours) != std::bit_cast<uint32_t>(canary);
    canary_trip += CanaryFloat32To20e4(canary, false) != n;
    ours_trip += ShaderFloat32To20e4(std::bit_cast<uint32_t>(ours)) != n;
    remap_trip += ShaderD24fs8RoundTrip(n, true) != n;
    cpu_trip += xenos::formats::Float32To20e4(xenos::formats::Float20e4To32(n), false) != n;
  }
  CHECK(decode_mismatch == 0);
  CHECK(canary_trip == 0);
  CHECK(ours_trip == 0);
  CHECK(remap_trip == 0);
  CHECK(cpu_trip == 0);
  // The job-111 word's depth (a 20e4 denormal) explicitly.
  CHECK(ShaderD24fs8RoundTrip(0x01407Cu, true) == 0x01407Cu);
  // Canary's rounding mode leaves exact values alone too (notes/48 section 3).
  CHECK(CanaryFloat32To20e4(CanaryFloat20e4To32(0x01407Cu), true) == 0x01407Cu);
}

TEST_CASE("D24S8 words round-trip through the unorm24 host format", "[transfer][d24s8]") {
  uint32_t bad = 0;
  for (uint32_t n = 0; n < (1u << 24); ++n) bad += ShaderD24s8RoundTrip(n) != n;
  CHECK(bad == 0);
}

TEST_CASE("Guest words <-> UINT-view channels of the transfer bits path", "[transfer][bits]") {
  const uint32_t words[][2] = {{0x01407C0Fu, 0x7C0F8000u}, {0xFFFFFFFFu, 0x00000000u}, {0x80008001u, 0x7FFF7E00u}};
  for (uint32_t fmt : {4u, 5u, 6u, 7u, 14u, 15u}) {
    const bool two = fmt == 5 || fmt == 7 || fmt == 15;
    for (const auto& w : words) {
      uint32_t c[4], back[2];
      WordsToBits(w, fmt, c);
      BitsToWords(c, fmt, back);
      CHECK(back[0] == w[0]);
      CHECK(back[1] == (two ? w[1] : 0u));
    }
  }
}
