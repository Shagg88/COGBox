// Layer 0 (game-free): FidelityFX CACAO screen-space ambient occlusion as the renderer's built-in compute shaders.
//
// Ported from FidelityFX CACAO (https://github.com/GPUOpen-Effects/FidelityFX-CACAO, ffx-cacao/src/ffx_cacao.hlsl):
// Copyright (c) 2021 Advanced Micro Devices, Inc., and Copyright (c) 2016 Intel Corporation (ASSAO), MIT
// (THIRD_PARTY_NOTICES.md has the full text). Fixed quality levels only, no adaptive level (its importance map and
// load counter), and no input normals: the games have no normal buffer.
// Changes for our renderer (our code, MIT): the shared compute layout (16 root constants, t0-t3, u0-u1, no samplers),
// so the deinterleaved arrays are one 2D texture per layer, samples are Loads with a manual mirror and bilinear, and
// depth MIPs and the normals are one dispatch per level / one texture. Guest depth through the game's viewport as XeGTAO (ao.h).
//
// Root constants (uint4 x 4), set by ShadowRenderer::RunCacao from ao::GtaoConstants:
//   c0: scene width, height (host pixels), asfloat host->guest depth scale, pass parameter (layer | MIP << 8)
//   c1: asfloat depth unpack mul, add (view z = mul / (add - guest depth)), tan(fov/2) x, y
//   c2: asfloat effect radius (view units), final power, near-limit reciprocal; step S (2 full, 4 half resolution)
//   c3: layer width, height; asfloat fade mul, add
#pragma once

namespace xenos::renderer::builtin {

constexpr const char* kCacaoCommon = R"(
cbuffer Cacao : register(b0) { uint4 cc_c0; uint4 cc_c1; uint4 cc_c2; uint4 cc_c3; };
#define CC_SIZE         int2(cc_c0.xy)
#define CC_DEPTH_SCALE  asfloat(cc_c0.z)
#define CC_LAYER        (cc_c0.w & 3u)
#define CC_MIP          (cc_c0.w >> 8)
#define CC_UNPACK       asfloat(cc_c1.xy)
#define CC_TAN          asfloat(cc_c1.zw)
#define CC_RADIUS       asfloat(cc_c2.x)
#define CC_POWER        asfloat(cc_c2.y)
#define CC_NEAR_REC     asfloat(cc_c2.z)
#define CC_STEP         int(cc_c2.w)
#define CC_LAYER_SIZE   int2(cc_c3.xy)
#define CC_FADE_MUL     asfloat(cc_c3.z)
#define CC_FADE_ADD     asfloat(cc_c3.w)
static const float CC_FAR = 1.0e6;              // view z for "sky", as XeGTAO
static const float CC_INV_SHARPNESS = 0.02;     // FFX_CACAO_DEFAULT_SETTINGS: 1 - sharpness 0.98
static const float CC_STRENGTH = 4.3;           // shadowMultiplier 1.0 x 4.3
static const float CC_CLAMP = 0.98;             // shadowClamp
static const float CC_HORIZON = 0.06;           // horizonAngleThreshold
static const float CC_DETAIL = 0.5;             // detailShadowStrength
static const float CC_PRECISION = 0.9999;       // DepthPrecisionOffsetMod for 32-bit depths

float2 CcNdcMul() { return float2(CC_TAN.x * 2.0, CC_TAN.y * -2.0); }
float2 CcNdcAdd() { return float2(-CC_TAN.x, CC_TAN.y); }
float3 CcViewPos(float2 uv, float z) { return float3((CcNdcMul() * uv + CcNdcAdd()) * z, z); }
float CcViewDepth(float host_depth) {
  const float z = CC_UNPACK.x / (CC_UNPACK.y - host_depth * CC_DEPTH_SCALE);
  return (z > 0.0 && z < CC_FAR) ? z : CC_FAR;
}
int CcMirror1(int v, int size) {
  if (v < 0) v = -v - 1;
  if (v >= size) v = 2 * size - 1 - v;
  return clamp(v, 0, size - 1);
}
int2 CcMirror(int2 p, int2 size) { return int2(CcMirror1(p.x, size.x), CcMirror1(p.y, size.y)); }
int2 CcLayerOffset(uint layer) { return int2(layer & 1u, layer >> 1) * (CC_STEP / 2); }
float CcPackEdges(float4 e) {
  e = round(saturate(e) * 3.05);
  return dot(e, float4(64.0 / 255.0, 16.0 / 255.0, 4.0 / 255.0, 1.0 / 255.0));
}
float4 CcUnpackEdges(float v) {
  const uint p = uint(v * 255.5);
  return saturate(float4((p >> 6) & 3u, (p >> 4) & 3u, (p >> 2) & 3u, p & 3u) / 3.0 + CC_INV_SHARPNESS);
}
float4 CcEdges(float c, float l, float r, float t, float b) {
  float4 e = float4(l, r, t, b) - c;
  const float4 slope = e + e.yxwz;
  e = min(abs(e), abs(slope));
  return saturate(1.3 - e / (c * 0.040));
}
)";

