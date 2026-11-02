// Layer 0 (game-free): XeGTAO screen-space ambient occlusion as the renderer's built-in compute shaders (notes/80).
//
// Ported from XeGTAO (https://github.com/GameTechDev/XeGTAO, Source/Rendering/Shaders/XeGTAO.hlsli, XeGTAO.h,
// vaGTAO.hlsl): Copyright (C) 2016-2021, Intel Corporation, SPDX-License-Identifier: MIT (THIRD_PARTY_NOTICES.md has the
// full text). XeGTAO is based on "Jimenez et al. / Practical Real-Time Strategies for Accurate Indirect Occlusion".
// Changes for our renderer (our code, MIT): one compute layout for every pass (16 root constants, t0-t3, u0-u1, no
// samplers - Gather / SampleLevel with a point-clamp sampler are Loads); 32-bit view depths (XE_GTAO_FP32_DEPTHS, no
// half precision); normals made in place from depth (XE_GTAO_GENERATE_NORMALS_INPLACE - the games have no normal
// buffer); the depth MIPs made by one dispatch per level, each recomputing its 16x16 tile from the scene depth (the
// backend transitions whole textures, so one dispatch can't read one level and write another); guest depth through
// the game's viewport and our host-side remap (ao.h); reversed-Z / infinite far: depths behind the far limit are "sky"
// (kFar), which gets no occlusion. Default constants as XeGTAO.h (XE_GTAO_USE_DEFAULT_CONSTANTS).
//
// Root constants (uint4 x 4), set by ShadowRenderer::RunGtao from ao::GtaoConstants:
//   c0: width, height (host pixels of the scene depth), asfloat host->guest depth scale, pass parameter (MIP level)
//   c1: asfloat depth unpack mul, add (view z = mul / (add - guest depth)), tan(fov/2) x, y
//   c2: asfloat effect radius (view units), falloff range, final value power; noise index (uint)
//   c3: asfloat denoise blur beta, depth MIP sampling offset, sample distribution power
#pragma once

namespace xenos::renderer::builtin {

constexpr const char* kXeGtaoCommon = R"(
cbuffer XeGtao : register(b0) { uint4 xe_c0; uint4 xe_c1; uint4 xe_c2; uint4 xe_c3; };
#define XE_SIZE            xe_c0.xy
#define XE_DEPTH_SCALE     asfloat(xe_c0.z)
#define XE_PARAM           xe_c0.w
#define XE_UNPACK          asfloat(xe_c1.xy)
#define XE_TAN_HALF_FOV    asfloat(xe_c1.zw)
#define XE_RADIUS          asfloat(xe_c2.x)
#define XE_FALLOFF_RANGE   asfloat(xe_c2.y)
#define XE_FINAL_POWER     asfloat(xe_c2.z)
#define XE_NOISE_INDEX     xe_c2.w
#define XE_DENOISE_BETA    asfloat(xe_c3.x)
#define XE_MIP_OFFSET      asfloat(xe_c3.y)
#define XE_SAMPLE_POWER    asfloat(xe_c3.z)
static const float XE_PI = 3.1415926535897932384626433832795;
static const float XE_PI_HALF = 1.5707963267948966192313216916398;
static const float XE_RADIUS_MULTIPLIER = 1.457;   // XE_GTAO_DEFAULT_RADIUS_MULTIPLIER
static const float XE_OCCLUSION_TERM_SCALE = 1.5;  // XE_GTAO_OCCLUSION_TERM_SCALE
static const float XE_DEPTH_MIP_LEVELS = 5.0;      // XE_GTAO_DEPTH_MIP_LEVELS
static const float XE_FAR = 1.0e6;                 // view z for "sky" (behind an infinite far plane)

float2 XePixelSize() { return 1.0 / float2(XE_SIZE); }
float2 XeNdcToViewMul() { return float2(XE_TAN_HALF_FOV.x * 2.0, XE_TAN_HALF_FOV.y * -2.0); }
float2 XeNdcToViewAdd() { return float2(-XE_TAN_HALF_FOV.x, XE_TAN_HALF_FOV.y); }
// XeGTAO_ComputeViewspacePosition
float3 XeViewPos(float2 uv, float z) { return float3((XeNdcToViewMul() * uv + XeNdcToViewAdd()) * z, z); }
// XeGTAO_ScreenSpaceToViewSpaceDepth, from the host depth value (guest depth x our remap) - anything that isn't a
// positive, finite distance (cleared "sky" in reversed-Z, behind the near plane) is the far limit.
float XeViewDepth(float host_depth) {
  const float z = XE_UNPACK.x / (XE_UNPACK.y - host_depth * XE_DEPTH_SCALE);
  return (z > 0.0 && z < XE_FAR) ? z : XE_FAR;
}
)";

