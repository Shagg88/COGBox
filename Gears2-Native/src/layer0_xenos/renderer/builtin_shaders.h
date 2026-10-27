// Layer 0 (game-free): the renderer's own fixed HLSL shaders, compiled once through the shader cache.
//
// kRectListGs follows Xenia Canary e6cd2e4 src/xenia/gpu/d3d12/pipeline_cache.cc (the kRectangleList geometry shader:
// longest-edge choice, strip order, 4th vertex = -v[a] + v[b] + v[c] for every output, primitives with a NaN position
// dropped) and XeFloat32To20e4 in kDepthResolveCs is Canary's shaders/pixel_formats.xesli. Both are
// BSD-3-Clause: Copyright (c) Ben Vanik and Xenia project contributors. All rights reserved. Redistribution and use in
// source and binary forms, with or without modification, are permitted provided that the conditions of the BSD 3-Clause
// licence are met (THIRD_PARTY_NOTICES.md has the full text). The rest is our code (MIT).
#pragma once

namespace xenos::renderer::builtin {

// Rectangle list -> one 4-vertex strip per rectangle. Input/output = the translator's XeVsOut (notes/39).
constexpr const char* kRectListGs = R"(
struct XeVsOut {
  float4 interp[16] : TEXCOORD0;
  float4 position : SV_Position;
};
[maxvertexcount(4)]
void main(triangle XeVsOut v[3], inout TriangleStream<XeVsOut> stream) {
  [unroll] for (uint n = 0; n < 3u; ++n) {
    if (any(isnan(v[n].position))) return;
  }
  // Squared edge lengths, each product and sum rounded on its own (Canary uses DXBC add + dp2).
  precise float2 d12 = v[2].position.xy - v[1].position.xy;
  precise float2 d20 = v[0].position.xy - v[2].position.xy;
  precise float2 d01 = v[1].position.xy - v[0].position.xy;
  precise float e12 = d12.x * d12.x + d12.y * d12.y;
  precise float e20 = d20.x * d20.x + d20.y * d20.y;
  precise float e01 = d01.x * d01.x + d01.y * d01.y;
  uint3 o;
  if (e20 < e12 && e01 < e12) {
    o = uint3(0, 1, 2);
  } else {
    o = (e01 < e20) ? uint3(1, 2, 0) : uint3(2, 0, 1);
  }
  stream.Append(v[o.x]);
  stream.Append(v[o.y]);
  stream.Append(v[o.z]);
  XeVsOut f;
  [unroll] for (uint i = 0; i < 16u; ++i) {
    precise float4 t = -v[o.x].interp[i] + v[o.y].interp[i];
    f.interp[i] = t + v[o.z].interp[i];
  }
  precise float4 p = -v[o.x].position + v[o.y].position;
  f.position = p + v[o.z].position;
  stream.Append(f);
  stream.RestartStrip();
}
)";

// Job 238 (notes/60): fused scaled resolve. With --gow2_renderer_fused_scaled_resolve the resolve shaders' scaled pass
// (c3.y = 1) writes each word straight into a scaled-memory window (the kScaledWordsToMemCs addressing below) instead of
// the scaled destination texture, so the extra pass that copied that texture into the window (and its reads) is gone.
//   c2.z destination base, c2.w window base, c3.z destination pitch in texels | 0x80000000 (fused on),
//   c3.w destination endian (bits 0-1) | log2(bytes per texel) << 4
constexpr const char* kHlslScaledChunk = R"(
#define XE_CHUNK_WIDTH 16384u
#define XE_CHUNK_BYTES 0x2000000u
int XeChunkTiledCombine(int oib, uint bank, uint pipe, uint ylsb) {
  return int((ylsb << 4u) | (pipe << 6u) | (bank << 11u)) | (oib & 0xF) | (((oib >> 4) & 1) << 5) |
         (((oib >> 5) & 7) << 8) | ((oib >> 8) << 12);
}
int XeChunkTiledOffset2D(int x, int y, uint pitch, uint bpb_log2) {
  pitch = (pitch + 31u) & ~31u;
  int outer = ((y >> 5) * int(pitch >> 5) + (x >> 5)) << 6;
  int inner = (((y >> 1) & 7) << 3) | (x & 7);
  int oib = (outer | inner) << int(bpb_log2);
  uint bank = uint(y >> 4) & 1u;
  uint pipe = (uint(x >> 3) & 3u) ^ ((uint(y >> 3) & 1u) << 1u);
  return XeChunkTiledCombine(oib, bank, pipe, uint(y) & 1u);
}
uint XeChunkSwapWord(uint v, uint e) {
  if (e == 1u || e == 2u) v = ((v & 0x00FF00FFu) << 8u) | ((v & 0xFF00FF00u) >> 8u);
  if (e == 2u || e == 3u) v = (v << 16u) | (v >> 16u);
  return v;
}
// Flat word index in the window of word 0 of scaled host pixel id (rs = scale, origin = destination origin in guest
// texels), or 0xFFFFFFFF when that texel's address is outside the window.
uint XeChunkIndex(uint2 id, uint rs, uint2 origin, uint dest_base, uint window_base, uint pitch, uint bpl) {
  int gx = int(origin.x + id.x / rs), gy = int(origin.y + id.y / rs);
  uint sub = (id.y % rs) * rs + (id.x % rs);
  int off = XeChunkTiledOffset2D(gx, gy, pitch, bpl);
  if (off < 0) return 0xFFFFFFFFu;
  uint a = dest_base + uint(off);
  if (a < window_base || a - window_base >= XE_CHUNK_BYTES) return 0xFFFFFFFFu;
  uint words = bpl == 3u ? 2u : 1u;
  return ((a - window_base) * rs * rs + sub * 4u * words) >> 2u;
}
uint2 XeChunkCoord(uint i) { return uint2(i % XE_CHUNK_WIDTH, i / XE_CHUNK_WIDTH); }
)";

// Depth resolve, region to region: our depth target (D32_FLOAT + S8, or D24_UNORM + S8 for D24S8: the depth SRV then
// returns n / 0xFFFFFF and the round-to-nearest-even below gives n back exactly, as Canary's EDRAM dump does) -> an
// R32_UINT host texture holding the guest-format
// 32-bit words (24-bit depth << 8 | stencil, before the destination endian swap), one per destination block.
//   c0.xy  source origin (pixels), c0.zw size
//   c1.xy  destination origin (blocks), c1.z bit 0: D24FS8 (20e4, truncated as Canary's default depth_float24_round =
//          false), else D24S8 (unorm24, round to nearest even)
//   c1.w, c2.xy  de-jitter for the upscalers (see main); c3.xy resolution scale / scaled destination
constexpr const char* kDepthResolveCs = R"(
Texture2D<float> xe_depth : register(t0);
Texture2D<uint2> xe_stencil : register(t1);
RWTexture2D<uint> xe_dest : register(u0);
cbuffer XeResolveConstants : register(b0) { uint4 c0; uint4 c1; uint4 c2; uint4 c3; };

uint XeFloat32To20e4(uint f32u32, bool round_to_nearest_even) {
  f32u32 = min((f32u32 <= 0x7FFFFFFFu) ? f32u32 : 0u, 0x3FFFFFF8u);
  uint denormalized = ((f32u32 & 0x7FFFFFu) | 0x800000u) >> min(113u - (f32u32 >> 23u), 24u);
  uint f24u32 = (f32u32 < 0x38800000u) ? denormalized : (f32u32 + 0xC8000000u);
  if (round_to_nearest_even) {
    f24u32 += 3u + ((f24u32 >> 3u) & 1u);
  }
  return (f24u32 >> 3u) & 0xFFFFFFu;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  // c3.x: resolution scale (Stage 7; 0 = 1). c3.y = 0: one thread per guest pixel, reading its host sample nearest the
  // centre (the 1x destination); c3.y = 1: one thread per host pixel into the scaled destination (origins x scale).
  uint rs = max(c3.x, 1u);
  // Patch 0143 (upscaling modes): c3.y bits 8-15 = the source's own scale when it differs from the destination's
  // (a scene target below the output scale filling the scaled copy, nearest); 0 = the same (as before).
  uint ss = (c3.y >> 8u) & 0xFFu;
  if (ss == 0u) ss = rs;
  bool full = (c3.y & 0xFFu) != 0u;
  if (id.x >= (full ? c0.z * rs : c0.z) || id.y >= (full ? c0.w * rs : c0.w)) return;
  int3 src = int3(int2(full ? (c0.xy * rs + id.xy) * ss / rs : (c0.xy + id.xy) * rs + rs / 2u), 0);
  float d = xe_depth.Load(src);
  uint s = xe_stencil.Load(src).y & 0xFFu;
  // Upscalers (--gow2_upscale_dejitter_depth; upscale::DejitterDepth is the same arithmetic): c2.xy != 0 = write the
  // depth at the texel's unjittered centre. c2.xy = the frame's jitter (asfloat, host pixels, +y down), c1.w = asfloat
  // depth at infinity (a in depth = a - b / w), compared in guest convention; edge fraction 0.05 (kDejitterDepthEdge).
  // Only the non-fused paths read c2.xy (the job 238 fused path sets c2.zw / c3.zw, not c2.xy or c1.w).
  if (any(c2.xy != 0u)) {
    uint tw, th;
    xe_depth.GetDimensions(tw, th);
    float2 p = float2(src.xy) + asfloat(c2.xy);
    int2 i0 = int2(floor(p));
    float2 f = p - float2(i0);
    int2 hi = int2(tw, th) - 1;
    int3 q0 = int3(clamp(i0, 0, hi), 0), q1 = int3(clamp(i0 + int2(1, 0), 0, hi), 0);
    int3 q2 = int3(clamp(i0 + int2(0, 1), 0, hi), 0), q3 = int3(clamp(i0 + int2(1, 1), 0, hi), 0);
    float g = (c1.z & 3u) == 3u ? 2.0 : 1.0;  // a float24 target keeps guest * 0.5 (c1.z bit 1)
    float t0 = xe_depth.Load(q0) * g, t1 = xe_depth.Load(q1) * g, t2 = xe_depth.Load(q2) * g, t3 = xe_depth.Load(q3) * g;
    float a = asfloat(c1.w);
    float l0 = 1.0 / max(abs(a - t0), 1e-7), l1 = 1.0 / max(abs(a - t1), 1e-7);
    float l2 = 1.0 / max(abs(a - t2), 1e-7), l3 = 1.0 / max(abs(a - t3), 1e-7);
    float lmin = min(min(l0, l1), min(l2, l3)), lmax = max(max(l0, l1), max(l2, l3));
    if (lmax - lmin <= 0.05 * lmin) {
      float top = t0 + (t1 - t0) * f.x, bottom = t2 + (t3 - t2) * f.x;
      d = (top + (bottom - top) * f.y) / g;
    } else {
      int3 qn = f.y >= 0.5 ? (f.x >= 0.5 ? q3 : q2) : (f.x >= 0.5 ? q1 : q0);
      d = xe_depth.Load(qn);
      s = xe_stencil.Load(qn).y & 0xFFu;
    }
  }
  uint d24;
  if (c1.z & 1u) {
    // c1.z bit 1: the host keeps 20e4 depth as guest * 0.5 (the reference's full_float24_in_0_to_1 remap).
    precise float guest = (c1.z & 2u) ? d * 2.0 : d;
    d24 = XeFloat32To20e4(asuint(guest), false);
  } else {
    precise float scaled = saturate(d) * 16777215.0;
    d24 = min(uint(round(scaled)), 0xFFFFFFu);
  }
  if (full && (c3.z & 0x80000000u) != 0u) {  // job 238: fused - straight into the scaled-memory window
    uint i = XeChunkIndex(id.xy, rs, c1.xy, c2.z, c2.w, c3.z & 0xFFFFu, (c3.w >> 4u) & 7u);
    if (i != 0xFFFFFFFFu) xe_dest[XeChunkCoord(i)] = XeChunkSwapWord((d24 << 8u) | s, c3.w & 3u);
    return;
  }
  xe_dest[(full ? c1.xy * rs : c1.xy) + id.xy] = (d24 << 8u) | s;
}
)";