// One layer's view depth, one MIP level per dispatch (FFX_CACAO_PrepareNativeDepthsAndMips): level 0 from the scene
// depth, each further level from 2 x 2 of the one before. The backend transitions whole textures, so a texture can't
// be read and written in one dispatch: levels 0 and 2 live in one texture, 1 and 3 in another, each read from the other
// (t1). Dispatch (layer size >> MIP + 7) / 8.
constexpr const char* kCacaoDepthCs = R"(
Texture2D<float> cc_depth : register(t0);
Texture2D<float> cc_prev : register(t1);
RWTexture2D<float> cc_out : register(u0);
float CcRaw(int2 p) { return CcViewDepth(cc_depth.Load(int3(clamp(p, int2(0, 0), CC_SIZE - 1), 0))); }
[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
  uint w, h;
  cc_out.GetDimensions(w, h);
  if (dtid.x >= w || dtid.y >= h) return;
  const uint mip = CC_MIP;
  if (mip == 0u) { cc_out[dtid.xy] = CcRaw(int2(dtid.xy) * CC_STEP + CcLayerOffset(CC_LAYER)); return; }
  // FFX_CACAO_MipSmartAverage: samples far behind the closest one count less.
  const int2 last = max(CC_LAYER_SIZE >> (mip - 1u), int2(1, 1)) - 1;
  const int2 a = int2(dtid.xy) * 2, b = min(a + 1, last);
  const float4 d = float4(cc_prev.Load(int3(min(a, last), mip - 1u)), cc_prev.Load(int3(b.x, min(a.y, last.y), mip - 1u)),
                          cc_prev.Load(int3(min(a.x, last.x), b.y, mip - 1u)), cc_prev.Load(int3(b, mip - 1u)));
  const float closest = min(min(d.x, d.y), min(d.z, d.w));
  const float4 diff = d - closest;
  const float4 k = saturate(diff * diff * (-1.0 / (CC_RADIUS * CC_RADIUS)) + 1.0);
  cc_out[dtid.xy] = dot(k, d) / dot(k, float4(1.0, 1.0, 1.0, 1.0));
}
)";

