// Layer 0 (game-free): our wrappers around SMAA (smaa_hlsl.h, MIT) for the presented image - Stage 7.2, notes/54.
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

// Full source of one pass: our prefix, SMAA.hlsl, the shared vertex layout, the pass body.
inline std::string Source(const char* body) {
  std::string s = kSmaaPrefix;
  for (const char* part : kSmaaHlslParts) s += part;
  s += kSmaaCommon;
  s += body;
  return s;
}

}  // namespace xenos::renderer::smaa