// Stand-in pixel shader for depth-only draws on comparison frames: writes the draw-ID diagnostic (xe_sys1.z, as the
// translated pixel shaders do when xe_sys1.w bit 0 is set) to colour 0. Input = the translator's pixel input and
// system constants (notes/39).
constexpr const char* kCoveragePs = R"(
cbuffer XeSystem : register(b0, space0) {
  uint4 xe_sys0;
  float4 xe_ndc_scale;
  float4 xe_ndc_offset;
  float4 xe_color_exp_scale;
  uint4 xe_sys1;
};
struct XePsIn {
  float4 interp[16] : TEXCOORD0;
  float4 position : SV_Position;
  bool front_face : SV_IsFrontFace;
};
// Slot 4: slots 0-3 are the guest's colour targets (M2), the draw-ID diagnostic comes after them.
float4 main(XePsIn xe_in) : SV_Target4 { return float4(asfloat(xe_sys1.z), 0.0, 0.0, 0.0); }
)";

// EDRAM ownership transfer, depth -> depth (upl-n099; D29: transfers for the reinterpretations Gears does). Drawn over
// tile rectangles of the destination target. For each destination pixel: its EDRAM dword (Canary edram.xesli for 1x
// 32bpp: 80x16 samples per tile, depth tiles with their 40-sample halves swapped, 2048 tiles wrapping), the source pixel
// holding that dword, the source's 32-bit guest word (24-bit depth << 8 | stencil, the resolve's conversion), read back
// in the destination's depth format. Depth goes out through SV_Depth; stencil is written by 8 more passes, one bit each
// (discard where the bit is 0, stencil op replace with the write mask = that bit), so SV_StencilRef is not needed.
//   t0.x dest base tiles, t0.y dest pitch tiles, t0.z source base tiles, t0.w source pitch tiles
//   t1.x source is D24FS8, t1.y dest is D24FS8, t1.z 1 = stencil-bit pass, t1.w the bit
constexpr const char* kTransferVs = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2(float((id << 1u) & 2u), float(id & 2u));
  return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";
constexpr const char* kTransferPs = R"(
cbuffer XeTransfer : register(b0, space0) { uint4 t0; uint4 t1; uint4 t2; uint4 t3; };
Texture2D<float> xe_src_depth : register(t0, space0);
Texture2D<uint2> xe_src_stencil : register(t1, space0);
// M2 build B: t2.x = 1: the source is a colour target (t2 texture, format t2.y): word = its EDRAM bits,
// depth24 = word >> 8, stencil = word & 0xFF (needs colour_hlsl.h + kHlslHostWords before this text).
Texture2D<float4> xe_src_colour : register(t2, space0);
// upl-n108: t2.z = 1: read the colour source through its UINT bits view (t3) instead.
Texture2D<uint4> xe_src_colour_bits : register(t3, space0);
// Gears 3 2026-10-23 (patch 0005): t2.w = 1: the colour source is 64bpp (40-pixel tiles, two dwords per pixel). The
// depth sample at 32bpp column u (after the colour/depth half swap) is half (u & 1) of source pixel u >> 1 (Canary
// d3d12_render_target_cache.cc: "the 32bpp destination sample is one half of the 64bpp source sample at
// u = u_32bpp >> 1"; the colour/depth column swap by 40 samples = 20 pixels of the 64bpp source).

uint XeFloat32To20e4(uint f32u32) {
  f32u32 = min((f32u32 <= 0x7FFFFFFFu) ? f32u32 : 0u, 0x3FFFFFF8u);
  uint denormalized = ((f32u32 & 0x7FFFFFu) | 0x800000u) >> min(113u - (f32u32 >> 23u), 24u);
  uint f24u32 = (f32u32 < 0x38800000u) ? denormalized : (f32u32 + 0xC8000000u);
  return (f24u32 >> 3u) & 0xFFFFFFu;
}
float XeFloat20e4To32(uint f24u32) {
  uint mantissa = f24u32 & 0xFFFFFu;
  uint exponent = f24u32 >> 20u;
  bool denormal = exponent == 0u;
  uint lz = 20u - firstbithigh(mantissa);
  exponent = denormal ? (1u - lz) : exponent;
  mantissa = denormal ? ((mantissa << lz) & 0xFFFFFu) : mantissa;
  return asfloat(f24u32 != 0u ? (((exponent + 112u) << 23u) | (mantissa << 3u)) : 0u);
}

#ifdef XE_STENCIL_REF
// One pass: depth and all 8 stencil bits (SV_StencilRef, PSSpecifiedStencilRefSupported).
void main(float4 pos : SV_Position, out float o_depth : SV_Depth, out uint o_stencil : SV_StencilRef) {
#else
void main(float4 pos : SV_Position, out float o_depth : SV_Depth) {
#endif
  // t3.x: resolution scale (Stage 7; 0 = 1): the EDRAM mapping works on guest pixels, the host sub-pixel is kept.
  uint rs = max(t3.x, 1u);
  uint2 hp = uint2(pos.xy);
  uint2 p = hp / rs, sub = hp % rs;
  // t3.y: the SOURCE target's scale (per-target scale threshold; 0 = same as the destination): source host pixel =
  // guest pixel x source scale + this sub-pixel mapped onto it (2x -> 1x takes the top-left sample, 1x -> 2x repeats).
  uint rss = t3.y != 0u ? t3.y : rs;
  uint2 ssub = (sub * rss) / rs;
  // Destination pixel -> EDRAM dword (1x, 32bpp, depth: halves swapped).
  uint sx = p.x % 80u, sy = p.y % 16u;
  sx = sx >= 40u ? sx - 40u : sx + 40u;
  uint tile = (t0.x + (p.y / 16u) * t0.y + p.x / 80u) % 2048u;
  // EDRAM dword -> source pixel.
  uint rel = (tile + 2048u - t0.z) % 2048u;
  // Depth source: the sample at EDRAM column sx is its pixel with the halves swapped back; colour: no swap.
  uint qx = t2.x != 0u ? sx : (sx >= 40u ? sx - 40u : sx + 40u);
  bool src64 = t2.x != 0u && t2.w != 0u;
  uint src_tw = src64 ? 40u : 80u;
  if (src64) qx = sx >> 1u;
  uint2 q = uint2((rel % t0.w) * src_tw + qx, (rel / t0.w) * 16u + sy) * rss + ssub;
  float d = 0.0;
  uint stencil = 0u, colour_word = 0u;
  [branch] if (t2.x != 0u) {
    uint2 cw = (t2.z != 0u) ? XeBitsToGuestWords(xe_src_colour_bits.Load(int3(q, 0)), t2.y)
                            : XeHostToGuestWords(xe_src_colour.Load(int3(q, 0)), t2.y);
    colour_word = (src64 && (sx & 1u) != 0u) ? cw.y : cw.x;
    stencil = colour_word & 0xFFu;
  } else {
    d = xe_src_depth.Load(int3(q, 0));
    stencil = xe_src_stencil.Load(int3(q, 0)).y & 0xFFu;
  }
  // Raw 24-bit guest word, no value conversion (Canary d3d12_render_target_cache.cc ~3560-3590, ~4140-4235):
  // source host depth -> word (D24S8: round to nearest even of d * 0xFFFFFF; D24FS8: 20e4, truncated), then the word
  // decoded in the destination format (D24S8: (n + (n >> 23)) * 2^-24, exact in float and what D3D turns back into
  // n; D24FS8: 20e4 -> float32).
  precise float scaled = saturate(d) * 16777215.0;
  // Bit 1 of the source/destination flags: that D24FS8 target keeps guest depth * 0.5 on the host (the reference's
  // full_float24_in_0_to_1 remap), so the whole 20e4 range [0, 2) survives a round trip through it.
  precise float guest = (t1.x & 2u) ? d * 2.0 : d;
  uint d24 = (t2.x != 0u) ? (colour_word >> 8u)
             : (t1.x & 1u) ? XeFloat32To20e4(asuint(guest)) : min(uint(round(scaled)), 0xFFFFFFu);
#ifdef XE_STENCIL_REF
  o_stencil = stencil;
#else
  if (t1.z != 0u && ((stencil >> t1.w) & 1u) == 0u) discard;
#endif
  float f24 = XeFloat20e4To32(d24);
  o_depth = (t1.y & 1u) ? ((t1.y & 2u) ? f24 * 0.5 : f24) : float(d24 + (d24 >> 23u)) * (1.0 / 16777216.0);
}
)";

// EDRAM ownership transfer between colour targets of the same storage class (M2 build A): a raw copy through the
// EDRAM mapping. Colour tiles have no 40-sample swap; a 64bpp tile is 40 pixels wide. The value goes through float
// in the same host format on both sides, which is exact for unorm8/unorm10/fp16/fp32 (NaN payloads aside).
//   t0.x dest base tiles, t0.y dest pitch tiles, t0.z source base tiles, t0.w source pitch tiles; t1.x tile width
constexpr const char* kColourTransferPs = R"(
cbuffer XeTransfer : register(b0, space0) { uint4 t0; uint4 t1; uint4 t2; uint4 t3; };
#ifdef XE_BITS_OUT
// upl-n108: float/SNORM16 formats: the UINT bits views on both sides, so NaN payloads and -32768 are kept.
Texture2D<uint4> xe_src : register(t0, space0);
uint4 main(float4 pos : SV_Position) : SV_Target0 {
#else
Texture2D<float4> xe_src : register(t0, space0);
float4 main(float4 pos : SV_Position) : SV_Target0 {
#endif
  uint rs = max(t3.x, 1u);  // t3.x: resolution scale (Stage 7; 0 = 1)
  uint2 hp = uint2(pos.xy);
  uint2 p = hp / rs, sub = hp % rs;
  // t3.y: the SOURCE target's scale (per-target scale threshold; 0 = same as the destination): source host pixel =
  // guest pixel x source scale + this sub-pixel mapped onto it (2x -> 1x takes the top-left sample, 1x -> 2x repeats).
  uint rss = t3.y != 0u ? t3.y : rs;
  uint2 ssub = (sub * rss) / rs;
  uint tw = max(t1.x, 1u);
  uint sx = p.x % tw, sy = p.y % 16u;
  uint tile = (t0.x + (p.y / 16u) * t0.y + p.x / tw) % 2048u;
  uint rel = (tile + 2048u - t0.z) % 2048u;
  uint2 q = uint2((rel % t0.w) * tw + sx, (rel / t0.w) * 16u + sy) * rss + ssub;
  return xe_src.Load(int3(q, 0));
}
)";