// Normals from depth on the merged grid (2 x layer size): FFX_CACAO_PrepareNormals. x and y are stored: every normal
// of a visible surface faces the camera (z < 0), so z follows.
constexpr const char* kCacaoNormalsCs = R"(
Texture2D<float> cc_depth : register(t0);
RWTexture2D<float2> cc_out : register(u0);
float3 CcAt(int2 g, int2 d) {
  const int2 p = clamp((g + d) * (CC_STEP / 2), int2(0, 0), CC_SIZE - 1);
  return CcViewPos((float2(p) + 0.5) / float2(CC_SIZE), CcViewDepth(cc_depth.Load(int3(p, 0))));
}
[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
  uint w, h;
  cc_out.GetDimensions(w, h);
  if (dtid.x >= w || dtid.y >= h) return;
  const int2 g = int2(dtid.xy);
  const float3 c = CcAt(g, int2(0, 0)), l = CcAt(g, int2(-1, 0)), r = CcAt(g, int2(1, 0)), t = CcAt(g, int2(0, -1)),
               b = CcAt(g, int2(0, 1));
  const float4 e = CcEdges(c.z, l.z, r.z, t.z, b.z);
  const float4 accepted = float4(e.x * e.z, e.z * e.y, e.y * e.w, e.w * e.x);
  const float3 nl = normalize(l - c), nr = normalize(r - c), nt = normalize(t - c), nb = normalize(b - c);
  float3 n = float3(0.0, 0.0, -0.0005);
  n += accepted.x * cross(nl, nt);
  n += accepted.y * cross(nt, nr);
  n += accepted.z * cross(nr, nb);
  n += accepted.w * cross(nb, nl);
  cc_out[dtid.xy] = normalize(n).xy;
}
)";

// One layer's obscurance and edges: FFX_CACAO_GenerateSSAOShadowsInternal. CC_Q 1 = its medium (5 taps, no MIPs), 2 =
// its high (12 taps, depth MIPs, normal-based edges). x = visibility, y = packed edges.
constexpr const char* kCacaoSsaoCs = R"(
Texture2D<float> cc_depth : register(t0);    // this layer's view depth, MIPs 0 and 2
Texture2D<float2> cc_normals : register(t1);
Texture2D<float> cc_depth_odd : register(t2);  // MIPs 1 and 3
RWTexture2D<float2> cc_out : register(u0);
static const float4 cc_pattern[12] = {
  float4( 0.78488064,  0.56661671, 1.500000, -0.126083), float4( 0.26022232, -0.29575172, 1.500000, -1.064030),
  float4( 0.10459357,  0.08372527, 1.110000, -2.730563), float4(-0.68286800,  0.04963045, 1.090000, -0.498827),
  float4(-0.13570161, -0.64190155, 1.250000, -0.532765), float4(-0.26193795, -0.08205118, 0.670000, -1.783245),
  float4(-0.61177456,  0.66664219, 0.710000, -0.044234), float4( 0.43675563,  0.25119025, 0.610000, -1.167283),
  float4( 0.07884444,  0.86618668, 0.640000, -0.459002), float4(-0.12790935, -0.29869005, 0.600000, -1.729424),
  float4(-0.04031125,  0.02413622, 0.600000, -4.792042), float4( 0.16201244, -0.52851415, 0.790000, -1.067055)
};
#if CC_Q >= 2
  #define CC_TAPS 12
#else
  #define CC_TAPS 5
#endif
static const float CC_HALO_AMOUNT = 0.6;  // FFX_CACAO_HALOING_REDUCTION_AMOUNT
static const float CC_MIP_OFFSET = -4.3;  // FFX_CACAO_DEPTH_MIPS_GLOBAL_OFFSET
static const float CC_NORMAL_EDGE = 0.5;  // FFX_CACAO_NORMAL_BASED_EDGES_DOT_THRESHOLD

