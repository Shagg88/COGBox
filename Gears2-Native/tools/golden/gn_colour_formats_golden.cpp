// Golden generator / cross-check for xenos/colour_formats.h. Not part of the game build.
//
// It holds verbatim (or op-for-op) copies of the reference code and compares xenos/colour_formats.h against them
// exhaustively, then writes tests/golden/layer0_colour_formats_golden.inc with tables the Catch2 test checks.
//   build: g++ -std=c++23 -O2 -ffp-contract=off -I<repo>/src/layer0_xenos/include gn_colour_formats_golden.cpp
//   run:   ./a.out > tests/golden/layer0_colour_formats_golden.inc   (exit status 1 on any mismatch, report on stderr)
//
// The copied reference code is from Xenia Canary e6cd2e4bc and keeps its licence:
// ---------------------------------------------------------------------------------------------------------------------
// Copyright (c) 2015, Ben Vanik. All rights reserved.
// Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
// following conditions are met:
//   * Redistributions of source code must retain the above copyright notice, this list of conditions and the following
//     disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
//     following disclaimer in the documentation and/or other materials provided with the distribution.
//   * Neither the name of the project nor the names of its contributors may be used to endorse or promote products
//     derived from this software without specific prior written permission.
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
// INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL BEN VANIK BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
// OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// ---------------------------------------------------------------------------------------------------------------------
#include <bit>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "xenos/colour_formats.h"