// M2 build B: the colour shaders are built from upl-n103's xenos/renderer/colour_hlsl.h (kHlslColourAll, our maths,
// verified against Canary/SDK) + kHlslHostWords + their body; the renderer concatenates them (ColourShaderSource).
//
// kHlslHostWords: one host render-target texel <-> the guest EDRAM word(s) of a colour storage class (the reference's
// dump/load: exact for every modelled format; 7e3 is quantised to 7e3 bits on the way out, which is what leaves a
// 7e3 render target in both emulators). fmt = xenos ColorRenderTargetFormat. 16_16 / 16_16_16_16 are host SNORM
// whose bits equal the guest bits (exp bias -5 in the shader, snorm16_render_target_full_range).
constexpr const char* kHlslHostWords = R"(
uint XeSnorm16Bits(float v) { return uint(int(round(clamp(v, -1.0f, 1.0f) * 32767.0f))) & 0xFFFFu; }
float XeSnorm16Value(uint bits) { return max(-1.0f, float(int(bits << 16u) >> 16) / 32767.0f); }
uint2 XeHostToGuestWords(float4 v, uint fmt) {
  switch (fmt) {
    case 0u: case 1u:
      return uint2(XeFloatToUNorm(v.r, 8u) | (XeFloatToUNorm(v.g, 8u) << 8u) | (XeFloatToUNorm(v.b, 8u) << 16u) |
                   (XeFloatToUNorm(v.a, 8u) << 24u), 0u);
    case 2u: case 10u:
      return uint2(XeFloatToUNorm(v.r, 10u) | (XeFloatToUNorm(v.g, 10u) << 10u) | (XeFloatToUNorm(v.b, 10u) << 20u) |
                   (XeFloatToUNorm(v.a, 2u) << 30u), 0u);
    case 3u: case 12u:
      return uint2(XeHost7e3Fp16ToGuest(uint2(f32tof16(v.r) | (f32tof16(v.g) << 16u),
                                              f32tof16(v.b) | (f32tof16(v.a) << 16u))), 0u);
    case 4u: return uint2(XeSnorm16Bits(v.r) | (XeSnorm16Bits(v.g) << 16u), 0u);
    case 5u:
      return uint2(XeSnorm16Bits(v.r) | (XeSnorm16Bits(v.g) << 16u), XeSnorm16Bits(v.b) | (XeSnorm16Bits(v.a) << 16u));
    case 6u: return uint2(f32tof16(v.r) | (f32tof16(v.g) << 16u), 0u);
    case 7u: return uint2(f32tof16(v.r) | (f32tof16(v.g) << 16u), f32tof16(v.b) | (f32tof16(v.a) << 16u));
    case 15u: return uint2(asuint(v.r), asuint(v.g));
    default: return uint2(asuint(v.r), 0u);  // 14 32_FLOAT
  }
}
float4 XeGuestWordsToHost(uint2 w, uint fmt) {
  switch (fmt) {
    case 0u: case 1u:
      return float4(XeUNorm8ToFloat(w.x), XeUNorm8ToFloat(w.x >> 8u), XeUNorm8ToFloat(w.x >> 16u),
                    XeUNorm8ToFloat(w.x >> 24u));
    case 2u: case 10u:
      return float4(XeUNorm10ToFloat(w.x), XeUNorm10ToFloat(w.x >> 10u), XeUNorm10ToFloat(w.x >> 20u),
                    XeAlpha2ToFloat(w.x >> 30u));
    case 3u: case 12u: {
      uint2 h = XeGuest7e3ToHostFp16(w.x);
      return float4(f16tof32(h.x), f16tof32(h.x >> 16u), f16tof32(h.y), f16tof32(h.y >> 16u));
    }
    case 4u: return float4(XeSnorm16Value(w.x), XeSnorm16Value(w.x >> 16u), 0.0f, 1.0f);
    case 5u:
      return float4(XeSnorm16Value(w.x), XeSnorm16Value(w.x >> 16u), XeSnorm16Value(w.y), XeSnorm16Value(w.y >> 16u));
    case 6u: return float4(f16tof32(w.x), f16tof32(w.x >> 16u), 0.0f, 1.0f);
    case 7u: return float4(f16tof32(w.x), f16tof32(w.x >> 16u), f16tof32(w.y), f16tof32(w.y >> 16u));
    case 15u: return float4(asfloat(w.x), asfloat(w.y), 0.0f, 1.0f);
    default: return float4(asfloat(w.x), 0.0f, 0.0f, 1.0f);
  }
}
// upl-n108: the raw channels of a UINT view of a 16/32-bit float or SNORM16 target (formats 4-7, 14, 15) <-> words,
// so transfers keep NaN payloads and SNORM -32768 (Canary GetColorOwnershipTransferDXGIFormat).
uint2 XeBitsToGuestWords(uint4 c, uint fmt) {
  switch (fmt) {
    case 4u: case 6u: return uint2((c.r & 0xFFFFu) | (c.g << 16u), 0u);
    case 5u: case 7u: return uint2((c.r & 0xFFFFu) | (c.g << 16u), (c.b & 0xFFFFu) | (c.a << 16u));
    case 15u: return c.rg;
    default: return uint2(c.r, 0u);
  }
}
uint4 XeGuestWordsToBits(uint2 w, uint fmt) {
  switch (fmt) {
    case 4u: case 6u: return uint4(w.x & 0xFFFFu, w.x >> 16u, 0u, 0u);
    case 5u: case 7u: return uint4(w.x & 0xFFFFu, w.x >> 16u, w.y & 0xFFFFu, w.y >> 16u);
    case 15u: return uint4(w.x, w.y, 0u, 0u);
    default: return uint4(w.x, 0u, 0u, 0u);
  }
}
)";

// Colour resolve, region to region (notes/45 section 3, SDK reference): our host texel -> the EDRAM word(s) the
// reference dumps -> XeColourResolvePixel (decode, average, 2^bias, swap, encode; endian left to the comparison) or,
// on the reference's raw path, the words themselves -> R32_UINT words (two per pixel for 64bpp).
//   c0.xy source origin, c0.zw size; c1.xy destination origin (pixels); c1.z source ColorRenderTargetFormat;
//   c1.w RB_COPY_DEST_INFO with the endian bits cleared; c2.x 1 = raw copy, c2.y 1 = reference Canary (D23 runs)
constexpr const char* kColourResolveCs = R"(
Texture2D<float4> xe_src : register(t0);
RWTexture2D<uint> xe_dest : register(u0);
cbuffer XeResolveConstants : register(b0) { uint4 c0; uint4 c1; uint4 c2; uint4 c3; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  // c3.x: resolution scale (Stage 7; 0 = 1); c3.y = 1: one thread per host pixel into the scaled destination.
  // Patch 0143: c3.y bits 8-15 = the source's own scale when it differs (nearest); 0 = the same.
  uint rs = max(c3.x, 1u);
  uint ss = (c3.y >> 8u) & 0xFFu;
  if (ss == 0u) ss = rs;
  bool full = (c3.y & 0xFFu) != 0u;
  if (id.x >= (full ? c0.z * rs : c0.z) || id.y >= (full ? c0.w * rs : c0.w)) return;
  float4 v = xe_src.Load(int3(int2(full ? (c0.xy * rs + id.xy) * ss / rs : (c0.xy + id.xy) * rs + rs / 2u), 0));
  uint2 w = XeHostToGuestWords(v, c1.z);
  uint4 xe_lo = uint4(w.x, 0u, 0u, 0u), xe_hi = uint4(w.y, 0u, 0u, 0u);
  uint xe_n = 1u;
  uint2 o = c2.x != 0u ? w : XeColourResolvePixel(c1.z, xe_lo, xe_hi, xe_n, c1.w, c2.y != 0u, false);
  // Raw path + RB_COPY_DEST_INFO swap (bit 24): red/blue swapped by the source format, as the reference's fast resolve
  // shaders do (Canary resolve.xesli 1011-1055, XeResolveSwap8PixelsRedBlue32bpp / 4PixelsRedBlue64bpp). Job 123: the
  // front buffer (8888, swap) came out with red and blue exchanged without this.
  if (c2.x != 0u && ((c1.w >> 24u) & 1u) != 0u) {
    if (c1.z == 0u || c1.z == 1u) {
      o.x = (o.x & ~0xFF00FFu) | ((o.x & 0xFFu) << 16u) | ((o.x >> 16u) & 0xFFu);
    } else if (c1.z == 2u || c1.z == 3u || c1.z == 10u || c1.z == 12u) {
      o.x = (o.x & ~0x3FF003FFu) | ((o.x & 0x3FFu) << 20u) | ((o.x >> 20u) & 0x3FFu);
    } else if (c1.z == 5u || c1.z == 7u) {
      o = (o & ~0xFFFFu) | (o.yx & 0xFFFFu);
    }
  }
  if (full && (c3.z & 0x80000000u) != 0u) {  // job 238: fused - straight into the scaled-memory window
    uint bpl = (c3.w >> 4u) & 7u;
    uint i = XeChunkIndex(id.xy, rs, c1.xy, c2.z, c2.w, c3.z & 0xFFFFu, bpl);
    if (i == 0xFFFFFFFFu) return;
    xe_dest[XeChunkCoord(i)] = XeChunkSwapWord(o.x, c3.w & 3u);
    if (bpl == 3u) xe_dest[XeChunkCoord(i + 1u)] = XeChunkSwapWord(o.y, c3.w & 3u);
    return;
  }
  uint2 p = (full ? c1.xy * rs : c1.xy) + id.xy;
  // Patch 0030: 8 / 16 / 128 bpp destinations (Canary resolve_full_8/16/128bpp): one R32 word per pixel holding the
  // byte or 16-bit value (8 / 16 bpp), or four words per pixel (128 bpp); kWordsToGuestCs packs them into memory.
  uint xe_bpp = XeDestBytesPerPixelAny((c1.w >> 7u) & 63u);
  if (xe_bpp == 1u || xe_bpp == 2u || xe_bpp == 16u) {
    uint4 q = XeColourResolvePixelOther(c1.z, xe_lo, xe_hi, xe_n, c1.w, c2.y != 0u);
    if (xe_bpp == 16u) {
      [unroll] for (uint k = 0u; k < 4u; ++k) xe_dest[uint2(p.x * 4u + k, p.y)] = q[k];
    } else {
      xe_dest[p] = q.x;
    }
    return;
  }
  if (XeDestBytesPerPixel((c1.w >> 7u) & 63u) == 8u) {
    xe_dest[uint2(p.x * 2u, p.y)] = o.x;
    xe_dest[uint2(p.x * 2u + 1u, p.y)] = o.y;
  } else {
    xe_dest[p] = o.x;
  }
}
)";