float CcLoadZ(int2 p, uint mip) {
  const int2 size = max(CC_LAYER_SIZE >> mip, int2(1, 1));
  const int3 at = int3(CcMirror(p >> mip, size), mip);
#if CC_Q >= 2
  if (mip & 1u) return cc_depth_odd.Load(at);
#endif
  return cc_depth.Load(at);
}
float2 CcLayerUv(float2 p) { return (p * float(CC_STEP) + float2(CcLayerOffset(CC_LAYER)) + 0.5) / float2(CC_SIZE); }
float3 CcNormal(int2 p) {
  const int2 g = p * 2 + int2(CC_LAYER & 1u, CC_LAYER >> 1);
  const float2 n = cc_normals.Load(int3(CcMirror(g, CC_LAYER_SIZE * 2), 0));
  return float3(n, -sqrt(saturate(1.0 - dot(n, n))));
}
float CcObscurance(float3 n, float3 delta, float falloff) {
  const float len_sq = dot(delta, delta);
  const float n_dot_d = dot(n, delta) / sqrt(len_sq);
  return max(0.0, n_dot_d - CC_HORIZON) * max(0.0, len_sq * falloff + 1.0);
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
  if (dtid.x >= uint(CC_LAYER_SIZE.x) || dtid.y >= uint(CC_LAYER_SIZE.y)) return;
  const int2 p = int2(dtid.xy);
  const float z = CcLoadZ(p, 0), zl = CcLoadZ(p + int2(-1, 0), 0), zr = CcLoadZ(p + int2(1, 0), 0),
              zt = CcLoadZ(p + int2(0, -1), 0), zb = CcLoadZ(p + int2(0, 1), 0);
  if (z >= CC_FAR) { cc_out[p] = float2(1.0, CcPackEdges(float4(1.0, 1.0, 1.0, 1.0))); return; }
  float3 pos = CcViewPos(CcLayerUv(float2(p)), z);
  const float3 normal = CcNormal(p);
  // The size of one layer pixel at this depth.
  const float2 pix = pos.z * CcNdcMul() / float2(CC_LAYER_SIZE);
  // FFX_CACAO_CalculateRadiusParameters
  const float too_close = saturate(length(pos) * CC_NEAR_REC) * 0.8 + 0.2;
  const float radius = CC_RADIUS * too_close;
  const float lookup = (0.85 * radius) / abs(pix.x);
  const float falloff = -1.0 / (radius * radius);
  // PatternRotScaleMatrices (FFX_CACAO_UpdatePerPassConstants), per layer and pixel.
  static const uint spmap[5] = {0u, 1u, 4u, 3u, 2u};
  const uint sub = spmap[(uint(p.y) * 2u + uint(p.x)) % 5u];
  const float a = float(CC_LAYER), b5 = float(sub);
  const float angle = (a + b5 / 5.0) * 1.5707963267948966;
  const float scale = (1.0 + (a - 1.5 + (b5 - 2.0) / 5.0) * 0.07) * lookup;
  const float2x2 rot = float2x2(scale * cos(angle), scale * -sin(angle), -scale * sin(angle), -scale * cos(angle));

  pos *= CC_PRECISION;
  float4 edges = CcEdges(z, zl, zr, zt, zb);
  // Detail AO from the four neighbours (FFX_CACAO_DETAIL_AO).
  const float3 dir = float3(pos.xy / pos.z, 1.0);
  const float3 dl = float3(-abs(pix.x), 0.0, 0.0) + dir * (zl - pos.z), dr = float3(abs(pix.x), 0.0, 0.0) + dir * (zr - pos.z);
  const float3 dt = float3(0.0, -pix.y, 0.0) + dir * (zt - pos.z), db = float3(0.0, pix.y, 0.0) + dir * (zb - pos.z);
  const float near_falloff = 4.0 * falloff;
  const float4 detail = float4(CcObscurance(normal, dl, near_falloff), CcObscurance(normal, dr, near_falloff),
                               CcObscurance(normal, dt, near_falloff), CcObscurance(normal, db, near_falloff));
  float obscurance_sum = CC_DETAIL * dot(detail, edges), weight_sum = 0.0;
#if CC_Q >= 2
  const float3 nl = CcNormal(p + int2(-1, 0)), nr = CcNormal(p + int2(1, 0)), nt = CcNormal(p + int2(0, -1)), nb = CcNormal(p + int2(0, 1));
  edges *= saturate(float4(dot(normal, nl), dot(normal, nr), dot(normal, nt), dot(normal, nb)) + CC_NORMAL_EDGE);
  const float mip_offset = log2(lookup) + CC_MIP_OFFSET;
#endif
  for (int i = 0; i < CC_TAPS; ++i) {
    const float4 s = cc_pattern[i];
    const int2 off = int2(round(mul(rot, s.xy)));
#if CC_Q >= 2
    const uint mip = uint(clamp(round(s.w + mip_offset), 0.0, 3.0));
#else
    const uint mip = 0u;
#endif
    for (int side = 0; side < 2; ++side) {
      const int2 q = side == 0 ? p + off : p - off;
      const float3 hit = CcViewPos(CcLayerUv(float2(q)), CcLoadZ(q, mip));
      const float3 delta = hit - pos;
      const float obscurance = CcObscurance(normal, delta, falloff);
      // FFX_CACAO_HALOING_REDUCTION
      const float reduct = saturate(max(0.0, -delta.z) * (-1.0 / CC_RADIUS) + 2.0);
      const float weight = (CC_HALO_AMOUNT * reduct + (1.0 - CC_HALO_AMOUNT)) * s.z;
      obscurance_sum += obscurance * weight;
      weight_sum += weight;
    }
  }
  float obscurance = obscurance_sum / weight_sum;
  float fade = saturate(pos.z * CC_FADE_MUL + CC_FADE_ADD);
  fade *= saturate(1.0 - (saturate((1.0 - edges.x - edges.y) * 0.35) + saturate((1.0 - edges.z - edges.w) * 0.35)));
  obscurance = min(CC_STRENGTH * obscurance, CC_CLAMP) * fade;
  cc_out[p] = float2(pow(saturate(1.0 - obscurance), CC_POWER), CcPackEdges(edges));
}
)";

