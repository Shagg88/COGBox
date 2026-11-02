// Layer 0 (game-free): our wrappers around SMAA (smaa_hlsl.h, MIT) for the presented image - Stage 7.2, notes/54.
// XeGTAO ambient occlusion, upscaler motion-vector / depth fixes and SMAA T2x: PR #139 by gmihaic (https://github.com/gmihaic).
// SMAA 1x, preset HIGH, luma edges, on the swap's gamma-corrected RGB10A2 image (SMAA wants non-linear colour):
// edges (R8G8 in an RGBA8 target) -> blend weights (RGBA8, with SMAA's area and search tables) -> neighbourhood
// blending written straight back as packed RGB10A2 words into the present image (R32_UINT), so presenting and frame
// shots are unchanged. Bindings follow the translator contract (b0, t0-t2, s0 linear clamp, s1 point clamp, space0).
// Our code (MIT).
#pragma once

#include <string>

#include "smaa_hlsl.h"

namespace xenos::renderer::smaa {

inline constexpr const char* kSmaaPrefix = R"(
cbuffer XeSmaa : register(b0, space0) { float4 xe_smaa_rt; };
#define SMAA_RT_METRICS xe_smaa_rt
#define SMAA_PRESET_HIGH
#define SMAA_CUSTOM_SL
SamplerState xe_smaa_linear : register(s0, space0);
SamplerState xe_smaa_point : register(s1, space0);
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(xe_smaa_linear, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(xe_smaa_point, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(xe_smaa_linear, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(xe_smaa_linear, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(xe_smaa_point, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(xe_smaa_linear, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAAGather(tex, coord) tex.Gather(xe_smaa_linear, coord, 0)
)";

inline constexpr const char* kSmaaCommon = R"(
struct XeSmaaV {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
  float4 o0 : TEXCOORD1;
  float4 o1 : TEXCOORD2;
  float4 o2 : TEXCOORD3;
  float2 pix : TEXCOORD4;
};
XeSmaaV XeSmaaFullscreen(uint id) {
  XeSmaaV v;
  float2 uv = float2(float((id << 1u) & 2u), float(id & 2u));
  v.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  v.uv = uv;
  v.o0 = float4(0.0, 0.0, 0.0, 0.0);
  v.o1 = v.o0;
  v.o2 = v.o0;
  v.pix = float2(0.0, 0.0);
  return v;
}
)";

inline constexpr const char* kEdgeVs = R"(
XeSmaaV main(uint id : SV_VertexID) {
  XeSmaaV v = XeSmaaFullscreen(id);
  float4 o[3];
  SMAAEdgeDetectionVS(v.uv, o);
  v.o0 = o[0]; v.o1 = o[1]; v.o2 = o[2];
  return v;
}
)";
inline constexpr const char* kEdgePs = R"(
Texture2D xe_color : register(t0, space0);
float4 main(XeSmaaV v) : SV_Target0 {
  float4 o[3] = {v.o0, v.o1, v.o2};
  return float4(SMAALumaEdgeDetectionPS(v.uv, o, xe_color), 0.0, 0.0);
}
)";
inline constexpr const char* kWeightVs = R"(
XeSmaaV main(uint id : SV_VertexID) {
  XeSmaaV v = XeSmaaFullscreen(id);
  float4 o[3];
  float2 pix;
  SMAABlendingWeightCalculationVS(v.uv, pix, o);
  v.o0 = o[0]; v.o1 = o[1]; v.o2 = o[2];
  v.pix = pix;
  return v;
}
)";
inline constexpr const char* kWeightPs = R"(
Texture2D xe_edges : register(t0, space0);
Texture2D xe_area : register(t1, space0);
Texture2D xe_search : register(t2, space0);
float4 main(XeSmaaV v) : SV_Target0 {
  float4 o[3] = {v.o0, v.o1, v.o2};
  return SMAABlendingWeightCalculationPS(v.uv, v.pix, o, xe_edges, xe_area, xe_search, float4(0.0, 0.0, 0.0, 0.0));
}
)";
inline constexpr const char* kBlendVs = R"(
XeSmaaV main(uint id : SV_VertexID) {
  XeSmaaV v = XeSmaaFullscreen(id);
  float4 o;
  SMAANeighborhoodBlendingVS(v.uv, o);
  v.o0 = o;
  return v;
}
)";
// Output: the present image's packed words (R bits 0:9, G 10:19, B 20:29, alpha 3), as kPresentGammaCs writes them.
inline constexpr const char* kBlendPs = R"(
Texture2D xe_color : register(t0, space0);
Texture2D xe_blend : register(t1, space0);
uint main(XeSmaaV v) : SV_Target0 {
  float4 c = SMAANeighborhoodBlendingPS(v.uv, v.o0, xe_color, xe_blend);
  uint3 q = uint3(round(saturate(c.rgb) * 1023.0));
  return q.r | (q.g << 10u) | (q.b << 20u) | (3u << 30u);
}
)";

// ---- SMAA T2x (notes/80 s.7): SMAA 1x on the HDR scene with two alternating sub-pixel jitters, and a temporal
// resolve of this frame with the last one reprojected by the motion vectors. Runs at the scene colour resolve (as the
// upscalers do), so the HUD and the post passes never see history. The scene goes into a tone-mapped copy first
// (Reinhard on the largest channel, invertible): SMAA blends that, the resolve inverts it - bright highlights do not
// dominate the blends. The edges are found on its square root (SMAA wants non-linear colour). ----

