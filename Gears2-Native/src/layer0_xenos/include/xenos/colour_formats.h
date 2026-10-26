// Layer 0 (game-free): Xenos colour render-target formats as bits and numbers - 7e3 (k_2_10_10_10_FLOAT) encode and
// decode, the -32...32 fixed-point k_16_16 / k_16_16_16_16 formats, unorm / fp16 / fp32 formats, the per-pixel colour
// resolve pipeline (decode -> PWL degamma -> sample average -> exponent bias -> red/blue swap -> encode -> endian
// swap), the raw fast-path condition, colour <-> depth raw bit layout and the 40-sample column swap, resolve-clear
// values, and the host-fp16-7e3 -> guest-7e3 helper used by M2 comparisons. Plain CPU code, standard library only;
// xenos/renderer/colour_hlsl.h holds the same arithmetic as HLSL text.
//
// Our code is MIT. Behaviour follows Xenia Canary e6cd2e4bc (BSD-3-Clause, Copyright (c) Ben Vanik and Xenia project
// contributors), re-expressed here from (paths relative to src/xenia/gpu/):
//   dxbc_shader_translator_om.cc 3816-3889  PreClampedFloat32To7e3 / UnclampedFloat32To7e3 (DirectXTex-derived)
//   dxbc_shader_translator_om.cc 1436-1449, 1600-1625  ROV k_16_16 unpack / pack
//   xenos.cc 104-120                        Float7e3To32
//   xenos.h 613-642                         IsColorResolveFormatBitwiseEquivalent
//   shaders/pixel_formats.xesli 104-126, 128-353, 356-447  XePackFixed, XePack32bpp4Pixels, XePack64bpp4Pixels, unpacks
//   shaders/resolve.xesli 148-150, 282-700, 740-888  dest info, sample unpacks, PWL decode, averaging, bias, swap
//   shaders/endian.xesli 38-66              XeEndianSwap32 / XeEndianSwap64
//   draw_util.cc 1665-1676, 1715-1762       decode_pwl_gamma, +5 bias hack, raw fast-path condition
//   d3d12/d3d12_render_target_cache.cc 1535-1543 (64bpp clear dword order), 3254-3270 (column swap),
//   3493-3517 (7e3 transfer encode), 3586-3589 / 3974-3986 (colour <-> depth bits), 5353-5502 (resolve-clear values)
//
// Where the rexglue SDK (our reference emulator, an older Canary derivative; /tmp/claude-0/sdk-bd-wt) differs, the
// function takes a `Reference` and does what each one does. The M2 comparison must use Reference::kSdk; the
// Canary/hardware-leaning variant is kept for D23. SDK sources read: src/graphics/xenos.cpp (Float7e3To32, same),
// src/graphics/pipeline/shader/dxbc_translator_om.cpp 2762-2830 (7e3 encode, same), src/graphics/util/draw.cpp
// 1116-1131 (fast path), src/graphics/d3d12/render_target_cache.cpp 1349 (clear order), 2740-2757 (column swap, same),
// 4575-4660 (clear values, same), and the disassembly in the SDK's prebuilt
// src/graphics/shaders/bytecode/d3d12_5_1/resolve_full_{32,64}bpp_cs.h (the SDK ships no resolve shader source).
// Differences (README has the list):
//   1. 16_16 / 16_16_16_16 resolve decode: SDK max(-1, i*32/32767) (old Xenia), Canary max(-32, i*32/32767).
//   2. 8_8_8_8_GAMMA resolve source: SDK does not PWL-degamma (bytes /255 then re-encoded), Canary degammas per sample.
//   3. Resolve dest encode of fixed formats: SDK always unsigned-fraction saturate*max+0.5, ignoring
//      RB_COPY_DEST_INFO.copy_dest_number; Canary honours the number format (signed / integer). SDK has no case for
//      k_16_16_EDRAM (13) / k_16_16_16_16_EDRAM (21) dests (they fall through to raw 32-bit float bits).
//   4. Fast path: SDK = bias 0 + single sample + bitwise-equivalent format. Canary additionally excludes gamma sources
//      and requires the number format to match.
//   5. 64bpp resolve-clear value: SDK RB_COLOR_CLEAR | RB_COLOR_CLEAR_LO << 32; Canary RB_COLOR_CLEAR_LO |
//      RB_COLOR_CLEAR << 32 (32bpp: both RB_COLOR_CLEAR).
//
// Floating point: build with -ffp-contract=off (/fp:precise). The shaders' `mad` (unorm encode, alpha 2-bit) may be
// fused by a GPU driver; these functions do a separate multiply and add (see README "unsure").
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

#include "xenos/edram.h"  // edram::ColorFormat, edram::Msaa, edram::kTileWidthSamples
#include "xenos/gamma.h"  // gamma::Saturate, gamma::PWLGammaToLinear

