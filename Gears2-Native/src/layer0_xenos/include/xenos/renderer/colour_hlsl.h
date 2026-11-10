// Layer 0 (game-free): HLSL text for the colour-format arithmetic in xenos/colour_formats.h, as raw string literals the
// D3D12 backend pastes into its shaders (HLSL 2021, SM 6.x). Each function mirrors the C++ function of the same name
// without the `Xe` prefix; see that header for sources (Xenia Canary e6cd2e4bc, BSD-3-Clause, Copyright (c) Ben Vanik
// and Xenia project contributors; re-expressed here, our code is MIT) and for where the rexglue SDK and Canary differ.
// `canary` parameters select Reference::kCanary (true) or Reference::kSdk (false, what M2 compares against).
//
// Order: kHlslColourHelpers first, then any of the others; kHlslColourAll is all of them in a valid order.
// Format numbers: source = edram::ColorFormat (0-7, 10, 12, 14, 15), dest = colour::DestFormat (xenos::ColorFormat).
//
// Differences from the C++ that are the GPU's, not ours: f16tof32 / f32tof16 are the hardware conversions (the C++ is
// IEEE round-to-nearest-even; D3D does not pin f32tof16 rounding or NaN payloads), and `precise` stops DXC and the driver
// fusing a*b+c into one FMA so results match the C++ built with -ffp-contract=off. The emulators' own shaders use DXBC
// `mad`, which a driver may fuse - only exact .5 boundaries can be affected.
#pragma once

namespace xenos::renderer::hlsl {

inline constexpr const char* kHlslColourHelpers = R"hlsl(
// ---- colour_formats.h helpers --------------------------------------------------------------------------------------
float XeMaxGpu(float x, float lo) { return x > lo ? x : lo; }  // NaN -> lo, max(-0, 0) = +0
float XeMinGpu(float x, float hi) { return x < hi ? x : hi; }  // NaN -> hi
uint XeFToU(float f) { return f > 0.0f ? uint(f) : 0u; }      // values here are always < 2^32
int XeFToI(float f) { return f == f ? int(f) : 0; }
)hlsl";

inline constexpr const char* kHlsl7e3 = R"hlsl(
// ---- 7e3 (k_2_10_10_10_FLOAT): om.cc PreClampedFloat32To7e3 / UnclampedFloat32To7e3, xenos.cc Float7e3To32 --------
uint XePreClampedFloat32To7e3(float f) {
  uint u = asuint(f);
  uint biased;
  if (u < 0x3E800000u) {
    uint shift = min(125u - (u >> 23u), 24u);
    biased = ((u & 0x7FFFFFu) | 0x800000u) >> shift;
  } else {
    biased = u + 0xC2000000u;
  }
  return ((biased + 0x7FFFu + ((biased >> 16u) & 1u)) >> 16u) & 0x3FFu;
}
uint XeFloat32To7e3(float f) { return XePreClampedFloat32To7e3(XeMinGpu(XeMaxGpu(f, 0.0f), 31.875f)); }
float XeFloat7e3To32(uint f10) {
  f10 &= 0x3FFu;
  uint mantissa = f10 & 0x7Fu;
  uint exponent = f10 >> 7u;
  if (exponent == 0u && mantissa != 0u) {
    uint lzcnt = 7u - firstbithigh(mantissa);
    exponent = uint(1 - int(lzcnt));
    mantissa = (mantissa << lzcnt) & 0x7Fu;
  }
  return f10 != 0u ? asfloat(((exponent + 124u) << 23u) | (mantissa << 16u)) : 0.0f;
}
uint XeFloatToAlpha2(float a) {
  precise float t = saturate(a) * 3.0f;
  t = t + 0.5f;
  return XeFToU(t) & 3u;
}
float XeAlpha2ToFloat(uint a2) { return float(a2 & 3u) * (1.0f / 3.0f); }
uint XePack7e3(float4 c) {
  return XeFloat32To7e3(c.r) | (XeFloat32To7e3(c.g) << 10u) | (XeFloat32To7e3(c.b) << 20u) |
         (XeFloatToAlpha2(c.a) << 30u);
}
float4 XeUnpack7e3(uint p) {
  return float4(XeFloat7e3To32(p), XeFloat7e3To32(p >> 10u), XeFloat7e3To32(p >> 20u), XeAlpha2ToFloat(p >> 30u));
}
// Hardware 7e3 render-target range (ROV clamp); off to match the emulators' RTV path, on for D23.
float4 XeClamp7e3RenderTargetRange(float4 c, bool enable) {
  if (!enable) return c;
  return float4(XeMinGpu(XeMaxGpu(c.r, 0.0f), 31.875f), XeMinGpu(XeMaxGpu(c.g, 0.0f), 31.875f),
                XeMinGpu(XeMaxGpu(c.b, 0.0f), 31.875f), saturate(c.a));
}
// Host R16G16B16A16_FLOAT texel (x = R | G << 16, y = B | A << 16) -> guest 7e3 word, for M2 comparisons.
uint XeHost7e3Fp16ToGuest(uint2 rgba16) {
  return XePack7e3(float4(f16tof32(rgba16.x), f16tof32(rgba16.x >> 16u), f16tof32(rgba16.y), f16tof32(rgba16.y >> 16u)));
}
uint2 XeGuest7e3ToHostFp16(uint p) {
  float4 c = XeUnpack7e3(p);
  return uint2(f32tof16(c.r) | (f32tof16(c.g) << 16u), f32tof16(c.b) | (f32tof16(c.a) << 16u));
}
)hlsl";