// b0: x = the factor to linear scene light (1 / pre-exposure), y = 1 for HDR (tone map) or 0 (the colour as it is).
// t0 = the scene colour; u0 = the edge-detection input (RGB10A2), u1 = the tone-mapped colour (RGBA16F).
inline constexpr const char* kT2xPrepCs = R"(
Texture2D<float4> xe_scene : register(t0);
RWTexture2D<float4> xe_edge_in : register(u0);
RWTexture2D<float4> xe_tm : register(u1);
cbuffer XeT2x : register(b0) { float4 xe_k; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  xe_tm.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  float4 c = xe_scene.Load(int3(id.xy, 0));
  float3 l = max(c.rgb * xe_k.x, 0.0);
  float3 t = xe_k.y > 0.5 ? l / (1.0 + max(max(l.r, l.g), l.b)) : saturate(l);
  if (any(!isfinite(t))) t = float3(0.0, 0.0, 0.0);
  xe_tm[id.xy] = float4(t, c.a);
  xe_edge_in[id.xy] = float4(sqrt(t), 1.0);
}
)";
// SMAA's blending weights with T2x's sub-sample indices (b0 second register: (1,1,1,0) / (2,2,2,0) by jitter).
inline constexpr const char* kT2xWeightPs = R"(
Texture2D xe_edges : register(t0, space0);
Texture2D xe_area : register(t1, space0);
Texture2D xe_search : register(t2, space0);
float4 main(XeSmaaV v) : SV_Target0 {
  float4 o[3] = {v.o0, v.o1, v.o2};
  return SMAABlendingWeightCalculationPS(v.uv, v.pix, o, xe_edges, xe_area, xe_search, xe_smaa_sub);
}
)";
// Neighbourhood blending of the tone-mapped colour into an RGBA16F target (this frame's anti-aliased image).
inline constexpr const char* kT2xBlendPs = R"(
Texture2D xe_color : register(t0, space0);
Texture2D xe_blend : register(t1, space0);
float4 main(XeSmaaV v) : SV_Target0 {
  return SMAANeighborhoodBlendingPS(v.uv, v.o0, xe_color, xe_blend);
}
)";
// The temporal resolve. t0 = this frame (tone-mapped, anti-aliased), t1 = last frame's, t2 = motion (pixels, previous
// minus current); u0 = the output in the scene's format. b0: x = 1 / factor of kT2xPrepCs (back to the scene's units),
// y = HDR, z = 1 to ignore the history (camera cut / first frame). Last frame's pixel (bilinear, at the reprojected
// position) is clamped to this frame's 3x3 neighbourhood - what moved or appeared falls back to this frame - and the
// two are averaged: the two jitter positions together, about 2x supersampling on edges.
inline constexpr const char* kT2xResolveCs = R"(
Texture2D<float4> xe_cur : register(t0);
Texture2D<float4> xe_prev : register(t1);
Texture2D<float2> xe_motion : register(t2);
RWTexture2D<float4> xe_out : register(u0);
cbuffer XeT2x : register(b0) { float4 xe_k; };
float4 XePrevAt(float2 p, int2 size) {  // bilinear, p in pixels (centres at +0.5)
  float2 f = p - 0.5;
  int2 i = int2(floor(f));
  float2 t = f - float2(i);
  int2 a = clamp(i, int2(0, 0), size - 1), b = clamp(i + 1, int2(0, 0), size - 1);
  float4 c00 = xe_prev.Load(int3(a.x, a.y, 0)), c10 = xe_prev.Load(int3(b.x, a.y, 0));
  float4 c01 = xe_prev.Load(int3(a.x, b.y, 0)), c11 = xe_prev.Load(int3(b.x, b.y, 0));
  return lerp(lerp(c00, c10, t.x), lerp(c01, c11, t.x), t.y);
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  xe_out.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;
  const int2 size = int2(w, h);
  const float4 cur = xe_cur.Load(int3(id.xy, 0));
  float4 res = cur;
  const float2 p = float2(id.xy) + 0.5 + xe_motion.Load(int3(id.xy, 0));
  if (xe_k.z < 0.5 && all(p >= 0.0) && all(p <= float2(size))) {
    float3 lo = cur.rgb, hi = cur.rgb;
    for (int y = -1; y <= 1; ++y)
      for (int x = -1; x <= 1; ++x) {
        const float3 n = xe_cur.Load(int3(clamp(int2(id.xy) + int2(x, y), int2(0, 0), size - 1), 0)).rgb;
        lo = min(lo, n);
        hi = max(hi, n);
      }
    const float3 prev = clamp(XePrevAt(p, size).rgb, lo, hi);
    res.rgb = lerp(cur.rgb, prev, 0.5);
  }
  float3 c = res.rgb;
  if (xe_k.y > 0.5) c = c / max(1.0 - max(max(c.r, c.g), c.b), 1.0 / 1024.0);
  xe_out[id.xy] = float4(c * xe_k.x, res.a);
}
)";

// Full source of one pass: our prefix, SMAA.hlsl, the shared vertex layout, the pass body. t2x: the constant buffer
// also holds the sub-sample indices (xe_smaa_sub).
inline std::string Source(const char* body, bool t2x = false) {
  std::string s = kSmaaPrefix;
  if (t2x) s.replace(s.find("float4 xe_smaa_rt; };"), 21, "float4 xe_smaa_rt; float4 xe_smaa_sub; };");
  for (const char* part : kSmaaHlslParts) s += part;
  s += kSmaaCommon;
  s += body;
  return s;
}

}  // namespace xenos::renderer::smaa