namespace xenos::colour {

using edram::ColorFormat;

// Which emulator's behaviour to reproduce where they differ.
enum class Reference : uint8_t {
  kSdk,     // rexglue SDK - what M2 compares against
  kCanary,  // Xenia Canary e6cd2e4bc - closer to hardware where the two differ (D23)
};

struct Float4 {
  float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
  friend constexpr bool operator==(const Float4&, const Float4&) = default;
};

// ---------------------------------------------------------------------------------------------------------------------
// Shader-style helpers. GPU min/max return the non-NaN operand; written as comparisons so NaN and -0 behave the same
// on every compiler and in the HLSL copy (max(-0, 0) is +0 here; DXBC max may return either).
// ---------------------------------------------------------------------------------------------------------------------
constexpr float MaxGpu(float x, float lo) { return x > lo ? x : lo; }  // NaN -> lo
constexpr float MinGpu(float x, float hi) { return x < hi ? x : hi; }  // NaN -> hi
inline float Saturate(float v) { return gamma::Saturate(v); }           // NaN -> +0, -0 -> +0

// Float -> integer as DXBC ftou / ftoi for the in-range values these functions produce (truncation toward zero).
constexpr uint32_t FToU(float f) { return f > 0.0f ? (f >= 4294967040.0f ? 0xFFFFFFFFu : uint32_t(f)) : 0u; }
constexpr int32_t FToI(float f) {
  if (!(f == f)) return 0;
  if (f >= 2147483520.0f) return INT32_MAX;
  if (f <= -2147483648.0f) return INT32_MIN;
  return int32_t(f);
}

// ---------------------------------------------------------------------------------------------------------------------
// fp16 (f16tof32 / f32tof16)
// ---------------------------------------------------------------------------------------------------------------------
// Exact; fp16 denormals kept (D3D10+ keeps them on conversion).
constexpr float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16;
  const uint32_t exp = (h >> 10) & 0x1Fu, mant = h & 0x3FFu;
  uint32_t bits;
  if (exp == 0x1F) {
    bits = sign | 0x7F800000u | (mant << 13);
  } else if (exp != 0) {
    bits = sign | ((exp + 112u) << 23) | (mant << 13);
  } else if (mant == 0) {
    bits = sign;
  } else {
    const uint32_t lz = uint32_t(std::countl_zero(mant)) - 21u;  // shift so the leading 1 lands on bit 10
    bits = sign | ((113u - lz) << 23) | (((mant << lz) & 0x3FFu) << 13);
  }
  return std::bit_cast<float>(bits);
}

// Round to nearest even, no clamp: |x| >= 65520 -> Inf, NaN -> NaN (payload truncated, kept non-zero). Matches IEEE
// binary32 -> binary16 conversion; what f32tof16 does on a given GPU for NaN payloads is not specified.
constexpr uint16_t FloatToHalf(float f) {
  const uint32_t u = std::bit_cast<uint32_t>(f);
  const uint32_t sign = (u >> 16) & 0x8000u;
  const uint32_t a = u & 0x7FFFFFFFu;
  if (a > 0x7F800000u) {
    const uint32_t m = (a >> 13) & 0x3FFu;
    return uint16_t(sign | 0x7C00u | m | (m ? 0u : 0x200u));
  }
  if (a >= 0x477FF000u) return uint16_t(sign | 0x7C00u);  // 65520 and up (and Inf)
  if (a < 0x38800000u) {                                  // below 2^-14: fp16 denormal or zero
    if (a <= 0x33000000u) return uint16_t(sign);          // <= 2^-25 (the tie at 2^-25 goes to even 0)
    const uint32_t m = (a & 0x7FFFFFu) | 0x800000u;
    const uint32_t shift = 126u - (a >> 23);  // 14..24
    uint32_t r = m >> shift;
    const uint32_t rem = m & ((1u << shift) - 1u), half = 1u << (shift - 1u);
    if (rem > half || (rem == half && (r & 1u))) ++r;
    return uint16_t(sign | r);
  }
  const uint32_t b = a + 0xC8000000u;  // rebias exponent 127 -> 15 (wraps)
  return uint16_t(sign | ((b + 0x0FFFu + ((b >> 13) & 1u)) >> 13));
}

// ---------------------------------------------------------------------------------------------------------------------
// 1. 7e3 (k_2_10_10_10_FLOAT and _AS_16_16_16_16): RGB 7e3 float (3-bit exponent, bias 3, no sign, no Inf/NaN,
//    range [0, 31.875]), alpha 2-bit unsigned fraction in bits 30-31.
// ---------------------------------------------------------------------------------------------------------------------
constexpr float k7e3Max = 31.875f;

// om.cc PreClampedFloat32To7e3: input already in [0, 31.875]. Below 0.25 -> denormal; round to nearest even.
constexpr uint32_t PreClampedFloat32To7e3(float f) {
  const uint32_t u = std::bit_cast<uint32_t>(f);
  uint32_t biased;
  if (u < 0x3E800000u) {
    const uint32_t shift = std::min(125u - (u >> 23), 24u);
    biased = ((u & 0x7FFFFFu) | 0x800000u) >> shift;
  } else {
    biased = u + 0xC2000000u;  // exponent - 124
  }
  return ((biased + 0x7FFFu + ((biased >> 16) & 1u)) >> 16) & 0x3FFu;
}

// om.cc UnclampedFloat32To7e3: max(x, 0) then min(x, 31.875) (NaN -> 0), then the above.
constexpr uint32_t Float32To7e3(float f) { return PreClampedFloat32To7e3(MinGpu(MaxGpu(f, 0.0f), k7e3Max)); }