// Depth MIP pyramid (XeGTAO_PrefilterDepths16x16): 8x8 threads per 16x16 tile, the tile's levels 0-4 in group
// memory; the dispatch writes the level in XE_PARAM to u0 (the pyramid texture's MIP of that level). Dispatch
// (width + 15) / 16 x (height + 15) / 16.
constexpr const char* kXeGtaoPrefilterCs = R"(
Texture2D<float> xe_depth : register(t0);  // the scene's depth target (plane 0), host convention
RWTexture2D<float> xe_out : register(u0);
groupshared float xe_scratch[8][8];

float XeRaw(int2 p) { return XeViewDepth(xe_depth.Load(int3(clamp(p, int2(0, 0), int2(XE_SIZE) - 1), 0))); }
void XeStore(int2 p, float v) {
  uint w, h;
  xe_out.GetDimensions(w, h);
  if (p.x < int(w) && p.y < int(h)) xe_out[p] = v;
}
// XeGTAO_DepthMIPFilter (default constants)
float XeMipFilter(float d0, float d1, float d2, float d3) {
  const float max_depth = max(max(d0, d1), max(d2, d3));
  const float effect_radius = 0.75 * XE_RADIUS * XE_RADIUS_MULTIPLIER;
  const float falloff_range = XE_FALLOFF_RANGE * effect_radius;
  const float falloff_from = effect_radius * (1.0 - XE_FALLOFF_RANGE);
  const float falloff_mul = -1.0 / falloff_range;
  const float falloff_add = falloff_from / falloff_range + 1.0;
  const float w0 = saturate((max_depth - d0) * falloff_mul + falloff_add);
  const float w1 = saturate((max_depth - d1) * falloff_mul + falloff_add);
  const float w2 = saturate((max_depth - d2) * falloff_mul + falloff_add);
  const float w3 = saturate((max_depth - d3) * falloff_mul + falloff_add);
  return (w0 * d0 + w1 * d1 + w2 * d2 + w3 * d3) / (w0 + w1 + w2 + w3);
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID) {
  const uint mip = XE_PARAM;  // the same for the whole dispatch, so the early returns keep the barriers uniform
  const int2 base = int2(dtid.xy);
  const int2 pc = base * 2;
  const float d0 = XeRaw(pc), d1 = XeRaw(pc + int2(1, 0)), d2 = XeRaw(pc + int2(0, 1)), d3 = XeRaw(pc + int2(1, 1));
  if (mip == 0u) {
    XeStore(pc, d0);
    XeStore(pc + int2(1, 0), d1);
    XeStore(pc + int2(0, 1), d2);
    XeStore(pc + int2(1, 1), d3);
    return;
  }
  const float dm1 = XeMipFilter(d0, d1, d2, d3);
  if (mip == 1u) { XeStore(base, dm1); return; }
  xe_scratch[gtid.x][gtid.y] = dm1;
  GroupMemoryBarrierWithGroupSync();
  if (all((gtid.xy % 2u) == 0u)) {
    const float dm2 = XeMipFilter(xe_scratch[gtid.x][gtid.y], xe_scratch[gtid.x + 1][gtid.y],
                                  xe_scratch[gtid.x][gtid.y + 1], xe_scratch[gtid.x + 1][gtid.y + 1]);
    if (mip == 2u) XeStore(base / 2, dm2);
    xe_scratch[gtid.x][gtid.y] = dm2;
  }
  GroupMemoryBarrierWithGroupSync();
  if (mip == 2u) return;
  if (all((gtid.xy % 4u) == 0u)) {
    const float dm3 = XeMipFilter(xe_scratch[gtid.x][gtid.y], xe_scratch[gtid.x + 2][gtid.y],
                                  xe_scratch[gtid.x][gtid.y + 2], xe_scratch[gtid.x + 2][gtid.y + 2]);
    if (mip == 3u) XeStore(base / 4, dm3);
    xe_scratch[gtid.x][gtid.y] = dm3;
  }
  GroupMemoryBarrierWithGroupSync();
  if (mip == 3u) return;
  if (all((gtid.xy % 8u) == 0u)) {
    const float dm4 = XeMipFilter(xe_scratch[gtid.x][gtid.y], xe_scratch[gtid.x + 4][gtid.y],
                                  xe_scratch[gtid.x][gtid.y + 4], xe_scratch[gtid.x + 4][gtid.y + 4]);
    XeStore(base / 8, dm4);
  }
}
)";