// The edge-sensitive blur of a layer: FFX_CACAO_LDSEdgeSensitiveBlur. One dispatch does all CC_BLUR passes in group-shared
// memory, so a layer is read once and written once however many passes run. A group is 16 x 16 threads of 4 x 3 pixel
// tiles; groups overlap by CC_BLUR pixels on each side, whose results are thrown away. Dispatch layer size / (64 - 2 x
// CC_BLUR, 48 - 2 x CC_BLUR), rounded up. Plain floats here where its shader uses 16-bit ones.
constexpr const char* kCacaoBlurCs = R"(
Texture2D<float2> cc_in : register(t0);
RWTexture2D<float2> cc_out : register(u0);
#define CC_GROUP 16
groupshared uint cc_front[2 * CC_GROUP + 2][3 * CC_GROUP + 2];
groupshared uint cc_back[2 * CC_GROUP + 2][3 * CC_GROUP + 2];
uint CcPack(float2 v) { const uint2 p = f32tof16(v); return p.x | (p.y << 16); }
float2 CcUnpack(uint a) { return f16tof32(uint2(a & 0xFFFFu, a >> 16)); }
float4 CcBlur4(float4 packed, float4 c, float4 l, float4 r, float4 t, float4 b) {
  const uint4 p = uint4(packed * 255.5);
  const float4 el = saturate(float4((p >> 6) & 3u) / 3.0 + CC_INV_SHARPNESS);
  const float4 er = saturate(float4((p >> 4) & 3u) / 3.0 + CC_INV_SHARPNESS);
  const float4 et = saturate(float4((p >> 2) & 3u) / 3.0 + CC_INV_SHARPNESS);
  const float4 eb = saturate(float4(p & 3u) / 3.0 + CC_INV_SHARPNESS);
  return (c * 0.5 + l * el + r * er + t * et + b * eb) / (0.5 + el + er + et + eb);
}
#define CC_BLUR_PASS(FROM, TO)                                                                                      \
  [unroll] for (int y = 0; y < 3; ++y) {                                                                            \
    const int2 c = buf + int2(0, y);                                                                                \
    const float4 centre = float4(CcUnpack(FROM[c.x][c.y]), CcUnpack(FROM[c.x + 1][c.y]));                           \
    const float4 top = float4(CcUnpack(FROM[c.x][c.y - 1]), CcUnpack(FROM[c.x + 1][c.y - 1]));                      \
    const float4 bottom = float4(CcUnpack(FROM[c.x][c.y + 1]), CcUnpack(FROM[c.x + 1][c.y + 1]));                   \
    const float4 left = float4(CcUnpack(FROM[c.x - 1][c.y]).y, centre.xyz);                                         \
    const float4 right = float4(centre.yzw, CcUnpack(FROM[c.x + 2][c.y]).x);                                        \
    const float4 v = CcBlur4(edges[y], centre, left, right, top, bottom);                                           \
    TO[c.x][c.y] = CcPack(v.xy);                                                                                    \
    TO[c.x + 1][c.y] = CcPack(v.zw);                                                                                \
  }                                                                                                                 \
  GroupMemoryBarrierWithGroupSync();