// xenos.cc Float7e3To32.
constexpr float Float7e3To32(uint32_t f10) {
  f10 &= 0x3FFu;
  if (!f10) return 0.0f;
  uint32_t mantissa = f10 & 0x7Fu;
  uint32_t exponent = f10 >> 7;
  if (!exponent) {
    const uint32_t lzcnt = uint32_t(std::countl_zero(mantissa)) - (32u - 8u);
    exponent = uint32_t(1 - int32_t(lzcnt));
    mantissa = (mantissa << lzcnt) & 0x7Fu;
  }
  return std::bit_cast<float>(((exponent + 124u) << 23) | (mantissa << 16));
}

// 2-bit alpha: uint(saturate(a) * 3 + 0.5) (render_target_cache.cc 3509-3517), decode a / 3 (times 1/3).
inline uint32_t FloatToAlpha2(float a) {
  float t = Saturate(a) * 3.0f;
  t = t + 0.5f;
  return FToU(t) & 3u;
}
constexpr float Alpha2ToFloat(uint32_t a2) { return float(a2 & 3u) * (1.0f / 3.0f); }

inline uint32_t Pack7e3(const Float4& c) {
  return Float32To7e3(c.r) | (Float32To7e3(c.g) << 10) | (Float32To7e3(c.b) << 20) | (FloatToAlpha2(c.a) << 30);
}
constexpr Float4 Unpack7e3(uint32_t p) {
  return {Float7e3To32(p), Float7e3To32(p >> 10), Float7e3To32(p >> 20), Alpha2ToFloat(p >> 30)};
}

// The range a hardware 7e3 render target holds (the ROV path's clamp, render_target_cache.cc 250-254). Canary's and
// the SDK's default RTV path keep unclamped fp16 on the render target, so: off to match the emulators, on for D23.
inline Float4 Clamp7e3RenderTargetRange(const Float4& c, bool enable) {
  if (!enable) return c;
  return {MinGpu(MaxGpu(c.r, 0.0f), k7e3Max), MinGpu(MaxGpu(c.g, 0.0f), k7e3Max), MinGpu(MaxGpu(c.b, 0.0f), k7e3Max),
          Saturate(c.a)};
}

// 7. Host R16G16B16A16_FLOAT render-target texel (how both emulators store 7e3) -> the guest 7e3 word the emulator
//    would write when the data leaves the render target (EDRAM dump / ownership transfer, render_target_cache.cc
//    3493-3517): f16tof32, UnclampedFloat32To7e3 for RGB, saturate*3+0.5 for alpha. Compare 7e3 targets in these terms.
inline uint32_t Host7e3Fp16ToGuest(uint16_t r, uint16_t g, uint16_t b, uint16_t a) {
  return Pack7e3({HalfToFloat(r), HalfToFloat(g), HalfToFloat(b), HalfToFloat(a)});
}
inline uint32_t Host7e3Fp16ToGuest(uint64_t rgba16) {  // R in bits 0-15 ... A in bits 48-63 (D3D memory order)
  return Host7e3Fp16ToGuest(uint16_t(rgba16), uint16_t(rgba16 >> 16), uint16_t(rgba16 >> 32), uint16_t(rgba16 >> 48));
}
// And back (guest -> host transfer, render_target_cache.cc 3938-3946): every 7e3 RGB value is exact in fp16.
inline uint64_t Guest7e3ToHostFp16(uint32_t p) {
  const Float4 c = Unpack7e3(p);
  return uint64_t(FloatToHalf(c.r)) | (uint64_t(FloatToHalf(c.g)) << 16) | (uint64_t(FloatToHalf(c.b)) << 32) |
         (uint64_t(FloatToHalf(c.a)) << 48);
}

// ---------------------------------------------------------------------------------------------------------------------
// 2. k_16_16 / k_16_16_16_16 fixed point, guest range -32...32 (0x8001 = -32, 0x7FFF = 32; xenos.h 303-306).
// ---------------------------------------------------------------------------------------------------------------------
// Resolve / texture decode (pixel_formats.xesli 422-447): max(lo, int16 * 32/32767). Canary lo = -32 (0x8000 and 0x8001
// both -32); SDK lo = -1 (old Xenia: everything below -1 becomes -1).
constexpr float Fixed16ResolveDecode(uint32_t bits16, Reference ref) {
  const int32_t i = int32_t(int16_t(uint16_t(bits16)));
  return MaxGpu(float(i) * (32.0f / 32767.0f), ref == Reference::kCanary ? -32.0f : -1.0f);
}
// The ROV unpack (om.cc 1436-1449): no clamp, 0x8000 -> -32.000977.
constexpr float Fixed16RovDecode(uint32_t bits16) { return float(int32_t(int16_t(uint16_t(bits16)))) * (32.0f / 32767.0f); }
// Guest encode as the ROV path (om.cc 1600-1625 after the [-32, 32] clamp of render_target_cache.cc 271):
// clamp, v * 32767/32 +- 0.5, truncate. NaN -> -32 (0x8001), like DXBC max(NaN, -32).
inline uint32_t Fixed16GuestEncode(float v) {
  v = MinGpu(MaxGpu(v, -32.0f), 32.0f);
  float t = v * (32767.0f / 32.0f);
  t = t + (v >= 0.0f ? 0.5f : -0.5f);
  return uint32_t(FToI(t)) & 0xFFFFu;
}
// Host path (snorm16_render_target_full_range, default on in both): R16G16[B16A16]_SNORM, shader colour exp bias - 5
// (x 1/32). D3D float -> SNORM: NaN -> 0, clamp [-1, 1], x 32767, round to nearest (even here; D3D allows a tolerance).
// `shader_value` is the guest-scale value the shader computed (before the -5 bias).
inline uint32_t HostSnorm16FromShaderValue(float shader_value) {
  const float host = shader_value * (1.0f / 32.0f);  // the exp-bias-5 multiply, exact
  if (!(host == host)) return 0;
  const double c = std::clamp(double(host), -1.0, 1.0) * 32767.0;
  return uint32_t(int32_t(std::nearbyint(c))) & 0xFFFFu;
}
// Host SNORM bits read back through the integer view are the guest bits (host 1.0 = guest 32, -1.0 = 0x8001 = -32).
constexpr float HostSnorm16ToGuestValue(uint32_t bits16, Reference ref) { return Fixed16ResolveDecode(bits16, ref); }