inline constexpr const char* kHlslFixed16 = R"hlsl(
// ---- k_16_16 / k_16_16_16_16 fixed point, guest -32...32 ------------------------------------------------------------
// Resolve decode: SDK max(-1, i * 32/32767) (old Xenia); Canary (42c2b88, hardware-tested; patch 0023): 10 fractional
// bits, i / 1024, no clamp (0x8000 = -32).
float XeFixed16ResolveDecode(uint bits16, bool canary) {
  int i = int(bits16 << 16u) >> 16;
  return canary ? float(i) * (1.0f / 1024.0f) : XeMaxGpu(float(i) * (32.0f / 32767.0f), -1.0f);
}
// Guest encode (ROV formula): clamp [-32, 32], v * 32767/32 +- 0.5, truncate.
uint XeFixed16GuestEncode(float v) {
  v = XeMinGpu(XeMaxGpu(v, -32.0f), 32.0f);
  precise float t = v * (32767.0f / 32.0f);
  t = t + (v >= 0.0f ? 0.5f : -0.5f);
  return uint(XeFToI(t)) & 0xFFFFu;
}
)hlsl";

inline constexpr const char* kHlslPack = R"hlsl(
// ---- fixed-point packing: Canary XePackFixed (pixel_formats.xesli 104-126), SDK unorm only -------------------------
float XeUNorm8ToFloat(uint v) { return float(v & 0xFFu) * (1.0f / 255.0f); }
float XeUNorm10ToFloat(uint v) { return float(v & 0x3FFu) * (1.0f / 1023.0f); }
uint XeFloatToUNorm(float v, uint bits) {
  uint umax = (1u << bits) - 1u;
  precise float t = saturate(v) * float(umax);
  t = t + 0.5f;
  return XeFToU(t) & umax;
}
// nf: 0 unsigned fraction, 1 signed fraction, 2 unsigned integer, 3 signed integer, 7 float.
uint XePackFixedCanary(float v, uint bits, uint nf) {
  uint umax = (1u << bits) - 1u;
  uint packed;
  if (nf == 1u) {
    float pmax = float((1u << (bits - 1u)) - 1u);
    precise float t = XeMinGpu(XeMaxGpu(v, -1.0f), 1.0f) * pmax;
    t = t + (v >= 0.0f ? 0.5f : -0.5f);
    packed = uint(XeFToI(t));
  } else if (nf == 2u) {
    packed = XeFToU(XeMinGpu(XeMaxGpu(v, 0.0f), float(umax)) + 0.5f);
  } else if (nf == 3u) {
    float smin = -float(1u << (bits - 1u));
    float smax = float((1u << (bits - 1u)) - 1u);
    precise float t = XeMinGpu(XeMaxGpu(v, smin), smax);
    t = t + (v >= 0.0f ? 0.5f : -0.5f);
    packed = uint(XeFToI(t));
  } else {
    return XeFloatToUNorm(v, bits);
  }
  return packed & umax;
}
uint XePackFixed(float v, uint bits, uint nf, bool canary) {
  return canary ? XePackFixedCanary(v, bits, nf) : XeFloatToUNorm(v, bits);
}
)hlsl";