// Main pass (XeGTAO_MainPass + CSGTAO*): XE_SLICES / XE_STEPS are #defined per quality preset in front of this.
// t0 = the view-depth pyramid; u0 = the working AO term (visibility / XE_OCCLUSION_TERM_SCALE), u1 = packed edges.
// Dispatch (width + 7) / 8 x (height + 7) / 8.
constexpr const char* kXeGtaoMainCs = R"(
Texture2D<float> xe_vdepth : register(t0);
RWTexture2D<float> xe_ao : register(u0);
RWTexture2D<float> xe_edges : register(u1);

float XeLoadZ(int2 p) { return xe_vdepth.Load(int3(clamp(p, int2(0, 0), int2(XE_SIZE) - 1), 0)); }
// SampleLevel(point clamp, uv, mip): the nearest level, the texel under uv, clamped to the edge.
float XeSampleZ(float2 uv, float mip) {
  const uint m = uint(mip + 0.5);
  uint w, h, levels;
  xe_vdepth.GetDimensions(m, w, h, levels);
  const int2 p = clamp(int2(floor(uv * float2(w, h))), int2(0, 0), int2(w, h) - 1);
  return xe_vdepth.Load(int3(p, m));
}
float4 XeCalculateEdges(float c, float l, float r, float t, float b) {
  float4 e = float4(l, r, t, b) - c;
  const float slope_lr = (e.y - e.x) * 0.5, slope_tb = (e.w - e.z) * 0.5;
  const float4 adjusted = e + float4(slope_lr, -slope_lr, slope_tb, -slope_tb);
  e = min(abs(e), abs(adjusted));
  return saturate(1.25 - e / (c * 0.011));
}
float XePackEdges(float4 e) {
  e = round(saturate(e) * 2.9);
  return dot(e, float4(64.0 / 255.0, 16.0 / 255.0, 4.0 / 255.0, 1.0 / 255.0));
}
float3 XeCalculateNormal(float4 e, float3 c, float3 l, float3 r, float3 t, float3 b) {
  const float4 accepted = saturate(float4(e.x * e.z, e.z * e.y, e.y * e.w, e.w * e.x) + 0.01);
  l = normalize(l - c); r = normalize(r - c); t = normalize(t - c); b = normalize(b - c);
  return normalize(accepted.x * cross(l, t) + accepted.y * cross(t, r) + accepted.z * cross(r, b) +
                   accepted.w * cross(b, l));
}
float XeFastSqrt(float x) { return asfloat(0x1fbd1df5 + (asint(x) >> 1)); }
float XeFastACos(float in_x) {
  const float x = abs(in_x);
  float res = -0.156583 * x + XE_PI_HALF;
  res *= XeFastSqrt(1.0 - x);
  return in_x >= 0.0 ? res : XE_PI - res;
}
uint XeHilbertIndex(uint x, uint y) {  // XeGTAO.h HilbertIndex, XE_HILBERT_LEVEL 6
  uint index = 0u;
  for (uint level = 32u; level > 0u; level /= 2u) {
    const uint rx = (x & level) > 0u ? 1u : 0u, ry = (y & level) > 0u ? 1u : 0u;
    index += level * level * ((3u * rx) ^ ry);
    if (ry == 0u) {
      if (rx == 1u) { x = 63u - x; y = 63u - y; }
      const uint t = x; x = y; y = t;
    }
  }
  return index;
}
float2 XeSpatioTemporalNoise(uint2 p, uint temporal) {  // vaGTAO.hlsl, Hilbert curve driving R2
  uint index = XeHilbertIndex(p.x % 64u, p.y % 64u);
  index += 288u * (temporal % 64u);
  return frac(0.5 + float(index) * float2(0.75487766624669276005, 0.5698402909980532659114));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const int2 pix = int2(id.xy);
  if (any(id.xy >= XE_SIZE)) return;
  const float2 uv = (float2(pix) + 0.5) * XePixelSize();
  float viewspace_z = XeLoadZ(pix);
  const float pix_lz = XeLoadZ(pix + int2(-1, 0)), pix_rz = XeLoadZ(pix + int2(1, 0));
  const float pix_tz = XeLoadZ(pix + int2(0, -1)), pix_bz = XeLoadZ(pix + int2(0, 1));
  const float4 edges = XeCalculateEdges(viewspace_z, pix_lz, pix_rz, pix_tz, pix_bz);
  xe_edges[pix] = XePackEdges(edges);
  if (viewspace_z >= XE_FAR * 0.5) {  // sky: nothing to occlude
    xe_ao[pix] = 1.0 / XE_OCCLUSION_TERM_SCALE;
    return;
  }
  const float2 ps = XePixelSize();
  const float3 center = XeViewPos(uv, viewspace_z);
  const float3 normal = XeCalculateNormal(edges, center, XeViewPos(uv + float2(-1, 0) * ps, pix_lz),
                                          XeViewPos(uv + float2(1, 0) * ps, pix_rz),
                                          XeViewPos(uv + float2(0, -1) * ps, pix_tz),
                                          XeViewPos(uv + float2(0, 1) * ps, pix_bz));
  viewspace_z *= 0.99999;  // XE_GTAO_FP32_DEPTHS
  const float3 pix_center_pos = XeViewPos(uv, viewspace_z);
  const float3 view_vec = normalize(-pix_center_pos);

  const float effect_radius = XE_RADIUS * XE_RADIUS_MULTIPLIER;
  const float sample_distribution_power = XE_SAMPLE_POWER;
  const float falloff_range = XE_FALLOFF_RANGE * effect_radius;
  const float falloff_from = effect_radius * (1.0 - XE_FALLOFF_RANGE);
  const float falloff_mul = -1.0 / falloff_range;
  const float falloff_add = falloff_from / falloff_range + 1.0;

  float visibility = 0.0;
  const float2 noise = XeSpatioTemporalNoise(id.xy, XE_NOISE_INDEX);
  const float pixel_too_close_threshold = 1.3;
  const float2 pixel_dir_rb_viewspace_size = viewspace_z.xx * (XeNdcToViewMul() * ps);
  const float screenspace_radius = effect_radius / pixel_dir_rb_viewspace_size.x;
  visibility += saturate((10.0 - screenspace_radius) / 100.0) * 0.5;  // fade out for small screen radii
  const float min_s = pixel_too_close_threshold / screenspace_radius;

  for (float slice = 0.0; slice < XE_SLICES; slice += 1.0) {
    const float slice_k = (slice + noise.x) / XE_SLICES;
    const float phi = slice_k * XE_PI;
    const float cos_phi = cos(phi), sin_phi = sin(phi);
    float2 omega = float2(cos_phi, -sin_phi) * screenspace_radius;
    const float3 direction_vec = float3(cos_phi, sin_phi, 0.0);
    const float3 ortho_direction_vec = direction_vec - dot(direction_vec, view_vec) * view_vec;
    const float3 axis_vec = normalize(cross(ortho_direction_vec, view_vec));
    const float3 projected_normal_vec = normal - axis_vec * dot(normal, axis_vec);
    const float sign_norm = sign(dot(ortho_direction_vec, projected_normal_vec));
    float projected_normal_vec_length = length(projected_normal_vec);
    const float cos_norm = saturate(dot(projected_normal_vec, view_vec) / projected_normal_vec_length);
    const float n = sign_norm * XeFastACos(cos_norm);
    const float low_horizon_cos0 = cos(n + XE_PI_HALF), low_horizon_cos1 = cos(n - XE_PI_HALF);
    float horizon_cos0 = low_horizon_cos0, horizon_cos1 = low_horizon_cos1;
    [unroll]
    for (float step = 0.0; step < XE_STEPS; step += 1.0) {
      const float step_base_noise = (slice + step * XE_STEPS) * 0.6180339887498948482;
      const float step_noise = frac(noise.y + step_base_noise);
      float s = (step + step_noise) / XE_STEPS;
      s = pow(s, sample_distribution_power);
      s += min_s;
      float2 sample_offset = s * omega;
      const float sample_offset_length = length(sample_offset);
      const float mip_level = clamp(log2(sample_offset_length) - XE_MIP_OFFSET, 0.0, XE_DEPTH_MIP_LEVELS - 1.0);
      sample_offset = round(sample_offset) * ps;
      const float2 sample_screen_pos0 = uv + sample_offset;
      const float3 sample_pos0 = XeViewPos(sample_screen_pos0, XeSampleZ(sample_screen_pos0, mip_level));
      const float2 sample_screen_pos1 = uv - sample_offset;
      const float3 sample_pos1 = XeViewPos(sample_screen_pos1, XeSampleZ(sample_screen_pos1, mip_level));
      const float3 sample_delta0 = sample_pos0 - pix_center_pos, sample_delta1 = sample_pos1 - pix_center_pos;
      const float sample_dist0 = length(sample_delta0), sample_dist1 = length(sample_delta1);
      const float3 sample_horizon_vec0 = sample_delta0 / sample_dist0;
      const float3 sample_horizon_vec1 = sample_delta1 / sample_dist1;
      const float weight0 = saturate(sample_dist0 * falloff_mul + falloff_add);
      const float weight1 = saturate(sample_dist1 * falloff_mul + falloff_add);
      float shc0 = dot(sample_horizon_vec0, view_vec), shc1 = dot(sample_horizon_vec1, view_vec);
      shc0 = lerp(low_horizon_cos0, shc0, weight0);
      shc1 = lerp(low_horizon_cos1, shc1, weight1);
      horizon_cos0 = max(horizon_cos0, shc0);
      horizon_cos1 = max(horizon_cos1, shc1);
    }
    projected_normal_vec_length = lerp(projected_normal_vec_length, 1.0, 0.05);
    const float h0 = -XeFastACos(horizon_cos1), h1 = XeFastACos(horizon_cos0);
    const float iarc0 = (cos_norm + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n)) / 4.0;
    const float iarc1 = (cos_norm + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n)) / 4.0;
    visibility += projected_normal_vec_length * (iarc0 + iarc1);
  }
  visibility /= XE_SLICES;
  visibility = pow(max(visibility, 0.0), XE_FINAL_POWER);
  visibility = max(0.03, visibility);
  xe_ao[pix] = saturate(visibility / XE_OCCLUSION_TERM_SCALE);
}
)";