// ---------------------------------------------------------------------------------------------------------------------
// 3. Other formats
// ---------------------------------------------------------------------------------------------------------------------
constexpr float UNorm8ToFloat(uint32_t v) { return float(v & 0xFFu) * (1.0f / 255.0f); }
constexpr float UNorm10ToFloat(uint32_t v) { return float(v & 0x3FFu) * (1.0f / 1023.0f); }
// saturate(v) * max + 0.5, truncated (XePackFixed unsigned fraction; SDK resolve `mov_sat; mad; ftou`).
inline uint32_t FloatToUNorm(float v, uint32_t bits) {
  const uint32_t umax = (1u << bits) - 1u;
  float t = Saturate(v) * float(umax);
  t = t + 0.5f;
  return FToU(t) & umax;
}

// Canary XePackFixed (pixel_formats.xesli 104-126): `num_format` = RB_COPY_DEST_INFO.copy_dest_number.
enum class NumFormat : uint32_t {
  kUnsignedRepeatingFraction = 0,
  kSignedRepeatingFraction = 1,
  kUnsignedInteger = 2,
  kSignedInteger = 3,
  kFloat = 7,
};
inline uint32_t PackFixedCanary(float v, uint32_t bits, NumFormat nf) {
  const uint32_t umax = (1u << bits) - 1u;
  uint32_t packed;
  if (nf == NumFormat::kSignedRepeatingFraction) {
    const float pmax = float((1u << (bits - 1u)) - 1u);
    float t = MinGpu(MaxGpu(v, -1.0f), 1.0f) * pmax;
    t = t + (v >= 0.0f ? 0.5f : -0.5f);
    packed = uint32_t(FToI(t));
  } else if (nf == NumFormat::kUnsignedInteger) {
    packed = FToU(MinGpu(MaxGpu(v, 0.0f), float(umax)) + 0.5f);
  } else if (nf == NumFormat::kSignedInteger) {
    const float smin = -float(1u << (bits - 1u)), smax = float((1u << (bits - 1u)) - 1u);
    float t = MinGpu(MaxGpu(v, smin), smax);
    t = t + (v >= 0.0f ? 0.5f : -0.5f);
    packed = uint32_t(FToI(t));
  } else {
    return FloatToUNorm(v, bits);
  }
  return packed & umax;
}
inline uint32_t PackFixed(float v, uint32_t bits, NumFormat nf, Reference ref) {
  return ref == Reference::kCanary ? PackFixedCanary(v, bits, nf) : FloatToUNorm(v, bits);
}

constexpr uint32_t FloatBits(float f) { return std::bit_cast<uint32_t>(f); }
constexpr float BitsFloat(uint32_t u) { return std::bit_cast<float>(u); }

// ---------------------------------------------------------------------------------------------------------------------
// 4. Colour resolve
// ---------------------------------------------------------------------------------------------------------------------
// Destination texture formats a 32bpp / 64bpp resolve writes (xenos::ColorFormat numbering).
enum class DestFormat : uint32_t {
  k_8_8_8_8 = 6,
  k_2_10_10_10 = 7,
  k_16_16_EDRAM = 13,
  k_8_8_8_8_A = 14,
  k_10_11_11 = 16,
  k_11_11_10 = 17,
  k_16_16_16_16_EDRAM = 21,
  k_16_16 = 25,
  k_16_16_16_16 = 26,
  k_16_16_FLOAT = 31,
  k_16_16_16_16_FLOAT = 32,
  k_32_FLOAT = 36,
  k_32_32_FLOAT = 37,
  k_8_8_8_8_AS_16_16_16_16 = 50,
  k_2_10_10_10_AS_16_16_16_16 = 54,
  k_10_11_11_AS_16_16_16_16 = 55,
  k_11_11_10_AS_16_16_16_16 = 56,
};
// 0 for formats this header does not cover (8/16/128bpp destinations).
constexpr uint32_t DestBytesPerPixel(DestFormat f) {
  switch (f) {
    case DestFormat::k_16_16_16_16_EDRAM:
    case DestFormat::k_16_16_16_16:
    case DestFormat::k_16_16_16_16_FLOAT:
    case DestFormat::k_32_32_FLOAT:
      return 8;
    case DestFormat::k_8_8_8_8:
    case DestFormat::k_2_10_10_10:
    case DestFormat::k_16_16_EDRAM:
    case DestFormat::k_8_8_8_8_A:
    case DestFormat::k_10_11_11:
    case DestFormat::k_11_11_10:
    case DestFormat::k_16_16:
    case DestFormat::k_16_16_FLOAT:
    case DestFormat::k_32_FLOAT:
    case DestFormat::k_8_8_8_8_AS_16_16_16_16:
    case DestFormat::k_2_10_10_10_AS_16_16_16_16:
    case DestFormat::k_10_11_11_AS_16_16_16_16:
    case DestFormat::k_11_11_10_AS_16_16_16_16:
      return 4;
  }
  return 0;
}