namespace ref {
// --- shims for the Xenia helpers the copied code calls --------------------------------------------------------------
inline uint32_t lzcnt(uint32_t v) { return v ? uint32_t(__builtin_clz(v)) : 32u; }
template <typename T, typename F>
T Reinterpret(F f) {
  T t;
  std::memcpy(&t, &f, sizeof(T));
  return t;
}

// --- xenos.cc 104-120, verbatim (namespaces flattened) ---------------------------------------------------------------
float Float7e3To32(uint32_t f10) {
  f10 &= 0x3FF;
  if (!f10) {
    return 0.0f;
  }
  uint32_t mantissa = f10 & 0x7F;
  uint32_t exponent = f10 >> 7;
  if (!exponent) {
    // Normalize the value in the resulting float.
    // do { Exponent--; Mantissa <<= 1; } while ((Mantissa & 0x80) == 0)
    uint32_t mantissa_lzcnt = lzcnt(mantissa) - (32 - 8);
    exponent = uint32_t(1 - int32_t(mantissa_lzcnt));
    mantissa = (mantissa << mantissa_lzcnt) & 0x7F;
  }
  return Reinterpret<float>(uint32_t(((exponent + 124) << 23) | (mantissa << 16)));
}

// --- dxbc_shader_translator_om.cc 3816-3889: the DXBC instruction sequence, executed op for op -----------------------
// DXBC semantics: ushr uses the low 5 bits of the shift; iadd wraps; bfi(w, o, ins, base); ubfe(w, o, src);
// max/min return the non-NaN operand.
uint32_t OpUShR(uint32_t a, uint32_t s) { return a >> (s & 31); }
uint32_t OpBFI(uint32_t w, uint32_t o, uint32_t ins, uint32_t base) {
  const uint32_t mask = (w >= 32 ? 0xFFFFFFFFu : ((1u << w) - 1u)) << o;
  return (base & ~mask) | ((ins << o) & mask);
}
uint32_t OpUBFE(uint32_t w, uint32_t o, uint32_t src) { return (src >> o) & ((1u << w) - 1u); }
float OpMax(float a, float b) { return std::isnan(a) ? b : std::isnan(b) ? a : (a > b ? a : (b > a ? b : b)); }
float OpMin(float a, float b) { return std::isnan(a) ? b : std::isnan(b) ? a : (a < b ? a : (b < a ? b : b)); }
uint32_t PreClampedFloat32To7e3(uint32_t f32) {
  uint32_t f10, temp;
  temp = f32 < 0x3E800000u ? 0xFFFFFFFFu : 0u;  // OpULT
  if (temp) {
    temp = OpUShR(f32, 23);
    temp = uint32_t(125 - int32_t(temp));  // OpIAdd(LI(125), -temp)
    temp = std::min(temp, 24u);            // OpUMin
    f10 = OpBFI(9, 23, 1, f32);
    f10 = OpUShR(f10, temp);
  } else {
    f10 = f32 + 0xC2000000u;
  }
  temp = OpUBFE(1, 16, f10);
  f10 = f10 + 0x7FFFu;
  f10 = f10 + temp;
  f10 = OpUBFE(10, 16, f10);
  return f10;
}
// OpMax(x, 0) with OpMax(+0 vs -0) resolved as "b" (the literal +0): our header documents max(-0, 0) = +0.
uint32_t UnclampedFloat32To7e3(float f) {
  float c = OpMax(f, 0.0f);
  c = OpMin(c, 31.875f);
  return PreClampedFloat32To7e3(std::bit_cast<uint32_t>(c));
}

// --- d3d12_render_target_cache.cc 5385-5465, the colour clear switch, verbatim with an int format -----------------------
float PWLGammaToLinear(float g) { return xenos::gamma::PWLGammaToLinear(g); }  // gamma.h already checked bit for bit
void ColourClear(uint32_t format, uint64_t clear_value, float color_clear_value[4], bool& clear_via_drawing) {
  for (int i = 0; i < 4; ++i) color_clear_value[i] = 0.0f;
  clear_via_drawing = false;
  switch (format) {
    case 0: {  // k_8_8_8_8
      for (uint32_t j = 0; j < 4; ++j) {
        color_clear_value[j] = ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
      }
    } break;
    case 1: {  // k_8_8_8_8_GAMMA
      for (uint32_t j = 0; j < 4; ++j) {
        color_clear_value[j] = ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
      }
      for (uint32_t j = 0; j < 3; ++j) {
        color_clear_value[j] = PWLGammaToLinear(color_clear_value[j]);
      }
    } break;
    case 2:
    case 10: {
      for (uint32_t j = 0; j < 3; ++j) {
        color_clear_value[j] = ((clear_value >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
      }
      color_clear_value[3] = ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
    } break;
    case 3:
    case 12: {
      for (uint32_t j = 0; j < 3; ++j) {
        color_clear_value[j] = Float7e3To32((clear_value >> (j * 10)) & 0x3FF);
      }
      color_clear_value[3] = ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
    } break;
    case 4:
    case 6: {
      for (uint32_t j = 0; j < 2; ++j) {
        color_clear_value[j] = float((clear_value >> (j * 16)) & 0xFFFF);
      }
    } break;
    case 5:
    case 7: {
      for (uint32_t j = 0; j < 4; ++j) {
        color_clear_value[j] = float((clear_value >> (j * 16)) & 0xFFFF);
      }
    } break;
    case 14: {
      color_clear_value[0] = float(uint32_t(clear_value));
      if (uint64_t(color_clear_value[0]) != uint32_t(clear_value)) {
        clear_via_drawing = true;
      }
    } break;
    case 15: {
      color_clear_value[0] = float(uint32_t(clear_value));
      color_clear_value[1] = float(uint32_t(clear_value >> 32));
      if (uint64_t(color_clear_value[0]) != uint32_t(clear_value) ||
          uint64_t(color_clear_value[1]) != uint32_t(clear_value >> 32)) {
        clear_via_drawing = true;
      }
    } break;
  }
}

// --- pixel_formats.xesli 104-126 XePackFixed, the HLSL compiled as C++ (min/max/saturate with HLSL semantics) --------
#define kXenosSurfaceNumberFormat_SignedRepeatingFraction 1u
#define kXenosSurfaceNumberFormat_UnsignedInteger 2u
#define kXenosSurfaceNumberFormat_SignedInteger 3u
using uint = uint32_t;
float max(float a, float b) { return OpMax(a, b); }
float min(float a, float b) { return OpMin(a, b); }
float saturate_xe(float v) { return OpMin(OpMax(v, 0.0f), 1.0f); }
uint XePackFixed(float value, uint bits, uint num_format) {
  uint unsigned_max = (1u << bits) - 1u;
  uint packed = 0u;
  if (num_format == kXenosSurfaceNumberFormat_SignedRepeatingFraction) {
    // Signed fraction uses the positive endpoint, then keeps the destination
    // bit width.
    float positive_max = float((1u << (bits - 1u)) - 1u);
    packed = uint(int(min(max(value, -1.0f), 1.0f) * positive_max +
                      (value >= 0.0f ? 0.5f : -0.5f)));
  } else if (num_format == kXenosSurfaceNumberFormat_UnsignedInteger) {
    packed = uint(min(max(value, 0.0f), float(unsigned_max)) + 0.5f);
  } else if (num_format == kXenosSurfaceNumberFormat_SignedInteger) {
    int signed_min = -int(1u << (bits - 1u));
    int signed_max = int((1u << (bits - 1u)) - 1u);
    packed = uint(int(min(max(value, float(signed_min)), float(signed_max)) +
                      (value >= 0.0f ? 0.5f : -0.5f)));
  } else {
    // Unsigned fraction, float-on-fixed, or anything unexpected: keep the
    // usual unsigned-fraction pack.
    packed = uint(saturate_xe(value) * float(unsigned_max) + 0.5f);
  }
  return packed & unsigned_max;
}

// --- pixel_formats.xesli 422-447 XeUnpackR16G16Edram (scalar), Canary, and the SDK's disassembled form --------------
float UnpackR16Canary(uint p) { return max(-32.0f, float(int(p << 16) >> 16) * (32.0f / 32767.0f)); }
// SDK resolve_full_32bpp_cs.h: ishl 16; ishr 16; itof; mul l(0.000977) (= 0x3a800100, 32/32767); max l(-1.0)
float UnpackR16Sdk(uint p) { return max(float(int(p << 16) >> 16) * std::bit_cast<float>(0x3A800100u), -1.0f); }
}  // namespace ref

namespace xc = xenos::colour;

static int g_fail = 0;
#define CHECK(cond, ...)                         \
  do {                                           \
    if (!(cond)) {                               \
      if (g_fail < 20) {                         \
        std::fprintf(stderr, "MISMATCH: " __VA_ARGS__); \
        std::fprintf(stderr, "\n");              \
      }                                          \
      ++g_fail;                                  \
    }                                            \
  } while (0)

static uint64_t Fnv(uint64_t h, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    h ^= (v >> (i * 8)) & 0xFF;
    h *= 0x100000001B3ull;
  }
  return h;
}

int main() {
  // 1. Float7e3To32 for all 1024 codes.
  std::printf("// Generated by tools/golden/gn_colour_formats_golden.cpp from verbatim Xenia Canary e6cd2e4bc code.\n");
  std::printf("// Do not edit.\n#pragma once\n#include <cstdint>\n\nnamespace golden_colour {\n");
  std::printf("// xenos.cc 104-120 Float7e3To32(code), as float bits.\ninline constexpr uint32_t kFloat7e3To32[1024] = {");
  for (uint32_t c = 0; c < 1024; ++c) {
    const uint32_t r = std::bit_cast<uint32_t>(ref::Float7e3To32(c));
    CHECK(std::bit_cast<uint32_t>(xc::Float7e3To32(c)) == r, "Float7e3To32(%u)", c);
    std::printf("%s0x%08X", c == 0 ? "\n    " : (c % 8 ? ", " : ",\n    "), r);
  }
  std::printf("};\n\n");

  // 2. 7e3 encode over every float32 bit pattern (header vs the DXBC op sequence).
  uint64_t h_enc = 0xCBF29CE484222325ull;
  for (uint64_t i = 0; i <= 0xFFFFFFFFull; ++i) {
    const float f = std::bit_cast<float>(uint32_t(i));
    const uint32_t r = ref::UnclampedFloat32To7e3(f);
    const uint32_t m = xc::Float32To7e3(f);
    if (r != m) CHECK(false, "Float32To7e3(0x%08X) ref %u ours %u", uint32_t(i), r, m);
    if ((i & 0xFFF) == 0) h_enc = Fnv(h_enc, r);
  }
  std::printf("// UnclampedFloat32To7e3 (om.cc 3816-3889, executed op for op) over float bits 0, 0x1000, 0x2000, ...;\n");
  std::printf("// FNV-1a 64 of the codes. Every one of the 2^32 inputs was also compared directly.\n");
  std::printf("inline constexpr uint64_t kEncode7e3Stride4096Fnv = 0x%016" PRIX64 "ull;\n\n", h_enc);

  // 3. Spot table: encode of selected floats.
  const float spots[] = {0.0f,     -0.0f,   -1.0f,   0.25f,     0.2490234375f, 0.125f,   0.0625f,      0.015625f,
                         0.00390625f, 0.001953125f, 0.0009765625f, 1.0f, 1.00390625f, 1.01171875f, 1.0078125f, 31.875f,
                         32.0f,   1e9f,    3.0f,    0.333333343f, 15.9375f, 0.5f, 0.1f, 2.0f};
  std::printf("struct Encode7e3Spot { uint32_t in_bits, code; };\ninline constexpr Encode7e3Spot kEncode7e3Spots[] = {");
  int n = 0;
  for (float f : spots) {
    std::printf("%s{0x%08X, 0x%03X}", n == 0 ? "\n    " : (n % 4 ? ", " : ",\n    "), std::bit_cast<uint32_t>(f), ref::UnclampedFloat32To7e3(f));
    ++n;
  }
  {
    const float nan = std::bit_cast<float>(0x7FC00000u), inf = std::bit_cast<float>(0x7F800000u);
    std::printf(",\n    {0x%08X, 0x%03X}, {0x%08X, 0x%03X}", std::bit_cast<uint32_t>(nan), ref::UnclampedFloat32To7e3(nan),
                std::bit_cast<uint32_t>(inf), ref::UnclampedFloat32To7e3(inf));
  }
  std::printf("};\n\n");

  // 4. XePackFixed for every fp16 value as the input, bits 2/8/10/11/16, num formats 0-3.
  uint64_t h_pack = 0xCBF29CE484222325ull;
  for (uint32_t hbits = 0; hbits < 0x10000; ++hbits) {
    const float v = xc::HalfToFloat(uint16_t(hbits));
    for (uint32_t bits : {2u, 8u, 10u, 11u, 16u}) {
      for (uint32_t nf = 0; nf < 4; ++nf) {
        const uint32_t r = ref::XePackFixed(v, bits, nf);
        const uint32_t m = xc::PackFixedCanary(v, bits, xc::NumFormat(nf));
        if (r != m) CHECK(false, "XePackFixed(h=0x%04X, %u, %u) ref %u ours %u", hbits, bits, nf, r, m);
        h_pack = Fnv(h_pack, r);
      }
    }
  }
  std::printf("// XePackFixed (pixel_formats.xesli 104-126) of every fp16 value x bits {2,8,10,11,16} x num format 0-3.\n");
  std::printf("inline constexpr uint64_t kPackFixedHalvesFnv = 0x%016" PRIX64 "ull;\n\n", h_pack);

  // 5. 16-bit fixed decode, all 65536 codes, both references.
  uint64_t h_c = 0xCBF29CE484222325ull, h_s = 0xCBF29CE484222325ull;
  for (uint32_t c = 0; c < 0x10000; ++c) {
    const float rc = ref::UnpackR16Canary(c), rs = ref::UnpackR16Sdk(c);
    CHECK(std::bit_cast<uint32_t>(xc::Fixed16ResolveDecode(c, xc::Reference::kCanary)) == std::bit_cast<uint32_t>(rc),
          "Fixed16 canary %u", c);
    CHECK(std::bit_cast<uint32_t>(xc::Fixed16ResolveDecode(c, xc::Reference::kSdk)) == std::bit_cast<uint32_t>(rs),
          "Fixed16 sdk %u", c);
    h_c = Fnv(h_c, std::bit_cast<uint32_t>(rc));
    h_s = Fnv(h_s, std::bit_cast<uint32_t>(rs));
  }
  std::printf("// XeUnpackR16G16Edram (Canary, max -32) and the SDK resolve disassembly (max -1) for all 65536 codes.\n");
  std::printf("inline constexpr uint64_t kFixed16DecodeCanaryFnv = 0x%016" PRIX64 "ull;\n", h_c);
  std::printf("inline constexpr uint64_t kFixed16DecodeSdkFnv = 0x%016" PRIX64 "ull;\n\n", h_s);

  // 6. Colour clear values (both emulators have the same switch).
  const uint32_t formats[] = {0, 1, 2, 3, 4, 5, 6, 7, 10, 12, 14, 15};
  const uint64_t values[] = {0, 0xFFFFFFFFFFFFFFFFull, 0x80402010C0FF7F01ull, 0x3F8000004B800001ull, 0x0123456789ABCDEFull,
                             0x00000000FFFFFF00ull, 0x7FFF80017FFF8000ull, 0x3C00BC007C00FC00ull};
  std::printf("struct ClearCase { uint32_t format; uint64_t value; uint32_t rgba_bits[4]; bool via_drawing; };\n");
  std::printf("inline constexpr ClearCase kClearCases[] = {\n");
  for (uint32_t f : formats) {
    for (uint64_t v : values) {
      float c[4];
      bool d;
      ref::ColourClear(f, v, c, d);
      const xc::HostColourClear m = xc::HostColourClearValue(xc::ColorFormat(f), v);
      for (int j = 0; j < 4; ++j) {
        CHECK(std::bit_cast<uint32_t>(m.rgba[j]) == std::bit_cast<uint32_t>(c[j]), "clear f%u v%016" PRIX64 " c%d", f, v, j);
      }
      CHECK(m.via_drawing == d, "clear via_drawing f%u", f);
      std::printf("    {%u, 0x%016" PRIX64 "ull, {0x%08X, 0x%08X, 0x%08X, 0x%08X}, %s},\n", f, v,
                  std::bit_cast<uint32_t>(c[0]), std::bit_cast<uint32_t>(c[1]), std::bit_cast<uint32_t>(c[2]),
                  std::bit_cast<uint32_t>(c[3]), d ? "true" : "false");
    }
  }
  std::printf("};\n\n}  // namespace golden_colour\n");

  std::fprintf(stderr, "%s: %d mismatches\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