[numthreads(CC_GROUP, CC_GROUP, 1)]
void main(uint3 tid3 : SV_GroupThreadID, uint3 gid3 : SV_GroupID) {
  const int2 tid = int2(tid3.xy), gid = int2(gid3.xy);
  const int2 image = gid * (int2(4 * CC_GROUP, 3 * CC_GROUP) - 2 * CC_BLUR) + int2(4, 3) * tid - CC_BLUR;
  const int2 buf = int2(2, 3) * tid + 1;
  float4 edges[3];
  [unroll] for (int y = 0; y < 3; ++y) {
    const float2 s0 = cc_in.Load(int3(CcMirror(image + int2(0, y), CC_LAYER_SIZE), 0));
    const float2 s1 = cc_in.Load(int3(CcMirror(image + int2(1, y), CC_LAYER_SIZE), 0));
    const float2 s2 = cc_in.Load(int3(CcMirror(image + int2(2, y), CC_LAYER_SIZE), 0));
    const float2 s3 = cc_in.Load(int3(CcMirror(image + int2(3, y), CC_LAYER_SIZE), 0));
    cc_front[buf.x][buf.y + y] = CcPack(float2(s0.x, s1.x));
    cc_front[buf.x + 1][buf.y + y] = CcPack(float2(s2.x, s3.x));
    edges[y] = float4(s0.y, s1.y, s2.y, s3.y);
  }
  GroupMemoryBarrierWithGroupSync();
  [unroll] for (int i = 0; i < (CC_BLUR + 1) / 2; ++i) {
    CC_BLUR_PASS(cc_front, cc_back)
    if (2 * i + 1 < CC_BLUR) { CC_BLUR_PASS(cc_back, cc_front) }
  }
  [unroll] for (int y = 0; y < 3; ++y) {
    const int oy = 3 * tid.y + y;
    if (oy < CC_BLUR || oy >= 3 * CC_GROUP - CC_BLUR) continue;
#if (CC_BLUR % 2) == 0
    const float4 v = float4(CcUnpack(cc_front[buf.x][buf.y + y]), CcUnpack(cc_front[buf.x + 1][buf.y + y]));
#else
    const float4 v = float4(CcUnpack(cc_back[buf.x][buf.y + y]), CcUnpack(cc_back[buf.x + 1][buf.y + y]));
#endif
    [unroll] for (int x = 0; x < 4; ++x) {
      const int ox = 4 * tid.x + x;
      const int2 at = image + int2(x, y);
      if (ox >= CC_BLUR && ox < 4 * CC_GROUP - CC_BLUR && at.x < CC_LAYER_SIZE.x && at.y < CC_LAYER_SIZE.y)
        cc_out[at] = float2(v[x], edges[y][x]);
    }
  }
}
)";