// Patch 0119 (--gow2_renderer_resolve_draws): kColourResolveCs's 32 bpp path as a full-screen draw into the R32_UINT
// destination (a render target), one fragment per destination word - jobs 427 / 428: on Vulkan a step that reads the
// target just drawn costs ~16-23 us less as a draw than as a dispatch. The same constants (c0-c3) and the same maths;
// the caller limits it to unscaled / scaled (c3.y) 32 bpp destinations, single-sample sources, not fused (c3.z bit 31).
// The scissor is the destination rectangle, so id = this pixel - its corner (the dispatch's id.xy).
constexpr const char* kColourResolvePs = R"(
Texture2D<float4> xe_src : register(t0, space0);
cbuffer XeResolveConstants : register(b0, space0) { uint4 c0; uint4 c1; uint4 c2; uint4 c3; };
uint main(float4 pos : SV_Position) : SV_Target0 {
  uint rs = max(c3.x, 1u);
  uint ss = (c3.y >> 8u) & 0xFFu;  // patch 0143: the source's own scale when it differs; 0 = the same
  if (ss == 0u) ss = rs;
  bool full = (c3.y & 0xFFu) != 0u;
  uint2 corner = full ? c1.xy * rs : c1.xy;
  uint3 id = uint3(uint2(pos.xy) - corner, 0u);
  float4 v = xe_src.Load(int3(int2(full ? (c0.xy * rs + id.xy) * ss / rs : (c0.xy + id.xy) * rs + rs / 2u), 0));
  uint2 w = XeHostToGuestWords(v, c1.z);
  uint4 xe_lo = uint4(w.x, 0u, 0u, 0u), xe_hi = uint4(w.y, 0u, 0u, 0u);
  uint xe_n = 1u;
  uint2 o = c2.x != 0u ? w : XeColourResolvePixel(c1.z, xe_lo, xe_hi, xe_n, c1.w, c2.y != 0u, false);
  if (c2.x != 0u && ((c1.w >> 24u) & 1u) != 0u) {
    if (c1.z == 0u || c1.z == 1u) {
      o.x = (o.x & ~0xFF00FFu) | ((o.x & 0xFFu) << 16u) | ((o.x >> 16u) & 0xFFu);
    } else if (c1.z == 2u || c1.z == 3u || c1.z == 10u || c1.z == 12u) {
      o.x = (o.x & ~0x3FF003FFu) | ((o.x & 0x3FFu) << 20u) | ((o.x >> 20u) & 0x3FFu);
    } else if (c1.z == 5u || c1.z == 7u) {
      o = (o & ~0xFFFFu) | (o.yx & 0xFFFFu);
    }
  }
  return o.x;
}
)";

// Word transfer into a colour target (M2 build B): from a colour target of another storage class with the same bits
// per sample, or from a depth target (32bpp). Each destination pixel -> its EDRAM dword(s) -> the source pixel ->
// the source's guest word(s) -> decoded in the destination's format. Colour <-> depth swaps the 40-sample halves of
// each tile (the colour/depth bit layout: word = depth24 << 8 | stencil).
//   t0 = dest base, dest pitch tiles, source base, source pitch tiles
//   t1.x tile width in pixels (80 at 32bpp, 40 at 64bpp), t1.y source kind (0 colour, 1 depth), t1.z source format
//        (colour: ColorRenderTargetFormat; depth: bit 0 20e4, bit 1 remapped by 0.5), t1.w dest format
//   t2.y = 1 (patch 0005): the colour source is 64bpp. Source and destination may now differ in bits per sample
//        (Canary: 32 -> 64bpp takes source pixels 2u and 2u + 1; 64 -> 32bpp takes half (u & 1) of pixel u >> 1;
//        the colour/depth swap is 40 32bpp samples).
constexpr const char* kWordTransferPs = R"(
cbuffer XeTransfer : register(b0, space0) { uint4 t0; uint4 t1; uint4 t2; uint4 t3; };
Texture2D<float> xe_src_depth : register(t0, space0);
Texture2D<uint2> xe_src_stencil : register(t1, space0);
Texture2D<float4> xe_src_colour : register(t2, space0);
// upl-n108: t2.x = 1: colour source read through its UINT bits view (t3); XE_BITS_OUT: the destination is written
// through its UINT bits view.
Texture2D<uint4> xe_src_colour_bits : register(t3, space0);
uint XeD24FromHost(float d, uint flags) {
  if ((flags & 1u) == 0u) {
    precise float scaled = saturate(d) * 16777215.0;
    return min(uint(round(scaled)), 0xFFFFFFu);
  }
  precise float guest = (flags & 2u) ? d * 2.0 : d;
  uint f32u32 = asuint(guest);
  f32u32 = min((f32u32 <= 0x7FFFFFFFu) ? f32u32 : 0u, 0x3FFFFFF8u);
  uint denormalized = ((f32u32 & 0x7FFFFFu) | 0x800000u) >> min(113u - (f32u32 >> 23u), 24u);
  uint f24u32 = (f32u32 < 0x38800000u) ? denormalized : (f32u32 + 0xC8000000u);
  return (f24u32 >> 3u) & 0xFFFFFFu;
}
#ifdef XE_BITS_OUT
uint4 main(float4 pos : SV_Position) : SV_Target0 {
#else
float4 main(float4 pos : SV_Position) : SV_Target0 {
#endif
  uint rs = max(t3.x, 1u);  // t3.x: resolution scale (Stage 7; 0 = 1)
  uint2 hp = uint2(pos.xy);
  uint2 p = hp / rs, sub = hp % rs;
  // t3.y: the SOURCE target's scale (per-target scale threshold; 0 = same as the destination): source host pixel =
  // guest pixel x source scale + this sub-pixel mapped onto it (2x -> 1x takes the top-left sample, 1x -> 2x repeats).
  uint rss = t3.y != 0u ? t3.y : rs;
  uint2 ssub = (sub * rss) / rs;
  uint tw = max(t1.x, 1u);
  uint sx = p.x % tw, sy = p.y % 16u;
  uint tile = (t0.x + (p.y / 16u) * t0.y + p.x / tw) % 2048u;
  uint rel = (tile + 2048u - t0.z) % 2048u;
  bool dst64 = tw == 40u;
  bool src64 = t1.y == 0u && t2.y != 0u;
  uint src_tw = src64 ? 40u : 80u;
  // The destination pixel's dword column(s) in the tile row; each dword -> its source pixel (and half).
  uint words[2] = {0u, 0u};
  [unroll] for (uint k = 0u; k < 2u; ++k) {
    if (k == 1u && !dst64) break;
    uint col = dst64 ? sx * 2u + k : sx;  // 0..79
    // Colour <-> depth: the source sample at this EDRAM position is in the other half of the tile (40 samples).
    if (t1.y != 0u) col = col >= 40u ? col - 40u : col + 40u;
    uint qx = src64 ? (col >> 1u) : col;
    int3 q = int3(int(((rel % t0.w) * src_tw + qx) * rss + ssub.x), int(((rel / t0.w) * 16u + sy) * rss + ssub.y), 0);
    uint2 sw;
    if (t1.y != 0u) {
      sw = uint2((XeD24FromHost(xe_src_depth.Load(q), t1.z) << 8u) | (xe_src_stencil.Load(q).y & 0xFFu), 0u);
    } else if (t2.x != 0u) {
      sw = XeBitsToGuestWords(xe_src_colour_bits.Load(q), t1.z);
    } else {
      sw = XeHostToGuestWords(xe_src_colour.Load(q), t1.z);
    }
    words[k] = (src64 && (col & 1u) != 0u) ? sw.y : sw.x;
  }
  uint2 w = uint2(words[0], words[1]);
#ifdef XE_BITS_OUT
  return XeGuestWordsToBits(w, t1.w);
#else
  return XeGuestWordsToHost(w, t1.w);
#endif
}
)";


// Tile trace sample of a colour target (M2 build B): one texel's guest word(s) into the trace texture.
//   c0.xy pixel, c0.z trace index (two slots: word 0 and word 1), c0.w ColorRenderTargetFormat
constexpr const char* kTraceColourCs = R"(
Texture2D<float4> xe_src : register(t0);
RWTexture2D<uint> xe_dest : register(u0);
cbuffer XeTraceConstants : register(b0) { uint4 c0; uint4 c1; uint4 c2; uint4 c3; };
[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint2 w = XeHostToGuestWords(xe_src.Load(int3(int2(c0.xy), 0)), c0.w);
  xe_dest[uint2(c0.z, 0u)] = w.x;
  xe_dest[uint2(c0.z + 1u, 0u)] = w.y;
}
)";

// Takeover T2 (notes/50): a resolve's destination words (R32_UINT, destination pixel coordinates, before the endian
// swap - as written by the colour/depth resolve shaders) -> the GPU's guest-memory mirror, tiled at
// dest_base + TiledOffset2D and endian-swapped: the same bytes the emulator would write to guest memory (the colour
// comparison's addressing). Our later texture loads then read OUR resolve.
//   c0.x dest_base, c0.y pitch in pixels (dest_pitch_aligned_div_32 * 32), c0.zw destination origin (pixels)
//   c1.xy size (pixels), c1.z log2(bytes per pixel) (2 or 3), c1.w endian (0 none, 1 8in16, 2 8in32, 3 16in32)
constexpr const char* kWordsToGuestCs = R"(
Texture2D<uint> xe_words : register(t0);
RWByteAddressBuffer xe_guest : register(u0);
cbuffer XeWordsConstants : register(b0) { uint4 c0; uint4 c1; uint4 c2; uint4 c3; };
int XeTiledCombine(int oib, uint bank, uint pipe, uint ylsb) {
  return int((ylsb << 4u) | (pipe << 6u) | (bank << 11u)) | (oib & 0xF) | (((oib >> 4) & 1) << 5) |
         (((oib >> 5) & 7) << 8) | ((oib >> 8) << 12);
}
int XeTiledOffset2D(int x, int y, uint pitch, uint bpb_log2) {
  pitch = (pitch + 31u) & ~31u;
  int outer = ((y >> 5) * int(pitch >> 5) + (x >> 5)) << 6;
  int inner = (((y >> 1) & 7) << 3) | (x & 7);
  int oib = (outer | inner) << int(bpb_log2);
  uint bank = uint(y >> 4) & 1u;
  uint pipe = (uint(x >> 3) & 3u) ^ ((uint(y >> 3) & 1u) << 1u);
  return XeTiledCombine(oib, bank, pipe, uint(y) & 1u);
}
uint XeSwapWord(uint v, uint e) {
  if (e == 1u || e == 2u) v = ((v & 0x00FF00FFu) << 8u) | ((v & 0xFF00FF00u) >> 8u);
  if (e == 2u || e == 3u) v = (v << 16u) | (v >> 16u);
  return v;
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  // Patch 0030: c1.z = 0 / 1 (8 / 16 bpp): one thread per 4 / 2 pixels = one stored word (consecutive pixels of an
  // 8-aligned run are consecutive bytes in a tile); 4 (128 bpp): four words per pixel. Endian 4 = 8in64, 5 = 8in128.
  uint per = c1.z == 0u ? 4u : c1.z == 1u ? 2u : 1u;
  if (id.x * per >= c1.x || id.y >= c1.y) return;
  int x = int(c0.z + id.x * per), y = int(c0.w + id.y);
  int off = XeTiledOffset2D(x, y, c0.y, c1.z);
  if (off < 0) return;
  uint addr = c0.x + uint(off);
  if (c1.z <= 1u) {
    if (addr + 4u > 0x20000000u) return;
    uint word = 0u;
    for (uint i = 0u; i < per; ++i) {
      uint v = xe_words.Load(int3(x + int(i), y, 0));
      word |= c1.z == 0u ? (v & 0xFFu) << (8u * i) : (v & 0xFFFFu) << (16u * i);
    }
    if (c1.z == 1u && c1.w == 1u) word = ((word & 0x00FF00FFu) << 8u) | ((word & 0xFF00FF00u) >> 8u);  // 8in16 only
    xe_guest.Store(addr, word);
    return;
  }
  uint words = c1.z == 4u ? 4u : c1.z == 3u ? 2u : 1u;
  if (addr + 4u * words > 0x20000000u) return;
  uint4 u = uint4(0u, 0u, 0u, 0u);
  for (uint k = 0u; k < words; ++k) u[k] = xe_words.Load(int3(int(uint(x) * words + k), y, 0));
  uint e = c1.w;
  if (e == 5u && words == 4u) { u = u.wzyx; e = 2u; }
  if (e == 4u && words >= 2u) { u = u.yxwz; e = 2u; }
  for (uint k = 0u; k < words; ++k) xe_guest.Store(addr + 4u * k, XeSwapWord(u[k], e));
}
)";