inline constexpr const char* kHlslResolve = R"hlsl(
// ---- colour resolve: decode -> PWL degamma -> average -> 2^bias -> R/B swap -> encode -> endian -------------------
int XeCopyDestExpBias(uint rb_copy_dest_info) { return int(rb_copy_dest_info << 10u) >> 26; }
float XeExpBiasFactor(int bias) { return asfloat(uint(bias) * (1u << 23u) + 0x3F800000u); }
// The -1...1 hack (off by default): +5, capped at 31, for 16_16 (4) / 16_16_16_16 (5) sources.
int XeResolveExpBias(int reg_bias, uint src_format, bool fixed16_truncated_hack) {
  return (fixed16_truncated_hack && (src_format == 4u || src_format == 5u)) ? min(reg_bias + 5, 31) : reg_bias;
}
// One EDRAM sample; `hi` is the second dword of 64bpp formats. Missing components are 0.
float4 XeDecodeSample(uint src_format, uint lo, uint hi, bool canary) {
  switch (src_format) {
    case 0u:  // 8_8_8_8
    case 1u:  // 8_8_8_8_GAMMA
      return float4(XeUNorm8ToFloat(lo), XeUNorm8ToFloat(lo >> 8u), XeUNorm8ToFloat(lo >> 16u),
                    XeUNorm8ToFloat(lo >> 24u));
    case 2u:   // 2_10_10_10
    case 10u:  // 2_10_10_10_AS_10_10_10_10
      return float4(XeUNorm10ToFloat(lo), XeUNorm10ToFloat(lo >> 10u), XeUNorm10ToFloat(lo >> 20u),
                    XeAlpha2ToFloat(lo >> 30u));
    case 3u:   // 2_10_10_10_FLOAT
    case 12u:  // 2_10_10_10_FLOAT_AS_16_16_16_16
      return XeUnpack7e3(lo);
    case 4u:  // 16_16
      return float4(XeFixed16ResolveDecode(lo, canary), XeFixed16ResolveDecode(lo >> 16u, canary), 0.0f, 0.0f);
    case 6u:  // 16_16_FLOAT
      return float4(f16tof32(lo), f16tof32(lo >> 16u), 0.0f, 0.0f);
    case 5u:  // 16_16_16_16
      return float4(XeFixed16ResolveDecode(lo, canary), XeFixed16ResolveDecode(lo >> 16u, canary),
                    XeFixed16ResolveDecode(hi, canary), XeFixed16ResolveDecode(hi >> 16u, canary));
    case 7u:  // 16_16_16_16_FLOAT
      return float4(f16tof32(lo), f16tof32(lo >> 16u), f16tof32(hi), f16tof32(hi >> 16u));
    case 15u:  // 32_32_FLOAT
      return float4(asfloat(lo), asfloat(hi), 0.0f, 0.0f);
    default:  // 32_FLOAT
      return float4(asfloat(lo), 0.0f, 0.0f, 0.0f);
  }
}
// gamma.h PWLGammaToLinear.
float XePWLGammaToLinear(float g) {
  g = saturate(g);
  float scale, offset;
  if (g >= 96.0f / 255.0f) {
    if (g >= 192.0f / 255.0f) { scale = 8.0f / 1024.0f; offset = -1024.0f; }
    else { scale = 4.0f / 1024.0f; offset = -256.0f; }
  } else {
    if (g >= 64.0f / 255.0f) { scale = 2.0f / 1024.0f; offset = -64.0f; }
    else { scale = 1.0f / 1024.0f; offset = 0.0f; }
  }
  precise float l = g * ((255.0f * 1024.0f) * scale);
  l = l + offset;
  l += trunc(l * scale);
  l *= 1.0f / 1023.0f;
  return l;
}
// Canary degammas 8_8_8_8_GAMMA (1) sources per sample, RGB only; the SDK does not.
float4 XeDegammaSample(float4 s, uint src_format, bool canary) {
  if (!canary || src_format != 1u) return s;
  return float4(XePWLGammaToLinear(s.r), XePWLGammaToLinear(s.g), XePWLGammaToLinear(s.b), s.a);
}
// Patch 0023 (Canary 534aeb6): 8_8_8_8_GAMMA samples stay 10-bit linear codes until the MSAA average; the averaged code
// goes through the PWL curve again (gamma code, then back to linear) before exp_bias; alpha stays on the normal path.
uint XePWLGammaToLinearCode(uint c) {
  uint scale = c < 64u ? 1u : c < 96u ? 2u : c < 192u ? 4u : 8u;
  uint offset = c < 64u ? 0u : c < 96u ? 64u : c < 192u ? 256u : 1024u;
  uint linear_code = c * scale - offset;
  return linear_code + ((linear_code * scale) >> 10u);
}
float3 XePWLGammaToLinearCode3(float3 g) {
  return float3(XePWLGammaToLinearCode(uint(g.x * 255.0f + 0.5f)), XePWLGammaToLinearCode(uint(g.y * 255.0f + 0.5f)),
                XePWLGammaToLinearCode(uint(g.z * 255.0f + 0.5f)));
}
float XeResolveAveragePWLGamma(float sum, float sample_weight) {
  uint m = uint(sum * sample_weight);
  uint c = m < 64u ? m : m < 128u ? (m + 64u) >> 1u : m < 512u ? (m + 256u) >> 2u : (m + 1024u) >> 3u;
  c = min(c, 255u);
  uint n = XePWLGammaToLinearCode(c);
  return n == 1023u ? 1.0f : float((n << 14u) | (n << 4u) | (n >> 6u)) * (1.0f / 16777216.0f);
}
// ((s0 + s1) + s2) + s3, bias halved per doubling, one multiply, then the swap. count = 1, 2 or 4.
float4 XeCombineSamples(float4 s0, float4 s1, float4 s2, float4 s3, uint count, int exp_bias, bool swap) {
  float bias = XeExpBiasFactor(exp_bias);
  precise float4 p = s0;
  if (count >= 2u) {
    bias *= 0.5f;
    p = p + s1;
    if (count >= 4u) {
      bias *= 0.5f;
      p = p + s2;
      p = p + s3;
    }
  }
  p = p * bias;
  return swap ? p.bgra : p;
}
uint XeDestBytesPerPixel(uint f) {
  if (f == 21u || f == 26u || f == 32u || f == 37u) return 8u;
  if (f == 6u || f == 7u || f == 13u || f == 14u || f == 16u || f == 17u || f == 25u || f == 31u || f == 36u ||
      f == 50u || f == 54u || f == 55u || f == 56u) return 4u;
  return 0u;
}
// Encode to the destination texture format. 32bpp results are in .x.
uint2 XeEncodeDest(float4 p, uint f, uint nf, bool canary) {
  // If-chain, not a switch (Steam Deck / Proton): DXC merges the cases' tails and vkd3d-proton's dxil-spirv then
  // writes SPIR-V that breaks the structured control-flow rules - the colour resolve shaders failed or ended the game.
  uint2 r = uint2(asuint(p.r), 0u);  // 32_FLOAT (the default)
  if (f == 6u || f == 14u || f == 50u) {  // 8_8_8_8, 8_8_8_8_A, 8_8_8_8_AS_16_16_16_16
    r = uint2(XePackFixed(p.r, 8u, nf, canary) | (XePackFixed(p.g, 8u, nf, canary) << 8u) |
                 (XePackFixed(p.b, 8u, nf, canary) << 16u) | (XePackFixed(p.a, 8u, nf, canary) << 24u), 0u);
  } else if (f == 7u || f == 54u) {  // 2_10_10_10
    r = uint2(XePackFixed(p.r, 10u, nf, canary) | (XePackFixed(p.g, 10u, nf, canary) << 10u) |
                 (XePackFixed(p.b, 10u, nf, canary) << 20u) | (XePackFixed(p.a, 2u, nf, canary) << 30u), 0u);
  } else if (f == 16u || f == 55u) {  // 10_11_11: R 11, G 11, B 10
    r = uint2(XePackFixed(p.r, 11u, nf, canary) | (XePackFixed(p.g, 11u, nf, canary) << 11u) |
                 (XePackFixed(p.b, 10u, nf, canary) << 22u), 0u);
  } else if (f == 17u || f == 56u) {  // 11_11_10: R 10, G 11, B 11
    r = uint2(XePackFixed(p.r, 10u, nf, canary) | (XePackFixed(p.g, 11u, nf, canary) << 10u) |
                 (XePackFixed(p.b, 11u, nf, canary) << 21u), 0u);
  } else if (f == 13u) {  // 16_16_EDRAM: no SDK case (raw R)
    r = canary ? uint2(XePackFixed(p.r, 16u, nf, true) | (XePackFixed(p.g, 16u, nf, true) << 16u), 0u) : uint2(asuint(p.r), 0u);
  } else if (f == 25u) {  // 16_16
    r = uint2(XePackFixed(p.r, 16u, nf, canary) | (XePackFixed(p.g, 16u, nf, canary) << 16u), 0u);
  } else if (f == 31u) {  // 16_16_FLOAT
    r = uint2(f32tof16(p.r) | (f32tof16(p.g) << 16u), 0u);
  } else if (f == 21u) {  // 16_16_16_16_EDRAM: no SDK case (raw RG)
    r = canary ? uint2(XePackFixed(p.r, 16u, nf, true) | (XePackFixed(p.g, 16u, nf, true) << 16u),
                       XePackFixed(p.b, 16u, nf, true) | (XePackFixed(p.a, 16u, nf, true) << 16u))
               : asuint(p.rg);
  } else if (f == 26u) {  // 16_16_16_16
    r = uint2(XePackFixed(p.r, 16u, nf, canary) | (XePackFixed(p.g, 16u, nf, canary) << 16u),
                 XePackFixed(p.b, 16u, nf, canary) | (XePackFixed(p.a, 16u, nf, canary) << 16u));
  } else if (f == 32u) {  // 16_16_16_16_FLOAT
    r = uint2(f32tof16(p.r) | (f32tof16(p.g) << 16u), f32tof16(p.b) | (f32tof16(p.a) << 16u));
  } else if (f == 37u) {  // 32_32_FLOAT
    r = asuint(p.rg);
  }
  return r;
}
// endian: 0 none, 1 8in16, 2 8in32, 3 16in32, 4 8in64 (64bpp only).
uint XeEndianSwap32(uint v, uint e) {
  if (e == 1u || e == 2u) v = ((v & 0x00FF00FFu) << 8u) | ((v & 0xFF00FF00u) >> 8u);
  if (e == 2u || e == 3u) v = (v << 16u) | (v >> 16u);
  return v;
}
uint2 XeEndianSwap64(uint2 v, uint e) {
  if (e == 4u) { v = v.yx; e = 2u; }
  return uint2(XeEndianSwap32(v.x, e), XeEndianSwap32(v.y, e));
}
// The whole pixel. 32bpp sources use lo only; 64bpp sources (lo, hi) per sample.
// Decode, degamma, average and exp-bias the samples, swap red/blue: the pixel a resolve then encodes (any destination).
float4 XeColourResolveCombine(uint src_format, uint4 lo, uint4 hi, uint count, uint rb_copy_dest_info, bool canary,
                              bool fixed16_truncated_hack) {
  float4 s0 = XeDegammaSample(XeDecodeSample(src_format, lo.x, hi.x, canary), src_format, canary);
  float4 s1 = XeDegammaSample(XeDecodeSample(src_format, lo.y, hi.y, canary), src_format, canary);
  float4 s2 = XeDegammaSample(XeDecodeSample(src_format, lo.z, hi.z, canary), src_format, canary);
  float4 s3 = XeDegammaSample(XeDecodeSample(src_format, lo.w, hi.w, canary), src_format, canary);
  int bias = XeResolveExpBias(XeCopyDestExpBias(rb_copy_dest_info), src_format, fixed16_truncated_hack);
  bool swap = ((rb_copy_dest_info >> 24u) & 1u) != 0u;
  float4 p;
  if (canary && src_format == 1u) {  // patch 0023: average the 10-bit codes, then the PWL round trip
    float4 d0 = XeDecodeSample(1u, lo.x, hi.x, true), d1 = XeDecodeSample(1u, lo.y, hi.y, true);
    float4 d2 = XeDecodeSample(1u, lo.z, hi.z, true), d3 = XeDecodeSample(1u, lo.w, hi.w, true);
    float3 sum = XePWLGammaToLinearCode3(d0.rgb);
    if (count >= 2u) sum += XePWLGammaToLinearCode3(d1.rgb);
    if (count >= 4u) sum += XePWLGammaToLinearCode3(d2.rgb) + XePWLGammaToLinearCode3(d3.rgb);
    float w = count >= 4u ? 0.25f : count >= 2u ? 0.5f : 1.0f;
    float a = XeCombineSamples(d0, d1, d2, d3, count, bias, false).a;
    float f = XeExpBiasFactor(bias);
    p = float4(XeResolveAveragePWLGamma(sum.x, w) * f, XeResolveAveragePWLGamma(sum.y, w) * f,
               XeResolveAveragePWLGamma(sum.z, w) * f, a);
    if (swap) p = p.bgra;
  } else {
    p = XeCombineSamples(s0, s1, s2, s3, count, bias, swap);
  }
  return p;
}
uint2 XeColourResolvePixel(uint src_format, uint4 lo, uint4 hi, uint count, uint rb_copy_dest_info, bool canary,
                           bool fixed16_truncated_hack) {
  float4 p = XeColourResolveCombine(src_format, lo, hi, count, rb_copy_dest_info, canary, fixed16_truncated_hack);
  uint dest_format = (rb_copy_dest_info >> 7u) & 63u;
  uint2 packed = XeEncodeDest(p, dest_format, (rb_copy_dest_info >> 13u) & 7u, canary);
  uint endian = rb_copy_dest_info & 7u;
  return XeDestBytesPerPixel(dest_format) == 8u ? XeEndianSwap64(packed, endian)
                                                : uint2(XeEndianSwap32(packed.x, endian), 0u);
}
// Patch 0030 (Canary resolve_full_8bpp / 16bpp / 128bpp, pixel_formats.xesli XePack16bpp4PixelsInUInt2): the
// destinations XeDestBytesPerPixel leaves at 0. 8bpp: one byte (red; k_8 with the swap bit takes alpha, k_8_A / k_8_B
// blue - Canary XeResolveLoad8ScalarColors), no endian swap. 16bpp: the 16-bit value, only an 8in16 swap. 128bpp
// (32_32_32_32_FLOAT): four words, XeEndianSwap128. Returns the bits in .x (low 8 / 16 bits) or .xyzw.
uint XeDestBytesPerPixelAny(uint f) {
  if (f == 2u || f == 8u || f == 9u) return 1u;
  if (f == 3u || f == 4u || f == 5u || f == 10u || f == 15u || f == 24u || f == 30u) return 2u;
  if (f == 38u) return 16u;
  return XeDestBytesPerPixel(f);
}
uint XeEndianSwap32Any(uint v, uint e) {
  if (e == 1u || e == 2u) v = ((v & 0x00FF00FFu) << 8u) | ((v & 0xFF00FF00u) >> 8u);
  if (e == 2u || e == 3u) v = (v << 16u) | (v >> 16u);
  return v;
}
uint4 XeColourResolvePixelOther(uint src_format, uint4 lo, uint4 hi, uint count, uint rb_copy_dest_info, bool canary) {
  float4 p = XeColourResolveCombine(src_format, lo, hi, count, rb_copy_dest_info, canary, false);
  uint f = (rb_copy_dest_info >> 7u) & 63u, nf = (rb_copy_dest_info >> 13u) & 7u, e = rb_copy_dest_info & 7u;
  bool swap = ((rb_copy_dest_info >> 24u) & 1u) != 0u;
  if (f == 2u || f == 8u || f == 9u) {
    // p is already red/blue swapped when the swap bit is set, so p.r is the source blue then. Patch 0087 (Canary
    // 8322658, with the Canary resolve rules): k_8_A and k_8_B resolve exactly like k_8 - alpha when swapped.
    float v = ((f == 2u || canary) && swap) ? p.a : p.r;
    return uint4(XePackFixed(v, 8u, nf, true), 0u, 0u, 0u);
  }
  if (f == 38u) {
    uint4 u = asuint(p);
    if (e == 5u) { u = u.wzyx; e = 2u; }  // 8in128
    if (e == 4u) { u = u.yxwz; e = 2u; }  // 8in64
    return uint4(XeEndianSwap32Any(u.x, e), XeEndianSwap32Any(u.y, e), XeEndianSwap32Any(u.z, e),
                 XeEndianSwap32Any(u.w, e));
  }
  uint v;
  switch (f) {
    case 3u: {  // 1_5_5_5: R5 G5 B5 A1 from bit 0
      v = XePackFixed(p.r, 5u, nf, true) | (XePackFixed(p.g, 5u, nf, true) << 5u) |
          (XePackFixed(p.b, 5u, nf, true) << 10u) | (XePackFixed(p.a, 1u, nf, true) << 15u);
      break;
    }
    case 4u:  // 5_6_5
      v = XePackFixed(p.r, 5u, nf, true) | (XePackFixed(p.g, 6u, nf, true) << 5u) | (XePackFixed(p.b, 5u, nf, true) << 11u);
      break;
    case 5u:  // 6_5_5: R5 G5 B6
      v = XePackFixed(p.r, 5u, nf, true) | (XePackFixed(p.g, 5u, nf, true) << 5u) | (XePackFixed(p.b, 6u, nf, true) << 10u);
      break;
    case 10u:  // 8_8
      v = XePackFixed(p.r, 8u, nf, true) | (XePackFixed(p.g, 8u, nf, true) << 8u);
      break;
    case 15u:  // 4_4_4_4
      v = XePackFixed(p.r, 4u, nf, true) | (XePackFixed(p.g, 4u, nf, true) << 4u) | (XePackFixed(p.b, 4u, nf, true) << 8u) |
          (XePackFixed(p.a, 4u, nf, true) << 12u);
      break;
    case 24u:  // 16
      v = XePackFixed(p.r, 16u, nf, true);
      break;
    default:  // 30 = 16_FLOAT (Canary's default case)
      v = f32tof16(p.r) & 0xFFFFu;
      break;
  }
  if (e == 1u) v = ((v & 0xFFu) << 8u) | ((v >> 8u) & 0xFFu);
  return uint4(v & 0xFFFFu, 0u, 0u, 0u);
}
// Raw fast path. single_sample = sanitised sample select is k0..k3.
bool XeColourResolveIsRaw(uint rt_format, uint rb_copy_dest_info, bool single_sample, bool canary) {
  uint d = (rb_copy_dest_info >> 7u) & 63u, nf = (rb_copy_dest_info >> 13u) & 7u;
  if (XeCopyDestExpBias(rb_copy_dest_info) != 0 || !single_sample) return false;
  bool eq;
  switch (rt_format) {
    case 0u: case 1u: eq = d == 6u || d == 14u || d == 50u; break;
    case 2u: case 10u: eq = d == 7u || d == 54u; break;
    case 6u: eq = d == 31u; break;
    case 7u: eq = d == 32u; break;
    case 14u: eq = d == 36u; break;
    case 15u: eq = d == 37u; break;
    default: eq = false; break;
  }
  if (!eq) return false;
  if (canary) {
    if (rt_format == 1u) return false;
    bool is_float = d == 31u || d == 32u || d == 36u || d == 37u;
    if (nf != (is_float ? 7u : 0u)) return false;
  }
  return true;
}
)hlsl";