// The four layers merged on the grid (2 x layer size): FFX_CACAO_Apply. u0 is R16G16Float at half resolution (for
// kCacaoUpscaleCs), or the final R16Float at full.
constexpr const char* kCacaoApplyCs = R"(
Texture2D<float2> cc_l0 : register(t0);
Texture2D<float2> cc_l1 : register(t1);
Texture2D<float2> cc_l2 : register(t2);
Texture2D<float2> cc_l3 : register(t3);
RWTexture2D<float4> cc_out : register(u0);
float2 CcLoad(int i, int2 p) {
  p = CcMirror(p, CC_LAYER_SIZE);
  if (i == 0) return cc_l0.Load(int3(p, 0));
  if (i == 1) return cc_l1.Load(int3(p, 0));
  if (i == 2) return cc_l2.Load(int3(p, 0));
  return cc_l3.Load(int3(p, 0));
}
float CcBilinear(int i, float2 pos) {  // pos in layer pixels, centres at .5
  const float2 f = pos - 0.5;
  const int2 b = int2(floor(f));
  const float2 w = f - floor(f);
  return lerp(lerp(CcLoad(i, b).x, CcLoad(i, b + int2(1, 0)).x, w.x),
              lerp(CcLoad(i, b + int2(0, 1)).x, CcLoad(i, b + int2(1, 1)).x, w.x), w.y);
}
[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
  uint w, h;
  cc_out.GetDimensions(w, h);
  if (dtid.x >= w || dtid.y >= h) return;
  const int2 g = int2(dtid.xy);
  const int mx = g.x % 2, my = g.y % 2;
  const int ic = mx + my * 2, ih = (1 - mx) + my * 2, iv = mx + (1 - my) * 2, id = (1 - mx) + (1 - my) * 2;
  const float2 c = CcLoad(ic, g / 2);
  const float4 e = CcUnpackEdges(c.y);
  const float fmx = float(mx), fmy = float(my), fmxe = e.y - e.x, fmye = e.w - e.z;
  const float2 pos = float2(g);
  const float ao_h = CcBilinear(ih, (pos + float2(fmx + fmxe - 0.5, 0.5 - fmy)) * 0.5);
  const float ao_v = CcBilinear(iv, (pos + float2(0.5 - fmx, fmy - 0.5 + fmye)) * 0.5);
  const float ao_d = CcBilinear(id, (pos + float2(fmx - 0.5 + fmxe, fmy - 0.5 + fmye)) * 0.5);
  float4 k = float4(1.0, (e.x + e.y) * 0.5, (e.z + e.w) * 0.5, 0.0);
  k.w = (k.y + k.z) * 0.5;
  const float ao = dot(float4(c.x, ao_h, ao_v, ao_d), k) / dot(k, float4(1.0, 1.0, 1.0, 1.0));
  cc_out[dtid.xy] = float4(ao, 0.0, 0.0, 1.0);
}
)";

// Half resolution to full: a depth-aware 2 x 2 upsample of the merged AO (the role of FFX_CACAO_BilateralUpscale).
constexpr const char* kCacaoUpscaleCs = R"(
Texture2D<float2> cc_half : register(t0);
Texture2D<float> cc_depth : register(t1);
RWTexture2D<float> cc_out : register(u0);
float CcZ(int2 p) { return CcViewDepth(cc_depth.Load(int3(clamp(p, int2(0, 0), CC_SIZE - 1), 0))); }
[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
  if (dtid.x >= uint(CC_SIZE.x) || dtid.y >= uint(CC_SIZE.y)) return;
  const int2 p = int2(dtid.xy);
  const float z = CcZ(p);
  const float2 f = (float2(p) + 0.5) * 0.5 - 0.5;
  const int2 b = int2(floor(f));
  const float2 w = f - floor(f);
  const int2 half_size = CC_LAYER_SIZE * 2;
  float sum = 0.0, weight = 0.0;
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 2; ++x) {
      const int2 q = CcMirror(b + int2(x, y), half_size);
      const float tent = (x == 0 ? 1.0 - w.x : w.x) * (y == 0 ? 1.0 - w.y : w.y);
      const float diff = abs(CcZ(q * 2) - z) / max(z, 1.0);
      const float k = tent * exp(-diff * 40.0) + 1.0e-4;
      sum += cc_half.Load(int3(q, 0)).x * k;
      weight += k;
    }
  cc_out[p] = sum / weight;
}
)";

}  // namespace xenos::renderer::builtin