enum class Endian128 : uint32_t { kNone = 0, k8in16 = 1, k8in32 = 2, k16in32 = 3, k8in64 = 4, k8in128 = 5 };

// RB_COPY_DEST_INFO (SDK registers.h 872-879).
struct CopyDestInfo {
  Endian128 endian = Endian128::kNone;  // bits 0-2
  DestFormat format = DestFormat::k_8_8_8_8;  // bits 7-12
  NumFormat number = NumFormat::kUnsignedRepeatingFraction;  // bits 13-15
  int32_t exp_bias = 0;  // bits 16-21, signed
  bool swap = false;     // bit 24
};
constexpr int32_t CopyDestExpBias(uint32_t reg) { return int32_t(reg << 10) >> 26; }  // sign-extend bits 16-21
constexpr CopyDestInfo DecodeCopyDestInfo(uint32_t reg) {
  CopyDestInfo i;
  i.endian = Endian128(reg & 7u);
  i.format = DestFormat((reg >> 7) & 63u);
  i.number = NumFormat((reg >> 13) & 7u);
  i.exp_bias = CopyDestExpBias(reg);
  i.swap = ((reg >> 24) & 1u) != 0;
  return i;
}
// 2^bias built as float bits (resolve.xesli 148-150; SDK `ibfe; ishl 23; iadd 0x3f800000`). Exact for -32..31.
constexpr float ExpBiasFactor(int32_t bias) { return std::bit_cast<float>(uint32_t(bias) * (1u << 23) + 0x3F800000u); }
// The -1...1 hack (fixed_rg16/rgba16_truncated_to_minus_1_to_1, off by default) adds 5, capped at 31 (draw_util.cc
// 1666-1675; SDK draw.cpp same).
constexpr int32_t ResolveExpBias(int32_t reg_bias, ColorFormat src, bool fixed16_truncated_hack) {
  if (fixed16_truncated_hack && (src == ColorFormat::k_16_16 || src == ColorFormat::k_16_16_16_16)) {
    return std::min(reg_bias + 5, 31);
  }
  return reg_bias;
}

// Step 1: one EDRAM sample -> float4 (resolve.xesli 282-311, 452-470; SDK resolve_full_32bpp disassembly). For 32bpp
// sources only `lo` is used; for 64bpp sources `lo` is the first dword. Missing components are 0 on this RGBA path
// (alpha 1.0 only on the 8bpp-destination scalar path, not covered here).
constexpr Float4 DecodeSample(ColorFormat src, uint32_t lo, uint32_t hi, Reference ref) {
  switch (src) {
    case ColorFormat::k_8_8_8_8:
    case ColorFormat::k_8_8_8_8_GAMMA:
      return {UNorm8ToFloat(lo), UNorm8ToFloat(lo >> 8), UNorm8ToFloat(lo >> 16), UNorm8ToFloat(lo >> 24)};
    case ColorFormat::k_2_10_10_10:
    case ColorFormat::k_2_10_10_10_AS_10_10_10_10:
      return {UNorm10ToFloat(lo), UNorm10ToFloat(lo >> 10), UNorm10ToFloat(lo >> 20), Alpha2ToFloat(lo >> 30)};
    case ColorFormat::k_2_10_10_10_FLOAT:
    case ColorFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return Unpack7e3(lo);
    case ColorFormat::k_16_16:
      return {Fixed16ResolveDecode(lo, ref), Fixed16ResolveDecode(lo >> 16, ref), 0.0f, 0.0f};
    case ColorFormat::k_16_16_FLOAT:
      return {HalfToFloat(uint16_t(lo)), HalfToFloat(uint16_t(lo >> 16)), 0.0f, 0.0f};
    case ColorFormat::k_16_16_16_16:
      return {Fixed16ResolveDecode(lo, ref), Fixed16ResolveDecode(lo >> 16, ref), Fixed16ResolveDecode(hi, ref),
              Fixed16ResolveDecode(hi >> 16, ref)};
    case ColorFormat::k_16_16_16_16_FLOAT:
      return {HalfToFloat(uint16_t(lo)), HalfToFloat(uint16_t(lo >> 16)), HalfToFloat(uint16_t(hi)),
              HalfToFloat(uint16_t(hi >> 16))};
    case ColorFormat::k_32_32_FLOAT:
      return {BitsFloat(lo), BitsFloat(hi), 0.0f, 0.0f};
    case ColorFormat::k_32_FLOAT:
    default:
      return {BitsFloat(lo), 0.0f, 0.0f, 0.0f};
  }
}