// Job 047 (resolve-through): kWordsToGuestCs that also writes the texture-cache texture the game will fetch from the
// same memory (same base, pitch and bytes per texel), exactly as kUntileCs would load it from those bytes: the stored
// word(s), the texture's endian swap, then kUntileCs's conversion (the switch below must stay equal to kUntileCs's).
// That texture then needs no reload (no untile dispatch, no barriers for it).
//   c0-c1 as kWordsToGuestCs; c2.x conversion code (kUntileCs's), c2.y texture endian, c2.zw texture size
constexpr const char* kWordsToGuestThroughCs = R"(
Texture2D<uint> xe_words : register(t0);
RWByteAddressBuffer xe_guest : register(u0);
RWTexture2D<float4> xe_through : register(u1);
cbuffer XeWordsConstants : register(b0) { uint4 c0; uint4 c1; uint4 c2; uint4 c3; };
int XeTiledCombine(int oib, uint bank, uint pipe, uint ylsb) {
  return int((ylsb << 4u) | (pipe << 6u) | (bank << 11u)) | (oib & 0xF) | (((oib >> 4) & 1) << 5) |
         (((oib >> 5) & 7) << 8) | ((oib >> 8) << 12);
}
int XeTiledOffset2D(int x, int y, uint pitch, uint bpb_log2) {
  pitch = (pitch + 31u) & ~31u;
  int outer = ((y >> 5) * int(pitch >> 5) + (x >> 5)) << 6;
  int inner = (((y >> 1) & 7) << 3) | (x & 7);
  int oib = (outer | inner) << int(bpb_log2);
  uint bank = uint(y >> 4) & 1u;
  uint pipe = (uint(x >> 3) & 3u) ^ ((uint(y >> 3) & 1u) << 1u);
  return XeTiledCombine(oib, bank, pipe, uint(y) & 1u);
}
uint XeSwapWord(uint v, uint e) {
  if (e == 1u || e == 2u) v = ((v & 0x00FF00FFu) << 8u) | ((v & 0xFF00FF00u) >> 8u);
  if (e == 2u || e == 3u) v = (v << 16u) | (v >> 16u);
  return v;
}
float XeUn(uint v, uint bits) { return float(v & ((1u << bits) - 1u)) / float((1u << bits) - 1u); }
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= c1.x || id.y >= c1.y) return;
  int x = int(c0.z + id.x), y = int(c0.w + id.y);
  int off = XeTiledOffset2D(x, y, c0.y, c1.z);
  if (off < 0) return;
  uint addr = c0.x + uint(off);
  uint words = c1.z == 3u ? 2u : 1u;
  if (addr + 4u * words > 0x20000000u) return;
  uint m0 = XeSwapWord(xe_words.Load(int3(int(uint(x) * words), y, 0)), c1.w);
  xe_guest.Store(addr, m0);
  uint m1 = 0u;
  if (words == 2u) {
    m1 = XeSwapWord(xe_words.Load(int3(int(uint(x) * 2u + 1u), y, 0)), c1.w);
    xe_guest.Store(addr + 4u, m1);
  }
  if (uint(x) >= c2.z || uint(y) >= c2.w) return;
  uint4 w = uint4(XeSwapWord(m0, c2.y), words == 2u ? XeSwapWord(m1, c2.y) : 0u, 0u, 0u);
  float4 o = float4(0.0, 0.0, 0.0, 1.0);
  switch (c2.x) {
    case 1u: o.x = XeUn(w.x, 8u); break;
    case 2u: o.xy = float2(XeUn(w.x, 8u), XeUn(w.x >> 8u, 8u)); break;
    case 3u: o = float4(XeUn(w.x, 8u), XeUn(w.x >> 8u, 8u), XeUn(w.x >> 16u, 8u), XeUn(w.x >> 24u, 8u)); break;
    case 4u: o.x = XeUn(w.x, 16u); break;
    case 5u: o.xy = float2(XeUn(w.x, 16u), XeUn(w.x >> 16u, 16u)); break;
    case 6u: o = float4(XeUn(w.x, 16u), XeUn(w.x >> 16u, 16u), XeUn(w.y, 16u), XeUn(w.y >> 16u, 16u)); break;
    case 7u: o.x = f16tof32(w.x); break;
    case 8u: o.xy = float2(f16tof32(w.x), f16tof32(w.x >> 16u)); break;
    case 9u: o = float4(f16tof32(w.x), f16tof32(w.x >> 16u), f16tof32(w.y), f16tof32(w.y >> 16u)); break;
    case 10u: o.x = asfloat(w.x); break;
    case 11u: o.xy = asfloat(w.xy); break;
    case 14u: o.x = float(w.x >> 8u) / 16777215.0; break;
    case 15u: {
      uint f24 = w.x >> 8u, m = f24 & 0xFFFFFu, e = f24 >> 20u;
      o.x = e == 0u ? float(m) * asfloat(0x2E800000u) : asfloat(((e + 112u) << 23u) | (m << 3u));  // 2^-34
      break;
    }
    case 13u: o = float4(XeUn(w.x, 10u), XeUn(w.x >> 10u, 10u), XeUn(w.x >> 20u, 10u), XeUn(w.x >> 30u, 2u)); break;
    default: break;
  }
  xe_through[uint2(uint(x), uint(y))] = o;
}
)";

// Job 048 resolve-through check: compares two textures of the same size and format texel by texel (raw bits) on the
// GPU. u0 = a 4x1 R32_UINT result: [0] differing texels, [1] first differing index (y * width + x), [2]/[3] the first
// component of t0/t1 there. c0.xy size; c0.z mode: 1 clear, 0 compare, 2 record the first difference's values.
constexpr const char* kThroughCompareCs = R"(
Texture2D<float4> xe_a : register(t0);
Texture2D<float4> xe_b : register(t1);
RWTexture2D<uint> xe_out : register(u0);
cbuffer XeCompareConstants : register(b0) { uint4 c0; uint4 c1; uint4 c2; uint4 c3; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (c0.z == 1u) {
    if (id.y == 0u && id.x < 4u) xe_out[uint2(id.x, 0u)] = id.x == 1u ? 0xFFFFFFFFu : 0u;
    return;
  }
  if (id.x >= c0.x || id.y >= c0.y) return;
  uint4 a = asuint(xe_a.Load(int3(int(id.x), int(id.y), 0)));
  uint4 b = asuint(xe_b.Load(int3(int(id.x), int(id.y), 0)));
  uint idx = id.y * c0.x + id.x;
  if (c0.z == 2u) {
    if (idx == xe_out[uint2(1u, 0u)]) {
      xe_out[uint2(2u, 0u)] = a.x;
      xe_out[uint2(3u, 0u)] = b.x;
    }
    return;
  }
  if (any(a != b)) {
    uint old;
    InterlockedAdd(xe_out[uint2(0u, 0u)], 1u, old);
    InterlockedMin(xe_out[uint2(1u, 0u)], idx, old);
  }
}
)";

// Stage 5C (notes/49 section 10): the swap's gamma pass on our front-buffer words. The SDK's IssueSwap samples the
// front buffer through fetch constant 0 (format, endian, swizzle) and runs Xenia's apply_gamma_table / apply_gamma_pwl
// (Canary apply_gamma_*.xesli, BSD, Copyright 2022 Ben Vanik) into an R10G10B10A2 image; this does the same in integer
// form and writes the packed RGB10A2 word (R bits 0:9, G 10:19, B 20:29, alpha 3) to an R32_UINT image.
//   t0 our resolve words (R32_UINT, before the destination endian swap), t1 ramps: [0,256) table entries (B 0:9, G 10:19,
//   R 20:29), [256 + 3 * entry + component] PWL (base 0:15, delta 16:31)
//   c0.xy size, c0.z destination endian (RB_COPY_DEST_INFO), c0.w fetch endian; c1.x fetch swizzle (12 bits),
//   c1.y 1 = PWL (2_10_10_10 front buffer), 0 = 256-entry table (8_8_8_8)
constexpr const char* kPresentGammaCs = R"(
Texture2D<uint> xe_src : register(t0);
Texture2D<uint> xe_ramp : register(t1);
RWTexture2D<uint> xe_out : register(u0);
cbuffer XePresentConstants : register(b0) { uint4 c0; uint4 c1; uint4 c2; uint4 c3; };
uint XeSwapWord(uint v, uint e) {
  if (e == 1u || e == 2u) v = ((v & 0x00FF00FFu) << 8u) | ((v & 0xFF00FF00u) >> 8u);
  if (e == 2u || e == 3u) v = (v << 16u) | (v >> 16u);
  return v;
}
uint XeComponent(uint v, uint sel, uint pwl) {
  if (sel == 4u) return 0u;
  if (sel == 5u) return pwl != 0u ? 1023u : 255u;
  if (pwl == 0u) return (v >> (8u * sel)) & 0xFFu;
  if (sel == 3u) return ((v >> 30u) & 3u) * 341u;  // 2-bit alpha as unorm10 (not used for the colour channels)
  return (v >> (10u * sel)) & 0x3FFu;
}
uint XePwl(uint input_value, uint entry) {
  uint base = entry & 0xFFFFu, delta = entry >> 16u;
  float f = saturate((float(base) + float((input_value & 7u) * delta) * (1.0f / 8.0f)) * (1.0f / (64.0f * 1023.0f)));
  return uint(f * 1023.0f + 0.5f);
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= c0.x || id.y >= c0.y) return;
  uint v = XeSwapWord(XeSwapWord(xe_src.Load(int3(int(id.x), int(id.y), 0)), c0.z), c0.w);
  uint pwl = c1.y;
  uint r = XeComponent(v, c1.x & 7u, pwl), g = XeComponent(v, (c1.x >> 3u) & 7u, pwl),
       b = XeComponent(v, (c1.x >> 6u) & 7u, pwl);
  uint orr, og, ob;
  if (pwl != 0u) {
    orr = XePwl(r, xe_ramp.Load(int3(int(256u + (r >> 3u) * 3u), 0, 0)));
    og = XePwl(g, xe_ramp.Load(int3(int(256u + (g >> 3u) * 3u + 1u), 0, 0)));
    ob = XePwl(b, xe_ramp.Load(int3(int(256u + (b >> 3u) * 3u + 2u), 0, 0)));
  } else {
    orr = (xe_ramp.Load(int3(int(r), 0, 0)) >> 20u) & 0x3FFu;
    og = (xe_ramp.Load(int3(int(g), 0, 0)) >> 10u) & 0x3FFu;
    ob = xe_ramp.Load(int3(int(b), 0, 0)) & 0x3FFu;
  }
  xe_out[id.xy] = orr | (og << 10u) | (ob << 20u) | (3u << 30u);
}
)";