// Denoise (XeGTAO_Denoise / CSDenoisePass / CSDenoiseLastPass): edge-aware blur, two horizontal pixels per thread.
// XE_PARAM 1 = the last pass (finalApply: full blur beta, unpacks the term to visibility 0..1), 0 = an earlier pass
// (beta / 5, the term stays packed). t0 = working AO, t1 = packed edges, u0 = the pass's output.
// Dispatch ((width + 1) / 2 + 7) / 8 x (height + 7) / 8.
constexpr const char* kXeGtaoDenoiseCs = R"(
Texture2D<float> xe_ao_in : register(t0);
Texture2D<float> xe_edges_in : register(t1);
RWTexture2D<float> xe_ao_out : register(u0);

float XeLoadAo(int2 p) { return xe_ao_in.Load(int3(clamp(p, int2(0, 0), int2(XE_SIZE) - 1), 0)); }
float XeLoadEdges(int2 p) { return xe_edges_in.Load(int3(clamp(p, int2(0, 0), int2(XE_SIZE) - 1), 0)); }
// GatherRed at the corner c (between texels c - 1 and c): x = (c.x - 1, c.y), y = c, z = (c.x, c.y - 1), w = c - 1.
float4 XeGatherAo(int2 c) { return float4(XeLoadAo(c + int2(-1, 0)), XeLoadAo(c), XeLoadAo(c + int2(0, -1)), XeLoadAo(c - 1)); }
float4 XeGatherEdges(int2 c) {
  return float4(XeLoadEdges(c + int2(-1, 0)), XeLoadEdges(c), XeLoadEdges(c + int2(0, -1)), XeLoadEdges(c - 1));
}
float4 XeUnpackEdges(float packed_f) {
  const uint v = uint(packed_f * 255.5);
  return saturate(float4((v >> 6) & 3u, (v >> 4) & 3u, (v >> 2) & 3u, v & 3u) / 3.0);
}
void XeAddSample(float value, float weight, inout float sum, inout float sum_weight) {
  sum += weight * value;
  sum_weight += weight;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const int2 base = int2(id.xy) * int2(2, 1);
  if (base.x >= int(XE_SIZE.x) || base.y >= int(XE_SIZE.y)) return;
  const bool final_apply = XE_PARAM != 0u;
  const float blur_amount = final_apply ? XE_DENOISE_BETA : XE_DENOISE_BETA / 5.0;
  const float diag_weight = 0.85 * 0.5;
  const float4 edges_q0 = XeGatherEdges(base);
  const float4 edges_q1 = XeGatherEdges(base + int2(2, 0));
  const float4 edges_q2 = XeGatherEdges(base + int2(1, 2));
  const float4 vis_q0 = XeGatherAo(base);
  const float4 vis_q1 = XeGatherAo(base + int2(2, 0));
  const float4 vis_q2 = XeGatherAo(base + int2(0, 2));
  const float4 vis_q3 = XeGatherAo(base + int2(2, 2));
  for (int side = 0; side < 2; ++side) {
    const int2 pix = int2(base.x + side, base.y);
    if (pix.x >= int(XE_SIZE.x)) break;
    const float4 edges_l = XeUnpackEdges(side == 0 ? edges_q0.x : edges_q0.y);
    const float4 edges_t = XeUnpackEdges(side == 0 ? edges_q0.z : edges_q1.w);
    const float4 edges_r = XeUnpackEdges(side == 0 ? edges_q1.x : edges_q1.y);
    const float4 edges_b = XeUnpackEdges(side == 0 ? edges_q2.w : edges_q2.z);
    float4 edges_c = XeUnpackEdges(side == 0 ? edges_q0.y : edges_q1.x);
    edges_c *= float4(edges_l.y, edges_r.x, edges_t.w, edges_b.z);
    const float leak_threshold = 2.5, leak_strength = 0.5;
    const float edginess = (saturate(4.0 - leak_threshold - dot(edges_c, 1.0.xxxx)) / (4.0 - leak_threshold)) * leak_strength;
    edges_c = saturate(edges_c + edginess);
    const float weight_tl = diag_weight * (edges_c.x * edges_l.z + edges_c.z * edges_t.x);
    const float weight_tr = diag_weight * (edges_c.z * edges_t.y + edges_c.y * edges_r.z);
    const float weight_bl = diag_weight * (edges_c.w * edges_b.x + edges_c.x * edges_l.w);
    const float weight_br = diag_weight * (edges_c.y * edges_r.w + edges_c.w * edges_b.y);
    const float v   = side == 0 ? vis_q0.y : vis_q1.x;
    const float v_l = side == 0 ? vis_q0.x : vis_q0.y;
    const float v_t = side == 0 ? vis_q0.z : vis_q1.w;
    const float v_r = side == 0 ? vis_q1.x : vis_q1.y;
    const float v_b = side == 0 ? vis_q2.z : vis_q3.w;
    const float v_tl = side == 0 ? vis_q0.w : vis_q0.z;
    const float v_br = side == 0 ? vis_q3.w : vis_q3.z;
    const float v_tr = side == 0 ? vis_q1.w : vis_q1.z;
    const float v_bl = side == 0 ? vis_q2.w : vis_q2.z;
    float sum_weight = blur_amount;
    float sum = v * sum_weight;
    XeAddSample(v_l, edges_c.x, sum, sum_weight);
    XeAddSample(v_r, edges_c.y, sum, sum_weight);
    XeAddSample(v_t, edges_c.z, sum, sum_weight);
    XeAddSample(v_b, edges_c.w, sum, sum_weight);
    XeAddSample(v_tl, weight_tl, sum, sum_weight);
    XeAddSample(v_tr, weight_tr, sum, sum_weight);
    XeAddSample(v_bl, weight_bl, sum, sum_weight);
    XeAddSample(v_br, weight_br, sum, sum_weight);
    xe_ao_out[pix] = saturate(sum / sum_weight * (final_apply ? XE_OCCLUSION_TERM_SCALE : 1.0));
  }
}
)";