// Step 2: Canary degammas 8_8_8_8_GAMMA sources (RGB only) before averaging; the SDK does not.
constexpr bool SourceDecodesPWLGamma(ColorFormat src, Reference ref) {
  return ref == Reference::kCanary && src == ColorFormat::k_8_8_8_8_GAMMA;
}
inline Float4 DegammaSample(const Float4& s, ColorFormat src, Reference ref) {
  if (!SourceDecodesPWLGamma(src, ref)) return s;
  return {gamma::PWLGammaToLinear(s.r), gamma::PWLGammaToLinear(s.g), gamma::PWLGammaToLinear(s.b), s.a};
}

// Steps 3-5: sum the selected samples in sample-index order ((s0 + s1) + s2) + s3, halve the bias factor per doubling,
// one multiply at the end, then the red/blue swap (resolve.xesli 768-810; SDK disassembly lines 615-1161 the same).
// `count` is 1, 2 or 4 (the sanitised sample select: k0..k3 -> 1, k01/k23 -> 2, k0123 -> 4).
inline Float4 CombineSamples(const Float4* s, uint32_t count, int32_t exp_bias, bool swap) {
  float bias = ExpBiasFactor(exp_bias);
  Float4 p = s[0];
  auto add = [](Float4& x, const Float4& y) {
    x.r = x.r + y.r;
    x.g = x.g + y.g;
    x.b = x.b + y.b;
    x.a = x.a + y.a;
  };
  if (count >= 2) {
    bias *= 0.5f;
    add(p, s[1]);
    if (count >= 4) {
      bias *= 0.5f;
      add(p, s[2]);
      add(p, s[3]);
    }
  }
  p = {p.r * bias, p.g * bias, p.b * bias, p.a * bias};
  if (swap) std::swap(p.r, p.b);
  return p;
}

// Step 6: encode to the destination (pixel_formats.xesli 128-353 / SDK disassembly 1163-1305). 32bpp destinations
// return the dword in the low 32 bits; 64bpp destinations return dword 0 in the low half.
inline uint64_t EncodeDest(const Float4& p, DestFormat f, NumFormat nf, Reference ref) {
  auto fx = [&](float v, uint32_t bits) { return uint64_t(PackFixed(v, bits, nf, ref)); };
  auto h2 = [](float x, float y) { return uint64_t(FloatToHalf(x)) | (uint64_t(FloatToHalf(y)) << 16); };
  switch (f) {
    case DestFormat::k_8_8_8_8:
    case DestFormat::k_8_8_8_8_A:
    case DestFormat::k_8_8_8_8_AS_16_16_16_16:
      return fx(p.r, 8) | (fx(p.g, 8) << 8) | (fx(p.b, 8) << 16) | (fx(p.a, 8) << 24);
    case DestFormat::k_2_10_10_10:
    case DestFormat::k_2_10_10_10_AS_16_16_16_16:
      return fx(p.r, 10) | (fx(p.g, 10) << 10) | (fx(p.b, 10) << 20) | (fx(p.a, 2) << 30);
    case DestFormat::k_10_11_11:  // XePackR11G11B10: R 11, G 11, B 10
    case DestFormat::k_10_11_11_AS_16_16_16_16:
      return fx(p.r, 11) | (fx(p.g, 11) << 11) | (fx(p.b, 10) << 22);
    case DestFormat::k_11_11_10:  // XePackR10G11B11: R 10, G 11, B 11
    case DestFormat::k_11_11_10_AS_16_16_16_16:
      return fx(p.r, 10) | (fx(p.g, 11) << 10) | (fx(p.b, 11) << 21);
    case DestFormat::k_16_16_EDRAM:
      if (ref == Reference::kSdk) return FloatBits(p.r);  // no SDK case: default (32_FLOAT)
      [[fallthrough]];
    case DestFormat::k_16_16:
      return fx(p.r, 16) | (fx(p.g, 16) << 16);
    case DestFormat::k_16_16_FLOAT:
      return h2(p.r, p.g);
    case DestFormat::k_16_16_16_16_EDRAM:
      if (ref == Reference::kSdk) return uint64_t(FloatBits(p.r)) | (uint64_t(FloatBits(p.g)) << 32);
      [[fallthrough]];
    case DestFormat::k_16_16_16_16:
      return (fx(p.r, 16) | (fx(p.g, 16) << 16)) | ((fx(p.b, 16) | (fx(p.a, 16) << 16)) << 32);
    case DestFormat::k_16_16_16_16_FLOAT:
      return h2(p.r, p.g) | (h2(p.b, p.a) << 32);
    case DestFormat::k_32_32_FLOAT:
      return uint64_t(FloatBits(p.r)) | (uint64_t(FloatBits(p.g)) << 32);
    case DestFormat::k_32_FLOAT:
    default:
      return FloatBits(p.r);
  }
}