// Stage 5C-2: our packed RGB10A2 words (kPresentGammaCs output) into the presenter's R10G10B10A2_UNORM guest-output
// texture (UAV). n / 1023 stored as UNORM10 gives n back exactly.   c0.xy size
constexpr const char* kPresentUnpackCs = R"(
Texture2D<uint> xe_src : register(t0);
RWTexture2D<unorm float4> xe_out : register(u0);
cbuffer XeUnpackConstants : register(b0) { uint4 c0; uint4 c1; uint4 c2; uint4 c3; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= c0.x || id.y >= c0.y) return;
  uint v = xe_src.Load(int3(int(id.x), int(id.y), 0));
  xe_out[id.xy] = float4(float(v & 0x3FFu), float((v >> 10u) & 0x3FFu), float((v >> 20u) & 0x3FFu), 1023.0f) *
                  (1.0f / 1023.0f);
}
)";

// "Match my monitor" (native resolution, Shagg 23 Oct 23:37): the present image (packed RGB10A2 words after the swap's
// gamma, SMAA and sharpening) resampled to the window's exact size, so the presenter shows it 1:1. Per axis: shrinking
// averages the source pixels each output pixel covers, weighted by their overlap (an exact box filter - no aliasing,
// no extra blur); enlarging is bilinear. The 2 top bits are the nearest source pixel's. Ours (MIT).
//   c0.xy source size, c0.zw output size. t0 = source words, u0 = output words.
constexpr const char* kPresentResampleCs = R"(
Texture2D<uint> xe_src : register(t0);
RWTexture2D<uint> xe_out : register(u0);
cbuffer XeResampleConstants : register(b0) { uint4 c0; uint4 c1; uint4 c2; uint4 c3; };
uint XeLoad(int2 p) { return xe_src.Load(int3(clamp(p, int2(0, 0), int2(c0.xy) - 1), 0)); }
// The words are display-encoded (after the game's gamma ramp); averaging is done in linear light (the sRGB curve),
// so shrinking keeps the brightness of fine detail (averaging encoded values darkens edges and highlights).
float3 XeToLinear(float3 e) { return select(e <= 0.04045, e / 12.92, pow((e + 0.055) / 1.055, 2.4)); }
float3 XeToEncoded(float3 l) { return select(l <= 0.0031308, l * 12.92, 1.055 * pow(l, 1.0 / 2.4) - 0.055); }
float3 XeRgb(uint v) {
  return XeToLinear(float3(float(v & 0x3FFu), float((v >> 10u) & 0x3FFu), float((v >> 20u) & 0x3FFu)) / 1023.0);
}
// Source pixels [first, first + count) and the weight of pixel i for output pixel o along one axis.
void XeSpan(uint o, float s, out int first, out int count) {
  if (s >= 1.0) {
    const float lo = float(o) * s, hi = lo + s;
    first = int(floor(lo));
    count = int(ceil(hi)) - first;
  } else {
    const float c = (float(o) + 0.5) * s - 0.5;
    first = int(floor(c));
    count = 2;
  }
}
float XeWeight(int i, uint o, float s) {
  if (s >= 1.0) {
    const float lo = float(o) * s, hi = lo + s;
    return max(0.0, min(float(i) + 1.0, hi) - max(float(i), lo));
  }
  const float c = (float(o) + 0.5) * s - 0.5;
  return max(0.0, 1.0 - abs(float(i) - c));
}
// 0.2.0: enlarging with Lanczos (c1.x = 1): 4 x 4 taps (a = 2) in linear light, clamped to the four nearest source
// pixels so edges don't ring; shrinking keeps the box filter above.
float XeLanczos(float x) {
  x = abs(x);
  if (x < 1e-5) return 1.0;
  if (x >= 2.0) return 0.0;
  const float px = 3.14159265 * x;
  return 2.0 * sin(px) * sin(px * 0.5) / (px * px);
}
float3 XeLanczosAt(uint2 o, float2 s) {
  const float2 c = (float2(o) + 0.5) * s - 0.5;
  const int2 b = int2(floor(c));
  float3 acc = float3(0.0, 0.0, 0.0);
  float wsum = 0.0;
  float3 mn = float3(1e9, 1e9, 1e9), mx = float3(-1e9, -1e9, -1e9);
  for (int y = -1; y <= 2; ++y) {
    const float wy = XeLanczos(float(b.y + y) - c.y);
    for (int x = -1; x <= 2; ++x) {
      const float3 v = XeRgb(XeLoad(b + int2(x, y)));
      const float w = wy * XeLanczos(float(b.x + x) - c.x);
      acc += w * v;
      wsum += w;
      if (x >= 0 && x <= 1 && y >= 0 && y <= 1) { mn = min(mn, v); mx = max(mx, v); }
    }
  }
  return clamp(acc / max(wsum, 1e-6), mn, mx);
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= c0.z || id.y >= c0.w) return;
  const float2 s = float2(c0.xy) / float2(c0.zw);  // source pixels per output pixel
  if (c1.x == 1u && s.x < 1.0 && s.y < 1.0) {
    const float3 l = XeLanczosAt(id.xy, s);
    const uint3 ql = uint3(clamp(floor(XeToEncoded(saturate(l)) * 1023.0 + 0.5), 0.0, 1023.0));
    xe_out[id.xy] = ql.x | (ql.y << 10u) | (ql.z << 20u) | (XeLoad(int2((float2(id.xy) + 0.5) * s)) & 0xC0000000u);
    return;
  }
  int fx, nx, fy, ny;
  XeSpan(id.x, s.x, fx, nx);
  XeSpan(id.y, s.y, fy, ny);
  float3 acc = float3(0.0, 0.0, 0.0);
  float wsum = 0.0;
  for (int y = 0; y < ny; ++y) {
    const float wy = XeWeight(fy + y, id.y, s.y);
    if (wy <= 0.0) continue;
    for (int x = 0; x < nx; ++x) {
      const float w = wy * XeWeight(fx + x, id.x, s.x);
      if (w <= 0.0) continue;
      acc += w * XeRgb(XeLoad(int2(fx + x, fy + y)));
      wsum += w;
    }
  }
  const float3 lin = acc / max(wsum, 1e-6);
  const uint3 q = uint3(clamp(floor(XeToEncoded(saturate(lin)) * 1023.0 + 0.5), 0.0, 1023.0));
  const uint top = XeLoad(int2((float2(id.xy) + 0.5) * s)) & 0xC0000000u;
  xe_out[id.xy] = q.x | (q.y << 10u) | (q.z << 20u) | top;
}
)";

// Sharpening (notes/64, user 15:53-15:59): robust contrast-adaptive sharpening on the present image (packed RGB10A2
// words after the swap's gamma, after SMAA, before the HDR encode). Ours (MIT), written from the published algorithm of
// AMD FidelityFX FSR 1's RCAS ("Robust Contrast Adaptive Sharpening", MIT, Copyright (c) AMD): a 5-tap cross; the
// negative lobe is solved as the largest that cannot clip any channel (out of 0..1), and scaled down where the cross is
// noise-like (luma). Like any sharpener it still leaves a slight dark/bright line along hard edges (a numeric check:
// a 0.2 -> 0.8 ramp gives 0.176 next to its foot at full strength). Strength 0..1 multiplies the lobe (1 = RCAS at 0 stops).
//   c0.xy size; c1.x strength (float). t0 = source words, u0 = sharpened words.
constexpr const char* kSharpenCs = R"(
Texture2D<uint> xe_src : register(t0);
RWTexture2D<uint> xe_out : register(u0);
cbuffer XeSharpenConstants : register(b0) { uint4 c0; float4 c1; uint4 c2; uint4 c3; };
float3 XeTap(int2 p) {
  p = clamp(p, int2(0, 0), int2(c0.xy) - 1);
  uint v = xe_src.Load(int3(p, 0));
  return float3(float(v & 0x3FFu), float((v >> 10u) & 0x3FFu), float((v >> 20u) & 0x3FFu)) * (1.0 / 1023.0);
}
float XeMin3(float3 v) { return min(v.x, min(v.y, v.z)); }
float XeMax3(float3 v) { return max(v.x, max(v.y, v.z)); }
float XeLuma(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }  // as RCAS: 2x luma, no divide
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= c0.x || id.y >= c0.y) return;
  const int2 p = int2(id.xy);
  //    b
  //  d e f
  //    h
  const float3 b = XeTap(p + int2(0, -1)), d = XeTap(p + int2(-1, 0)), e = XeTap(p),
               f = XeTap(p + int2(1, 0)), h = XeTap(p + int2(0, 1));
  const float3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));
  // Limiters: the largest negative lobe that keeps every channel inside [mn4, mx4] (peak = 1, -4).
  const float3 hit_min = mn4 / max(4.0 * mx4, 1e-5);
  const float3 hit_max = (1.0 - mx4) / min(4.0 * mn4 - 4.0, -1e-5);
  const float3 lobe3 = max(-hit_min, hit_max);
  const float kLimit = 0.25 - 1.0 / 16.0;
  float lobe = max(-kLimit, min(XeMax3(lobe3), 0.0)) * saturate(c1.x);
  // Noise: a centre that differs from all four neighbours the same way is sharpened less.
  const float bl = XeLuma(b), dl = XeLuma(d), el = XeLuma(e), fl = XeLuma(f), hl = XeLuma(h);
  float nz = 0.25 * (bl + dl + fl + hl) - el;
  const float range = max(max(max(bl, dl), max(el, fl)), hl) - min(min(min(bl, dl), min(el, fl)), hl);
  nz = saturate(abs(nz) / max(range, 1e-5));
  lobe *= -0.5 * nz + 1.0;
  const float3 c = saturate((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0));
  uint3 q = uint3(round(c * 1023.0));
  xe_out[id.xy] = q.r | (q.g << 10u) | (q.b << 20u) | (3u << 30u);
}
)";

// Upscalers (notes/64): a depth target (t0, depth plane) as R32 float in guest convention for DLSS / FSR and the
// motion pass; b0.x = the factor (asfloat) that undoes a host-side remap (2 for float24 targets stored halved).
constexpr const char* kDepthToFloatCs = R"(
Texture2D<float> xe_depth : register(t0);
RWTexture2D<float> xe_out : register(u0);
cbuffer XeDepthCopy : register(b0) { uint4 xe_c0; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  xe_out.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  xe_out[id.xy] = xe_depth.Load(int3(id.xy, 0)) * asfloat(xe_c0.x);
}
)";

// Upscalers (notes/64, upscaler.h): camera motion of every pixel from its depth. b0 = the reprojection matrix (rows;
// upscale::ReprojectionMatrix: (u, v, depth, 1) of this frame -> the previous frame's clip position). Writes motion in
// pixels, previous minus current, +y down (upscale::CameraMotion is the same formula on the CPU). A pixel whose previous
// position is behind the camera or not finite gets 0.
constexpr const char* kMotionVectorsCs = R"(
Texture2D<float> xe_depth : register(t0);
RWTexture2D<float2> xe_motion : register(u0);
cbuffer XeReprojection : register(b0) { float4 xe_row0; float4 xe_row1; float4 xe_row2; float4 xe_row3; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  xe_motion.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  const float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
  const float4 q = float4(uv, xe_depth.Load(int3(id.xy, 0)), 1.0);
  const float4 p = float4(dot(xe_row0, q), dot(xe_row1, q), dot(xe_row2, q), dot(xe_row3, q));
  float2 m = float2(0.0, 0.0);
  if (p.w > 1e-6) {
    const float2 prev = float2(p.x / p.w + 1.0, 1.0 - p.y / p.w) * 0.5;
    if (all(isfinite(prev))) m = (prev - uv) * float2(w, h);
  }
  xe_motion[id.xy] = m;
}
)";