// Debug view (--gow2_ao_debug, notes/80): an XeGTAO texture over the presented image (packed 10:10:10:2 words, the
// gamma pass's output). c0 = present width, height, -, mode (2 = split: the game's picture left, the texture right;
// anything else: the texture only); c1 = the texture's width, height, asfloat value scale. Dispatch (width + 7) / 8 x
// (height + 7) / 8.
constexpr const char* kXeGtaoDebugCs = R"(
cbuffer XeGtaoDebug : register(b0) { uint4 xe_c0; uint4 xe_c1; };
Texture2D<float> xe_ao : register(t0);
RWTexture2D<uint> xe_image : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const uint2 size = xe_c0.xy;
  if (any(id.xy >= size)) return;
  const uint mode = xe_c0.w;
  const uint half_x = size.x / 2u;
  if (mode == 2u && id.x < half_x) return;
  uint q;
  if (mode == 2u && id.x < half_x + 2u) {
    q = 1023u;  // the split line
  } else {
    const float2 uv = (float2(id.xy) + 0.5) / float2(size);
    const int2 p = min(int2(uv * float2(xe_c1.xy)), int2(xe_c1.xy) - 1);
    q = uint(saturate(xe_ao.Load(int3(p, 0)) * asfloat(xe_c1.z)) * 1023.0 + 0.5);
  }
  xe_image[id.xy] = q | (q << 10u) | (q << 20u) | (3u << 30u);
}
)";