// Step 7: endian swap on store (endian.xesli 38-66).
constexpr uint32_t EndianSwap32(uint32_t v, Endian128 e) {
  if (e == Endian128::k8in16 || e == Endian128::k8in32) v = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
  if (e == Endian128::k8in32 || e == Endian128::k16in32) v = (v << 16) | (v >> 16);
  return v;
}
// 64bpp: dword 0 in the low half. 8in64 swaps the dwords, then 8in32.
constexpr uint64_t EndianSwap64(uint64_t v, Endian128 e) {
  if (e == Endian128::k8in64) {
    v = (v >> 32) | (v << 32);
    e = Endian128::k8in32;
  }
  return uint64_t(EndianSwap32(uint32_t(v), e)) | (uint64_t(EndianSwap32(uint32_t(v >> 32), e)) << 32);
}

// The whole pixel. `words` holds `count` samples in sample-index order, one dword each for 32bpp sources, two
// (dword 0, dword 1) for 64bpp. Returns what lands in memory (after the endian swap), 4 or 8 bytes per DestBytesPerPixel.
struct ResolveOptions {
  Reference ref = Reference::kSdk;
  bool fixed16_truncated_hack = false;  // see ResolveExpBias
};
inline uint64_t ResolveColourPixel(ColorFormat src, const uint32_t* words, uint32_t count, uint32_t rb_copy_dest_info,
                                   const ResolveOptions& o = {}) {
  const CopyDestInfo d = DecodeCopyDestInfo(rb_copy_dest_info);
  const bool src64 = edram::Is64bpp(src);
  Float4 s[4];
  for (uint32_t i = 0; i < count && i < 4; ++i) {
    const uint32_t lo = words[src64 ? i * 2 : i], hi = src64 ? words[i * 2 + 1] : 0u;
    s[i] = DegammaSample(DecodeSample(src, lo, hi, o.ref), src, o.ref);
  }
  const Float4 p = CombineSamples(s, count, ResolveExpBias(d.exp_bias, src, o.fixed16_truncated_hack), d.swap);
  const uint64_t packed = EncodeDest(p, d.format, d.number, o.ref);
  return DestBytesPerPixel(d.format) == 8 ? EndianSwap64(packed, d.endian) : EndianSwap32(uint32_t(packed), d.endian);
}