// A plain copy of packed words (t0 -> u0), e.g. the sharpened image back into the present image.
constexpr const char* kCopyWordsCs = R"(
Texture2D<uint> xe_src : register(t0);
RWTexture2D<uint> xe_out : register(u0);
cbuffer XeCopyConstants : register(b0) { uint4 c0; uint4 c1; uint4 c2; uint4 c3; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= c0.x || id.y >= c0.y) return;
  xe_out[id.xy] = xe_src.Load(int3(int(id.x), int(id.y), 0));
}
)";

// Stage 7.3 (notes/55): HDR10 output. The present image (packed RGB10A2 words after the swap's gamma, i.e. what an SDR
// TV shows) in place -> linear light (gamma 2.2) -> optional highlight expansion above a luminance knee toward the
// peak -> nits (1.0 = paper white) -> BT.709 to BT.2020 -> ST.2084 (PQ) -> packed words again; the presenter's HDR10
// swap chain (SDK patch 24) shows them as they are.
//   c0.xy size; c1.x paper white nits, c1.y peak nits, c1.z knee (linear luminance 0-1), c1.w expansion 0-1 (floats)
constexpr const char* kHdrEncodeCs = R"(
RWTexture2D<uint> xe_img : register(u0);
cbuffer XeHdrConstants : register(b0) { uint4 c0; float4 c1; uint4 c2; uint4 c3; };
float3 XePq(float3 nits) {
  float3 y = saturate(nits / 10000.0);
  const float m1 = 0.1593017578125, m2 = 78.84375, k1 = 0.8359375, k2 = 18.8515625, k3 = 18.6875;
  float3 p = pow(y, m1);
  return pow((k1 + k2 * p) / (1.0 + k3 * p), m2);
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= c0.x || id.y >= c0.y) return;
  uint v = xe_img[id.xy];
  float3 c = float3(float(v & 0x3FFu), float((v >> 10u) & 0x3FFu), float((v >> 20u) & 0x3FFu)) / 1023.0;
  float3 lin = pow(c, 2.2);
  float lum = dot(lin, float3(0.2126, 0.7152, 0.0722));
  float t = saturate((lum - c1.z) / max(1.0 - c1.z, 1e-3));
  float boost = 1.0 + c1.w * t * t * max(c1.y / max(c1.x, 1.0) - 1.0, 0.0);
  float3 nits = lin * boost * c1.x;
  const float3x3 to2020 = float3x3(0.6274040, 0.3292820, 0.0433136,
                                   0.0690970, 0.9195400, 0.0113612,
                                   0.0163916, 0.0880132, 0.8955950);
  float3 pq = XePq(max(mul(to2020, nits), 0.0));
  uint3 q = uint3(round(pq * 1023.0));
  xe_img[id.xy] = q.r | (q.g << 10u) | (q.b << 20u) | (3u << 30u);
}
)";

// GPU texture load (upl-n106): one mip level of an uncompressed 2D guest texture, untiled and endian-swapped from the
// guest-memory buffer straight into the host texture (typed UAV). Same addressing as texture_cache.cpp / Canary
// texture_address (TiledOffset2D + TiledCombine); the endian swap is applied to the aligned 32-bit word holding the
// texel, as the guest memory controller does.
//   c0.x level base address (bytes), c0.y row pitch in blocks, c0.zw texel origin in the guest level (packed mips)
//   c1.xy size in blocks, c1.z log2(bytes per block), c1.w endian (0 none, 1 8in16, 2 8in32, 3 16in32)
//   c2.x 1 = tiled, c2.y host format code (1 R8, 2 RG8, 3 RGBA8, 4 R16, 5 RG16, 6 RGBA16 unorm, 7 R16F, 8 RG16F,
//        9 RGBA16F, 10 R32F, 11 RG32F, 12 RGBA32F, 13 RGB10A2, 14 24:8 unorm depth -> R32F, 15 24:8 20e4 depth -> R32F)
// Patch 0021 (Canary texture_cache, D36): GPU load of a block-compressed (BC1-5) or other raw-copy subresource from the
// guest-memory mirror into the backend's scratch buffer, laid out as the subresource's copy footprint (the backend then
// copies it into the texture; BC formats can't be UAV targets). One thread per guest block: the block's 8 or 16 bytes
// (tiled or linear, as kUntileCs addresses them), endian-swapped per 32-bit word like kUntileCs, stored at
// row * c2.y + column * bytes. c0.x storage base (layer included), c0.y row pitch (blocks), c0.zw block offset (packed
// mip tail); c1.xy blocks wide / high, c1.z bytes-per-block log2 (3 or 4), c1.w endian; c2.x tiled, c2.y row pitch
// (bytes) of the footprint.
constexpr const char* kUntileBlocksCs = R"(
ByteAddressBuffer xe_guest : register(t3);
RWByteAddressBuffer xe_out : register(u0);
cbuffer XeUntileConstants : register(b0) { uint4 c0; uint4 c1; uint4 c2; uint4 c3; };
int XeTiledCombine(int oib, uint bank, uint pipe, uint ylsb) {
  return int((ylsb << 4u) | (pipe << 6u) | (bank << 11u)) | (oib & 0xF) | (((oib >> 4) & 1) << 5) |
         (((oib >> 5) & 7) << 8) | ((oib >> 8) << 12);
}
int XeTiledOffset2D(int x, int y, uint pitch, uint bpb_log2) {
  pitch = (pitch + 31u) & ~31u;
  int outer = ((y >> 5) * int(pitch >> 5) + (x >> 5)) << 6;
  int inner = (((y >> 1) & 7) << 3) | (x & 7);
  int oib = (outer | inner) << int(bpb_log2);
  uint bank = uint(y >> 4) & 1u;
  uint pipe = (uint(x >> 3) & 3u) ^ ((uint(y >> 3) & 1u) << 1u);
  return XeTiledCombine(oib, bank, pipe, uint(y) & 1u);
}
uint XeSwapWord(uint v, uint e) {
  if (e == 1u || e == 2u) v = ((v & 0x00FF00FFu) << 8u) | ((v & 0xFF00FF00u) >> 8u);
  if (e == 2u || e == 3u) v = (v << 16u) | (v >> 16u);
  return v;
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= c1.x || id.y >= c1.y) return;
  int gx = int(c0.z + id.x), gy = int(c0.w + id.y);
  uint bpb_log2 = c1.z;
  int off = c2.x != 0u ? XeTiledOffset2D(gx, gy, c0.y, bpb_log2) : (gy * int(c0.y) + gx) << int(bpb_log2);
  uint dst = id.y * c2.y + (id.x << bpb_log2);
  uint addr = c0.x + uint(max(off, 0));
  bool ok = off >= 0 && addr + (1u << bpb_log2) <= 0x20000000u;
  if (bpb_log2 == 2u) {  // patch 0025: 32bpp cube faces
    uint v = ok ? xe_guest.Load(addr) : 0u;
    xe_out.Store(dst, XeSwapWord(v, c1.w));
  } else if (bpb_log2 == 3u) {
    uint2 v = ok ? xe_guest.Load2(addr) : uint2(0u, 0u);
    xe_out.Store2(dst, uint2(XeSwapWord(v.x, c1.w), XeSwapWord(v.y, c1.w)));
  } else {
    uint4 v = ok ? xe_guest.Load4(addr) : uint4(0u, 0u, 0u, 0u);
    xe_out.Store4(dst, uint4(XeSwapWord(v.x, c1.w), XeSwapWord(v.y, c1.w), XeSwapWord(v.z, c1.w), XeSwapWord(v.w, c1.w)));
  }
}
)";

constexpr const char* kUntileCs = R"(
ByteAddressBuffer xe_guest : register(t3);
RWTexture2D<float4> xe_dst : register(u0);
cbuffer XeUntileConstants : register(b0) { uint4 c0; uint4 c1; uint4 c2; uint4 c3; };
int XeTiledCombine(int oib, uint bank, uint pipe, uint ylsb) {
  return int((ylsb << 4u) | (pipe << 6u) | (bank << 11u)) | (oib & 0xF) | (((oib >> 4) & 1) << 5) |
         (((oib >> 5) & 7) << 8) | ((oib >> 8) << 12);
}
int XeTiledOffset2D(int x, int y, uint pitch, uint bpb_log2) {
  pitch = (pitch + 31u) & ~31u;
  int outer = ((y >> 5) * int(pitch >> 5) + (x >> 5)) << 6;
  int inner = (((y >> 1) & 7) << 3) | (x & 7);
  int oib = (outer | inner) << int(bpb_log2);
  uint bank = uint(y >> 4) & 1u;
  uint pipe = (uint(x >> 3) & 3u) ^ ((uint(y >> 3) & 1u) << 1u);
  return XeTiledCombine(oib, bank, pipe, uint(y) & 1u);
}
uint XeSwapWord(uint v, uint e) {
  if (e == 1u || e == 2u) v = ((v & 0x00FF00FFu) << 8u) | ((v & 0xFF00FF00u) >> 8u);
  if (e == 2u || e == 3u) v = (v << 16u) | (v >> 16u);
  return v;
}
float XeUn(uint v, uint bits) { return float(v & ((1u << bits) - 1u)) / float((1u << bits) - 1u); }
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= c1.x || id.y >= c1.y) return;
  int gx = int(c0.z + id.x), gy = int(c0.w + id.y);
  uint bpb_log2 = c1.z;
  int off = c2.x != 0u ? XeTiledOffset2D(gx, gy, c0.y, bpb_log2) : (gy * int(c0.y) + gx) << int(bpb_log2);
  if (off < 0) return;
  uint addr = c0.x + uint(off);
  if (addr + (1u << bpb_log2) > 0x20000000u) return;
  uint4 w = uint4(0u, 0u, 0u, 0u);
  if (bpb_log2 <= 2u) {
    uint word = XeSwapWord(xe_guest.Load(addr & ~3u), c1.w);
    uint shift = (addr & 3u) * 8u;
    w.x = bpb_log2 == 2u ? word : (word >> shift);
  } else if (bpb_log2 == 3u) {
    uint2 v = xe_guest.Load2(addr);
    w.xy = uint2(XeSwapWord(v.x, c1.w), XeSwapWord(v.y, c1.w));
  } else {
    uint4 v = xe_guest.Load4(addr);
    w = uint4(XeSwapWord(v.x, c1.w), XeSwapWord(v.y, c1.w), XeSwapWord(v.z, c1.w), XeSwapWord(v.w, c1.w));
  }
  float4 o = float4(0.0, 0.0, 0.0, 1.0);
  switch (c2.y) {
    case 1u: o.x = XeUn(w.x, 8u); break;
    case 2u: o.xy = float2(XeUn(w.x, 8u), XeUn(w.x >> 8u, 8u)); break;
    case 3u: o = float4(XeUn(w.x, 8u), XeUn(w.x >> 8u, 8u), XeUn(w.x >> 16u, 8u), XeUn(w.x >> 24u, 8u)); break;
    case 4u: o.x = XeUn(w.x, 16u); break;
    case 5u: o.xy = float2(XeUn(w.x, 16u), XeUn(w.x >> 16u, 16u)); break;
    case 6u: o = float4(XeUn(w.x, 16u), XeUn(w.x >> 16u, 16u), XeUn(w.y, 16u), XeUn(w.y >> 16u, 16u)); break;
    case 7u: o.x = f16tof32(w.x); break;
    case 8u: o.xy = float2(f16tof32(w.x), f16tof32(w.x >> 16u)); break;
    case 9u: o = float4(f16tof32(w.x), f16tof32(w.x >> 16u), f16tof32(w.y), f16tof32(w.y >> 16u)); break;
    case 10u: o.x = asfloat(w.x); break;
    case 11u: o.xy = asfloat(w.xy); break;
    case 12u: o = asfloat(w); break;
    case 14u: o.x = float(w.x >> 8u) / 16777215.0; break;  // = texture_cache.cpp kDepthUnorm
    case 15u: {                                            // = xenos::formats::Float20e4To32 (kDepthFloat)
      uint f24 = w.x >> 8u, m = f24 & 0xFFFFFu, e = f24 >> 20u;
      o.x = e == 0u ? float(m) * asfloat(0x2E800000u) : asfloat(((e + 112u) << 23u) | (m << 3u));  // 2^-34
      break;
    }
    case 13u: o = float4(XeUn(w.x, 10u), XeUn(w.x >> 10u, 10u), XeUn(w.x >> 20u, 10u), XeUn(w.x >> 30u, 2u)); break;
    default: break;
  }
  xe_dst[id.xy] = o;
}
)";