// Notes/80 s.4: the full-resolution stand-in for the game's final AO texture. x = XeGTAO's visibility (or 1 for
// "no AO"), y = the game's own y channel at the same place (its 640x360 target). t0 = the game's AO target (host
// R16G16 float), t1 = the final AO (scene size), u0 = the stand-in (scene size, R16G16 float).
// c0 = stand-in width, height, mode (0 XeGTAO, 1 no AO), 1 = the chain was skipped (y = 0); c1 = the resolved rectangle in the game target's host pixels:
// x, y, width, height. Dispatch (width + 7) / 8 x (height + 7) / 8.
constexpr const char* kXeGtaoPackCs = R"(
cbuffer XeGtaoPack : register(b0) { uint4 xe_c0; uint4 xe_c1; };
Texture2D<float2> xe_game : register(t0);
Texture2D<float> xe_ao : register(t1);
RWTexture2D<float2> xe_out : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= xe_c0.xy)) return;
  const float2 uv = (float2(id.xy) + 0.5) / float2(xe_c0.xy);
  const int2 g = int2(xe_c1.xy) + min(int2(uv * float2(xe_c1.zw)), int2(xe_c1.zw) - 1);
  // c0.w = 1: the game's chain was skipped this frame - its target holds stale data, so y is the game's usual 0.
  const float game_y = xe_c0.w != 0u ? 0.0 : xe_game.Load(int3(g, 0)).y;
  const float ao = xe_c0.z == 1u ? 1.0 : xe_ao.Load(int3(id.xy, 0));
  xe_out[id.xy] = float2(ao, game_y);
}
)";

// "Everywhere" (notes/80 s.5-19): where the game draws no AO chain, the scene is multiplied by XeGTAO's visibility
// just before the game's fog pass - the same place and formula as the game's own apply (scene * AO, then fog). Drawn
// with kTransferVs (a full-screen triangle) and a multiply blend (src Zero, dst SrcColor, RGB only). t0 = the final AO
// (scene size, as the viewport).
constexpr const char* kXeGtaoMultiplyPs = R"(
Texture2D<float> xe_ao : register(t0);
float4 main(float4 pos : SV_Position) : SV_Target0 {
  const float a = saturate(xe_ao.Load(int3(int2(pos.xy), 0)));
  return float4(a, a, a, 1.0);
}
)";

}  // namespace xenos::renderer::builtin