// Raw fast path (a byte copy) - xenos.h IsColorResolveFormatBitwiseEquivalent (identical in both) plus draw_util.cc
// 1735-1762 (Canary) / SDK draw.cpp 1125-1131.
constexpr bool IsColorResolveFormatBitwiseEquivalent(ColorFormat rt, DestFormat d) {
  switch (rt) {
    case ColorFormat::k_8_8_8_8:
    case ColorFormat::k_8_8_8_8_GAMMA:
      return d == DestFormat::k_8_8_8_8 || d == DestFormat::k_8_8_8_8_A || d == DestFormat::k_8_8_8_8_AS_16_16_16_16;
    case ColorFormat::k_2_10_10_10:
    case ColorFormat::k_2_10_10_10_AS_10_10_10_10:
      return d == DestFormat::k_2_10_10_10 || d == DestFormat::k_2_10_10_10_AS_16_16_16_16;
    case ColorFormat::k_16_16_FLOAT:
      return d == DestFormat::k_16_16_FLOAT;
    case ColorFormat::k_16_16_16_16_FLOAT:
      return d == DestFormat::k_16_16_16_16_FLOAT;
    case ColorFormat::k_32_FLOAT:
      return d == DestFormat::k_32_FLOAT;
    case ColorFormat::k_32_32_FLOAT:
      return d == DestFormat::k_32_32_FLOAT;
    default:
      return false;
  }
}
// Canary ColorResolveNumberFormatMatches (draw_util.cc 1719-1732), for the formats above.
constexpr bool ColorResolveNumberFormatMatches(DestFormat d, NumFormat nf) {
  switch (d) {
    case DestFormat::k_16_16_FLOAT:
    case DestFormat::k_16_16_16_16_FLOAT:
    case DestFormat::k_32_FLOAT:
    case DestFormat::k_32_32_FLOAT:
      return nf == NumFormat::kFloat;
    default:
      return nf == NumFormat::kUnsignedRepeatingFraction;
  }
}
// `single_sample` = the sanitised sample select is k0..k3. Depth resolves always take the fast path (not covered).
constexpr bool ColourResolveIsRaw(ColorFormat rt, uint32_t rb_copy_dest_info, bool single_sample, Reference ref) {
  const CopyDestInfo d = DecodeCopyDestInfo(rb_copy_dest_info);
  if (d.exp_bias != 0 || !single_sample || !IsColorResolveFormatBitwiseEquivalent(rt, d.format)) return false;
  if (ref == Reference::kCanary) {
    if (rt == ColorFormat::k_8_8_8_8_GAMMA) return false;
    if (!ColorResolveNumberFormatMatches(d.format, d.number)) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// 5. Colour <-> depth raw bits, and the column swap
// ---------------------------------------------------------------------------------------------------------------------
// A 32bpp colour dword seen as depth/stencil (render_target_cache.cc 3974-3986: ubfe 24 @ 8, ubfe 8 @ 0) and back
// (3586-3589: bfi 24 @ 8).
struct DepthStencilBits {
  uint32_t depth24 = 0, stencil = 0;
  friend constexpr bool operator==(const DepthStencilBits&, const DepthStencilBits&) = default;
};
constexpr DepthStencilBits DepthStencilFromColourWord(uint32_t w) { return {w >> 8, w & 0xFFu}; }
constexpr uint32_t ColourWordFromDepthStencil(uint32_t depth24, uint32_t stencil) {
  return ((depth24 & 0xFFFFFFu) << 8) | (stencil & 0xFFu);
}
// Depth tiles store their two 40-sample halves swapped relative to colour, so a colour <-> depth transfer swaps the
// halves of the source pixel x within its tile (render_target_cache.cc 3254-3270; SDK 2740-2757 identical).
// `x_in_tile` is in source pixels (host pixels with resolution scale).
constexpr uint32_t SwapTileHalfColumns(uint32_t x_in_tile, edram::Msaa source_msaa, bool source_is_64bpp,
                                       uint32_t resolution_scale_x = 1) {
  const uint32_t pixel_width_dwords_log2 = uint32_t(source_msaa >= edram::Msaa::k4X) + uint32_t(source_is_64bpp);
  const uint32_t half = (edram::kTileWidthSamples * resolution_scale_x) >> (1u + pixel_width_dwords_log2);
  return x_in_tile < half ? x_in_tile + half : x_in_tile - half;
}

// ---------------------------------------------------------------------------------------------------------------------
// 6. Resolve-clear values
// ---------------------------------------------------------------------------------------------------------------------
// The packed value (32bpp formats: RB_COLOR_CLEAR in both).
constexpr uint64_t ColourClearValue(bool format_is_64bpp, uint32_t rb_color_clear, uint32_t rb_color_clear_lo,
                                    Reference ref) {
  if (ref == Reference::kSdk) return uint64_t(rb_color_clear) | (uint64_t(rb_color_clear_lo) << 32);
  return format_is_64bpp ? (uint64_t(rb_color_clear_lo) | (uint64_t(rb_color_clear) << 32)) : uint64_t(rb_color_clear);
}
// Depth clear (RB_DEPTH_CLEAR): depth24 = bits 8-31, stencil = bits 0-7.
constexpr DepthStencilBits DepthClearValue(uint32_t rb_depth_clear) { return DepthStencilFromColourWord(rb_depth_clear); }

// The host ClearRenderTargetView colour (identical in both, render_target_cache.cc 5385-5465). The 16-bit formats are
// cleared through their UINT view, so the floats are the raw 16-bit integers; 32_FLOAT / 32_32_FLOAT likewise, and
// need a uint draw instead (via_drawing) when a value is not exact as a float (> 2^24).
struct HostColourClear {
  float rgba[4] = {};
  bool via_drawing = false;
  bool through_uint_view = false;
};
inline HostColourClear HostColourClearValue(ColorFormat f, uint64_t v) {
  HostColourClear c;
  switch (f) {
    case ColorFormat::k_8_8_8_8:
    case ColorFormat::k_8_8_8_8_GAMMA:
      for (uint32_t j = 0; j < 4; ++j) c.rgba[j] = float((v >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
      if (f == ColorFormat::k_8_8_8_8_GAMMA) {  // linear in R16G16B16A16_UNORM (gamma_render_target_as_unorm16)
        for (uint32_t j = 0; j < 3; ++j) c.rgba[j] = gamma::PWLGammaToLinear(c.rgba[j]);
      }
      break;
    case ColorFormat::k_2_10_10_10:
    case ColorFormat::k_2_10_10_10_AS_10_10_10_10:
      for (uint32_t j = 0; j < 3; ++j) c.rgba[j] = float((v >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
      c.rgba[3] = float((v >> 30) & 0x3) * (1.0f / 0x3);
      break;
    case ColorFormat::k_2_10_10_10_FLOAT:
    case ColorFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      for (uint32_t j = 0; j < 3; ++j) c.rgba[j] = Float7e3To32(uint32_t(v >> (j * 10)) & 0x3FF);
      c.rgba[3] = float((v >> 30) & 0x3) * (1.0f / 0x3);
      break;
    case ColorFormat::k_16_16:
    case ColorFormat::k_16_16_FLOAT:
      for (uint32_t j = 0; j < 2; ++j) c.rgba[j] = float((v >> (j * 16)) & 0xFFFF);
      c.through_uint_view = true;
      break;
    case ColorFormat::k_16_16_16_16:
    case ColorFormat::k_16_16_16_16_FLOAT:
      for (uint32_t j = 0; j < 4; ++j) c.rgba[j] = float((v >> (j * 16)) & 0xFFFF);
      c.through_uint_view = true;
      break;
    case ColorFormat::k_32_FLOAT:
      c.rgba[0] = float(uint32_t(v));
      c.via_drawing = uint64_t(c.rgba[0]) != uint32_t(v);
      c.through_uint_view = true;
      break;
    case ColorFormat::k_32_32_FLOAT:
      c.rgba[0] = float(uint32_t(v));
      c.rgba[1] = float(uint32_t(v >> 32));
      c.via_drawing = uint64_t(c.rgba[0]) != uint32_t(v) || uint64_t(c.rgba[1]) != uint32_t(v >> 32);
      c.through_uint_view = true;
      break;
  }
  return c;
}

}  // namespace xenos::colour