inline constexpr const char* kHlslDepthBits = R"hlsl(
// ---- colour <-> depth raw bits, 40-sample column swap ---------------------------------------------------------------
uint2 XeDepthStencilFromColourWord(uint w) { return uint2(w >> 8u, w & 0xFFu); }  // (depth24, stencil)
uint XeColourWordFromDepthStencil(uint depth24, uint stencil) { return ((depth24 & 0xFFFFFFu) << 8u) | (stencil & 0xFFu); }
// msaa: 0 1x, 1 2x, 2 4x.
uint XeSwapTileHalfColumns(uint x_in_tile, uint msaa, bool source_is_64bpp, uint resolution_scale_x) {
  uint dwords_log2 = (msaa >= 2u ? 1u : 0u) + (source_is_64bpp ? 1u : 0u);
  uint half_width = (80u * resolution_scale_x) >> (1u + dwords_log2);
  return x_in_tile < half_width ? x_in_tile + half_width : x_in_tile - half_width;
}
)hlsl";

// Everything, in dependency order: helpers, 7e3, fixed16, pack, resolve, depth bits. Pasted as consecutive strings.
inline constexpr const char* kHlslColourAll[] = {kHlslColourHelpers, kHlsl7e3, kHlslFixed16, kHlslPack, kHlslResolve,
                                                 kHlslDepthBits};

}  // namespace xenos::renderer::hlsl