// Stage 7 (notes/53, job 190): the scaled copy of guest memory (Canary's scaled resolve buffer idea). Every guest
// texel byte address A that our resolves write holds S x S host samples at (A - chunk) * S^2 + (sub_y * S + sub_x) *
// bytes per texel, in guest byte order (after the destination endian swap), inside a 32 MB window (chunk) of guest memory.
// A chunk is an R32_UINT texture used as a flat array of words (index -> (index % width, index / width)), so any later
// view of the same memory - another pitch, format or size, as the game reuses resolve memory - finds its samples.
//
// kScaledWordsToMemCs: a scaled resolve destination (R32 words, guest size x S, before the endian swap) -> the chunk.
//   c0.x dest base, c0.y dest pitch (texels), c0.zw rectangle origin (guest texels); c1.xy rectangle size (guest),
//   c1.z bpp log2 (2/3), c1.w dest endian; c2.x scale, c2.y chunk base address, c2.z chunk width, c2.w chunk bytes
constexpr const char* kScaledWordsToMemCs = R"(
Texture2D<uint> xe_words : register(t0);
RWTexture2D<uint> xe_chunk : register(u0);
cbuffer XeScaledConstants : register(b0) { uint4 c0; uint4 c1; uint4 c2; uint4 c3; };
int XeTiledCombine(int oib, uint bank, uint pipe, uint ylsb) {
  return int((ylsb << 4u) | (pipe << 6u) | (bank << 11u)) | (oib & 0xF) | (((oib >> 4) & 1) << 5) |
         (((oib >> 5) & 7) << 8) | ((oib >> 8) << 12);
}
int XeTiledOffset2D(int x, int y, uint pitch, uint bpb_log2) {
  pitch = (pitch + 31u) & ~31u;
  int outer = ((y >> 5) * int(pitch >> 5) + (x >> 5)) << 6;
  int inner = (((y >> 1) & 7) << 3) | (x & 7);
  int oib = (outer | inner) << int(bpb_log2);
  uint bank = uint(y >> 4) & 1u;
  uint pipe = (uint(x >> 3) & 3u) ^ ((uint(y >> 3) & 1u) << 1u);
  return XeTiledCombine(oib, bank, pipe, uint(y) & 1u);
}
uint XeSwapWord(uint v, uint e) {
  if (e == 1u || e == 2u) v = ((v & 0x00FF00FFu) << 8u) | ((v & 0xFF00FF00u) >> 8u);
  if (e == 2u || e == 3u) v = (v << 16u) | (v >> 16u);
  return v;
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint s = max(c2.x, 1u);
  if (id.x >= c1.x * s || id.y >= c1.y * s) return;
  int gx = int(c0.z + id.x / s), gy = int(c0.w + id.y / s);
  uint sub = (id.y % s) * s + (id.x % s);
  int off = XeTiledOffset2D(gx, gy, c0.y, c1.z);
  if (off < 0) return;
  uint a = c0.x + uint(off);
  if (a < c2.y || a - c2.y >= c2.w) return;
  uint words = c1.z == 3u ? 2u : 1u;
  uint idx = ((a - c2.y) * s * s + sub * 4u * words) >> 2u;
  uint2 hp = uint2(c0.z * s + id.x, c0.w * s + id.y);
  for (uint k = 0u; k < words; ++k) {
    uint v = XeSwapWord(xe_words.Load(int3(int(hp.x * words + k), int(hp.y), 0)), c1.w);
    uint i = idx + k;
    xe_chunk[uint2(i % c2.z, i / c2.z)] = v;
  }
}
)";

// kScaledCopyWordsCs: a new scaled-memory window that overlaps older ones first takes their words of the overlap.
//   c0.x source word index, c0.y destination word index, c0.z word count, c0.w chunk width; c1.x threads per row
constexpr const char* kScaledCopyWordsCs = R"(
Texture2D<uint> xe_src : register(t0);
RWTexture2D<uint> xe_dst : register(u0);
cbuffer XeScaledConstants : register(b0) { uint4 c0; uint4 c1; uint4 c2; uint4 c3; };
[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint i = id.y * c1.x + id.x;
  if (i >= c0.z) return;
  uint si = c0.x + i, di = c0.y + i;
  xe_dst[uint2(di % c0.w, di / c0.w)] = xe_src.Load(int3(int(si % c0.w), int(si / c0.w), 0));
}
)";

// kScaledLoadCs: a 32/64 bpp 2D texture whose memory our scaled resolves hold, at S x size, from the chunk: the texel's
// guest address as kUntileCs computes it, its S x S samples, the texture's endian swap, then kUntileCs's conversion.
//   c0.x base, c0.y pitch (texels); c1.xy host size, c1.z bpb log2, c1.w texture endian; c2.x tiled, c2.y conversion
//   code (kUntileCs's), c2.z scale, c2.w chunk base address; c3.x chunk width, c3.y chunk bytes
constexpr const char* kScaledLoadCs = R"(
Texture2D<uint> xe_chunk : register(t0);
RWTexture2D<float4> xe_dst : register(u0);
cbuffer XeScaledConstants : register(b0) { uint4 c0; uint4 c1; uint4 c2; uint4 c3; };
int XeTiledCombine(int oib, uint bank, uint pipe, uint ylsb) {
  return int((ylsb << 4u) | (pipe << 6u) | (bank << 11u)) | (oib & 0xF) | (((oib >> 4) & 1) << 5) |
         (((oib >> 5) & 7) << 8) | ((oib >> 8) << 12);
}
int XeTiledOffset2D(int x, int y, uint pitch, uint bpb_log2) {
  pitch = (pitch + 31u) & ~31u;
  int outer = ((y >> 5) * int(pitch >> 5) + (x >> 5)) << 6;
  int inner = (((y >> 1) & 7) << 3) | (x & 7);
  int oib = (outer | inner) << int(bpb_log2);
  uint bank = uint(y >> 4) & 1u;
  uint pipe = (uint(x >> 3) & 3u) ^ ((uint(y >> 3) & 1u) << 1u);
  return XeTiledCombine(oib, bank, pipe, uint(y) & 1u);
}
uint XeSwapWord(uint v, uint e) {
  if (e == 1u || e == 2u) v = ((v & 0x00FF00FFu) << 8u) | ((v & 0xFF00FF00u) >> 8u);
  if (e == 2u || e == 3u) v = (v << 16u) | (v >> 16u);
  return v;
}
float XeUn(uint v, uint bits) { return float(v & ((1u << bits) - 1u)) / float((1u << bits) - 1u); }
uint XeChunkWord(uint i) { return xe_chunk.Load(int3(int(i % c3.x), int(i / c3.x), 0)); }
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= c1.x || id.y >= c1.y) return;
  uint s = max(c2.z, 1u);
  int gx = int(id.x / s), gy = int(id.y / s);
  uint sub = (id.y % s) * s + (id.x % s);
  uint bpb_log2 = c1.z;
  int off = c2.x != 0u ? XeTiledOffset2D(gx, gy, c0.y, bpb_log2) : (gy * int(c0.y) + gx) << int(bpb_log2);
  if (off < 0) return;
  uint a = c0.x + uint(off);
  if (a < c2.w || a - c2.w >= c3.y) return;
  uint words = bpb_log2 == 3u ? 2u : 1u;
  uint idx = ((a - c2.w) * s * s + sub * 4u * words) >> 2u;
  uint4 w = uint4(XeSwapWord(XeChunkWord(idx), c1.w), 0u, 0u, 0u);
  if (words == 2u) w.y = XeSwapWord(XeChunkWord(idx + 1u), c1.w);
  float4 o = float4(0.0, 0.0, 0.0, 1.0);
  switch (c2.y) {
    case 1u: o.x = XeUn(w.x, 8u); break;
    case 2u: o.xy = float2(XeUn(w.x, 8u), XeUn(w.x >> 8u, 8u)); break;
    case 3u: o = float4(XeUn(w.x, 8u), XeUn(w.x >> 8u, 8u), XeUn(w.x >> 16u, 8u), XeUn(w.x >> 24u, 8u)); break;
    case 4u: o.x = XeUn(w.x, 16u); break;
    case 5u: o.xy = float2(XeUn(w.x, 16u), XeUn(w.x >> 16u, 16u)); break;
    case 6u: o = float4(XeUn(w.x, 16u), XeUn(w.x >> 16u, 16u), XeUn(w.y, 16u), XeUn(w.y >> 16u, 16u)); break;
    case 7u: o.x = f16tof32(w.x); break;
    case 8u: o.xy = float2(f16tof32(w.x), f16tof32(w.x >> 16u)); break;
    case 9u: o = float4(f16tof32(w.x), f16tof32(w.x >> 16u), f16tof32(w.y), f16tof32(w.y >> 16u)); break;
    case 10u: o.x = asfloat(w.x); break;
    case 11u: o.xy = asfloat(w.xy); break;
    case 14u: o.x = float(w.x >> 8u) / 16777215.0; break;
    case 15u: {
      uint f24 = w.x >> 8u, m = f24 & 0xFFFFFu, e = f24 >> 20u;
      o.x = e == 0u ? float(m) * asfloat(0x2E800000u) : asfloat(((e + 112u) << 23u) | (m << 3u));  // 2^-34
      break;
    }
    case 13u: o = float4(XeUn(w.x, 10u), XeUn(w.x >> 10u, 10u), XeUn(w.x >> 20u, 10u), XeUn(w.x >> 30u, 2u)); break;
    default: break;
  }
  xe_dst[id.xy] = o;
}
)";

}  // namespace xenos::renderer::builtin
