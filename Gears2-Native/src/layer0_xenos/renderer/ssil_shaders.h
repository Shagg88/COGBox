// Layer 0 (game-free): screen-space indirect lighting (one diffuse bounce) as built-in compute shaders - a prototype.
// The method is "Screen Space Indirect Lighting with Visibility Bitmask" (Therrien, Levesque, Gilet, 2023): XeGTAO's
// horizon search, with each slice's half circle split into 32 sectors (a bitmask) and a thickness per sample, so a
// sample lights the sectors it newly covers instead of only raising a horizon. Our code (MIT), written from the paper.
//
// The games are forward lit: there is no albedo or normal buffer. Normals are made from depth (as XeGTAO's); the light
// that bounces is the frame's own scene colour (half resolution); what it lands on gets an albedo estimated from the
// scene colour (S_ALBEDO). The view-space depth pyramid is XeGTAO's (ao_depth_: view z, 5 MIPs), made earlier in the
// frame at the scene depth resolve.
//
// Root constants (uint4 x 4), set by ShadowRenderer::RunSsil:
//   c0: grid width, height (the pass's own pixels), pass parameter (blur: tap spacing; light, main, temporal: flags),
//       noise index
//   c1: asfloat tan(fov/2) x, y, radius (view units), thickness (view units) - the main pass reads its radius,
//       thickness, fade and steps from kSsilScaleCs instead (t3)
//   c2: asfloat strength; view (0 normal, 1 indirect only, 2 split), albedo mode, slices
//   c3: steps per side; scene width, height (full resolution); asfloat view z where the effect has faded out
#pragma once

namespace xenos::renderer::builtin {

constexpr const char* kSsilCommon = R"(
cbuffer Ssil : register(b0) { uint4 s_c0; uint4 s_c1; uint4 s_c2; uint4 s_c3; };
#define S_SIZE         s_c0.xy
#define S_PARAM        s_c0.z
#define S_NOISE        s_c0.w
#define S_TAN_HALF_FOV asfloat(s_c1.xy)
#define S_RADIUS       asfloat(s_c1.z)
#define S_THICKNESS    asfloat(s_c1.w)
#define S_STRENGTH     asfloat(s_c2.x)
#define S_VIEW         s_c2.y
#define S_ALBEDO       s_c2.z
#define S_SLICES       s_c2.w
#define S_STEPS        s_c3.x
#define S_SCENE        s_c3.yz
#define S_FADE         asfloat(s_c3.w)
static const float S_PI = 3.1415926535897932384626433832795;
static const float S_FAR = 1.0e6;  // XeGTAO's "sky" view z
float2 SNdcToViewMul() { return float2(S_TAN_HALF_FOV.x * 2.0, S_TAN_HALF_FOV.y * -2.0); }
float2 SNdcToViewAdd() { return float2(-S_TAN_HALF_FOV.x, S_TAN_HALF_FOV.y); }
float3 SViewPos(float2 uv, float z) { return float3((SNdcToViewMul() * uv + SNdcToViewAdd()) * z, z); }
float SLum(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
)";

// The light that bounces: the scene colour averaged 2x2 to half resolution, very bright pixels tamed (a lamp seen
// directly would otherwise speckle everything near it). S_PARAM bit 0 (several bounces): last frame's indirect light,
// reprojected with the motion vectors and kept only where the depth agrees, is added as light too (x min(0.5 x
// strength, 0.6): grey 50% surfaces, damped so it can't run away). t0 = scene colour, t1 = last frame's accumulated
// indirect light (half, a = view z), t2 = motion vectors (full, render pixels, previous - current), t3 = view-depth
// pyramid; u0 = light. Grid = half size; S_SCENE = the scene's full size. Dispatch (half w + 7) / 8 x (half h + 7) / 8.
constexpr const char* kSsilLightCs = R"(
Texture2D<float4> s_scene : register(t0);
Texture2D<float4> s_hist : register(t1);
Texture2D<float2> s_motion : register(t2);
Texture2D<float> s_vdepth : register(t3);
RWTexture2D<float4> s_light : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= S_SIZE)) return;
  const int2 m = int2(S_SCENE) - 1;
  const int2 p = int2(id.xy) * 2;
  float3 c = s_scene.Load(int3(min(p, m), 0)).rgb + s_scene.Load(int3(min(p + int2(1, 0), m), 0)).rgb +
             s_scene.Load(int3(min(p + int2(0, 1), m), 0)).rgb + s_scene.Load(int3(min(p + int2(1, 1), m), 0)).rgb;
  c = max(c * 0.25, 0.0);
  const float l = SLum(c);
  c *= l > 16.0 ? 16.0 / l : 1.0;
  if ((S_PARAM & 1u) != 0u) {
    // The half-resolution texel's centre in UV, moved by the motion at the full-resolution pixel nearest to it.
    const float2 prev = (float2(id.xy) + 0.5) / float2(S_SIZE) + s_motion.Load(int3(min(p + 1, m), 0)) / float2(S_SCENE);
    if (all(prev >= 0.0) && all(prev < 1.0)) {
      const float4 h = s_hist.Load(int3(int2(prev * float2(S_SIZE)), 0));
      const float z = s_vdepth.Load(int3(id.xy, 1));
      if (abs(h.a - z) < 0.05 * z) c += max(h.rgb, 0.0) * min(0.5 * S_STRENGTH, 0.6);
    }
  }
  s_light[id.xy] = float4(c, 1.0);
}
)";

// Normals from depth, once per frame at half resolution (XeGTAO_CalculateEdges / CalculateNormal on MIP 1): the bounce
// reads them for the receiver and, for the back-face check, for each surface it hits. t0 = view-depth pyramid;
// u0 = xyz normal (view space), w = view z. Dispatch (half w + 7) / 8 x (half h + 7) / 8.
constexpr const char* kSsilNormalsCs = R"(
Texture2D<float> s_vdepth : register(t0);
RWTexture2D<float4> s_normal : register(u0);
float SLoadZ(int2 p) {
  uint w, h, l;
  s_vdepth.GetDimensions(1u, w, h, l);
  return s_vdepth.Load(int3(clamp(p, int2(0, 0), int2(w, h) - 1), 1));
}
float4 SEdges(float c, float l, float r, float t, float b) {
  float4 e = float4(l, r, t, b) - c;
  const float slope_lr = (e.y - e.x) * 0.5, slope_tb = (e.w - e.z) * 0.5;
  const float4 adjusted = e + float4(slope_lr, -slope_lr, slope_tb, -slope_tb);
  e = min(abs(e), abs(adjusted));
  return saturate(1.25 - e / (c * 0.011));
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= S_SIZE)) return;
  const int2 pix = int2(id.xy);
  const float2 ps = 1.0 / float2(S_SIZE);
  const float2 uv = (float2(pix) + 0.5) * ps;
  const float z = SLoadZ(pix);
  if (z >= S_FAR * 0.5) { s_normal[pix] = float4(0.0, 0.0, -1.0, z); return; }
  const float zl = SLoadZ(pix + int2(-1, 0)), zr = SLoadZ(pix + int2(1, 0));
  const float zt = SLoadZ(pix + int2(0, -1)), zb = SLoadZ(pix + int2(0, 1));
  const float4 e = SEdges(z, zl, zr, zt, zb);
  const float3 c = SViewPos(uv, z);
  const float3 l = normalize(SViewPos(uv + float2(-ps.x, 0.0), zl) - c), r = normalize(SViewPos(uv + float2(ps.x, 0.0), zr) - c);
  const float3 t = normalize(SViewPos(uv + float2(0.0, -ps.y), zt) - c), b = normalize(SViewPos(uv + float2(0.0, ps.y), zb) - c);
  const float4 accepted = saturate(float4(e.x * e.z, e.z * e.y, e.y * e.w, e.w * e.x) + 0.01);
  const float3 n = normalize(accepted.x * cross(l, t) + accepted.y * cross(t, r) + accepted.z * cross(r, b) + accepted.w * cross(b, l));
  s_normal[pix] = float4(n, z);
}
)";

// The bounce: per half-resolution pixel, S_SLICES slices x 2 sides x S_STEPS samples; each slice's half circle (from
// the normal's -90 to +90 degrees) is 32 sectors. A sample covers the sectors between its front (the depth buffer) and
// its back (the front pushed the thickness away from the camera); the sectors it covers first send its light, weighted
// by the cosine at the receiver. S_PARAM bit 0: back-face check - a surface seen from behind (its normal facing away
// from the receiver) still blocks but sends no light. t0 = view-depth pyramid (MIP 1 = this grid), t1 = light,
// t2 = normals (xyz, w = view z); u0 = rgb indirect light, a = the sectors left open (a visibility).
// Dispatch (half w + 7) / 8 x (half h + 7) / 8.
constexpr const char* kSsilMainCs = R"(
Texture2D<float> s_vdepth : register(t0);
Texture2D<float4> s_light : register(t1);
Texture2D<float4> s_normal : register(t2);
Texture2D<float4> s_scale : register(t3);  // kSsilScaleCs: radius, thickness, fade, steps (this frame's scale)
RWTexture2D<float4> s_gi : register(u0);

float SSampleZ(float2 uv, uint mip) {
  uint w, h, l;
  s_vdepth.GetDimensions(mip, w, h, l);
  return s_vdepth.Load(int3(clamp(int2(floor(uv * float2(w, h))), int2(0, 0), int2(w, h) - 1), mip));
}
int2 SHalfPixel(float2 uv) { return clamp(int2(floor(uv * float2(S_SIZE))), int2(0, 0), int2(S_SIZE) - 1); }
uint SHilbert(uint x, uint y) {  // XeGTAO.h HilbertIndex, level 6
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
float2 SNoise(uint2 p, uint temporal) {
  const uint index = SHilbert(p.x % 64u, p.y % 64u) + 288u * (temporal % 64u);
  return frac(0.5 + float(index) * float2(0.75487766624669276005, 0.5698402909980532659114));
}
uint SSectors(float lo, float hi) {  // the sectors between two positions 0..1 on the half circle
  if (hi <= 0.0 || lo >= 1.0) return 0u;  // entirely outside it (one used to mark the last sector)
  const uint start = uint(saturate(lo) * 32.0);
  const uint end = min(uint(ceil(saturate(hi) * 32.0)), 32u);
  if (end <= start) return 0u;
  const uint n = end - start;
  return n >= 32u ? 0xFFFFFFFFu : (((1u << n) - 1u) << start);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= S_SIZE)) return;
  const int2 pix = int2(id.xy);
  const float2 ps = 1.0 / float2(S_SIZE);
  const float2 uv = (float2(pix) + 0.5) * ps;
  const float4 nz = s_normal.Load(int3(pix, 0));
  const float z = nz.w;
  const float4 scale = s_scale.Load(int3(0, 0, 0));
  const float radius = scale.x, thickness = scale.y, fade = scale.z;
  const uint nsteps = uint(clamp(scale.w, 2.0, 16.0));
  if (z >= S_FAR * 0.5 || z > fade) { s_gi[pix] = float4(0.0, 0.0, 0.0, 1.0); return; }
  const float3 normal = nz.xyz;
  const bool backface = (S_PARAM & 1u) != 0u;
  const float3 pos = SViewPos(uv, z * 0.99999);
  const float3 view_vec = normalize(-pos);
  const float view_per_px = abs(SNdcToViewMul().x) * ps.x;  // view units per pixel of this grid, per unit of z
  const float radius_px = min(radius / (z * view_per_px), 0.3 * float(S_SIZE.x));
  if (radius_px < 2.0) { s_gi[pix] = float4(0.0, 0.0, 0.0, 1.0); return; }
  const float2 noise = SNoise(id.xy, S_NOISE);
  float3 gi = 0.0;
  float open = 0.0;
  const float slices = float(S_SLICES), steps = float(nsteps);
  for (uint slice = 0u; slice < S_SLICES; ++slice) {
    const float phi = (float(slice) + noise.x) / slices * S_PI;
    const float cos_phi = cos(phi), sin_phi = sin(phi);
    const float2 omega = float2(cos_phi, -sin_phi) * radius_px;  // screen y down, view y up (as XeGTAO)
    const float3 dir = float3(cos_phi, sin_phi, 0.0);
    const float3 ortho = dir - dot(dir, view_vec) * view_vec;
    const float3 axis = normalize(cross(ortho, view_vec));
    const float3 proj_n = normal - axis * dot(normal, axis);
    const float cos_n = saturate(dot(proj_n, view_vec) / max(length(proj_n), 1e-4));
    const float n = sign(dot(ortho, proj_n)) * acos(cos_n);  // the normal's angle from the view vector, signed to +dir
    uint covered = 0u;
    for (uint side = 0u; side < 2u; ++side) {
      const float sgn = side == 0u ? 1.0 : -1.0;  // +dir, then -dir
      for (uint step = 0u; step < nsteps; ++step) {
        const float jitter = frac(noise.y + float(slice * nsteps * 2u + side * nsteps + step) * 0.6180339887498948);
        float t = (float(step) + jitter) / steps;
        t *= t;
        float2 off = omega * t;
        const float off_len = length(off);
        if (off_len < 1.0) off *= 1.0 / max(off_len, 1e-4);  // at least the next pixel
        const uint mip = 1u + uint(clamp(log2(max(off_len, 1.0)) - 2.3, 0.0, 3.0));
        const float2 suv = uv + sgn * off * ps;
        if (any(suv < 0.0) || any(suv > 1.0)) break;
        const float sz = SSampleZ(suv, mip);
        if (sz >= S_FAR * 0.5) continue;
        const float3 delta = SViewPos(suv, sz) - pos;
        const float dist = length(delta);
        if (dist < 1e-3 || dist > radius * 2.0) continue;
        const float3 h_front = delta / dist;
        const float3 h_back = normalize(delta - view_vec * thickness);
        const float a_front = acos(clamp(dot(h_front, view_vec), -1.0, 1.0));
        const float a_back = acos(clamp(dot(h_back, view_vec), -1.0, 1.0));
        const float p_front = (sgn * a_front - n) / S_PI + 0.5, p_back = (sgn * a_back - n) / S_PI + 0.5;
        const uint sectors = SSectors(min(p_front, p_back), max(p_front, p_back));
        const uint fresh = sectors & ~covered;
        if (fresh != 0u) {
          float facing = 1.0;
          if (backface) facing = smoothstep(-0.05, 0.15, dot(s_normal.Load(int3(SHalfPixel(suv), 0)).xyz, -h_front));
          gi += (float(countbits(fresh)) / 32.0) * saturate(dot(normal, h_front)) * facing *
                s_light.Load(int3(SHalfPixel(suv), 0)).rgb;
        }
        covered |= sectors;
      }
    }
    open += 1.0 - float(countbits(covered)) / 32.0;
  }
  // The distance fade (here at half resolution, so the composite needs no scale): out over the last quarter.
  s_gi[pix] = float4(gi / slices * saturate((fade - z) / (fade * 0.25)), open / slices);
}
)";

// The scene's scale, once per frame (one group): the median view distance of the depth pyramid's smallest level (MIP 4,
// sky left out), as a 64-bin log histogram, smoothed in log space; between the "near" median (a room) and the "far" one
// (open ground) the radius, thickness, fade and steps go from their near to their far values. Its own constants:
//   c0: radius near, far; fade near, far          c1: thickness near, far; steps near, far
//   c2: the median view z where the near values apply, where the far ones do; the weight of the new value (smoothing);
//       1 = jump to it (a camera cut, a gap)      c3: x 1 = adaptive (0 = the near values always)
// t0 = view-depth pyramid; u0 = 2x1: [0] radius, thickness, fade, steps (what the main pass reads), [1] the smoothed
// median, the position between near and far (0..1), this frame's median (-1 = none), the texels counted.
// Dispatch 1 x 1 x 1.
constexpr const char* kSsilScaleCs = R"(
cbuffer SsilScale : register(b0) { float4 a_c0; float4 a_c1; float4 a_c2; float4 a_c3; };
Texture2D<float> s_vdepth : register(t0);
// Read and written: its format is declared for SPIR-V (a formatless storage read needs an optional Vulkan
// capability); D3D12 ignores the attribute. The texture is always RGBA32F.
[[vk::image_format("rgba32f")]] RWTexture2D<float4> s_scale : register(u0);
groupshared uint g_hist[64];
groupshared uint g_total;
[numthreads(256, 1, 1)]
void main(uint gi : SV_GroupIndex) {
  if (gi < 64u) g_hist[gi] = 0u;
  if (gi == 0u) g_total = 0u;
  GroupMemoryBarrierWithGroupSync();
  uint w, h, levels;
  s_vdepth.GetDimensions(0u, w, h, levels);
  const uint mip = levels > 4u ? 4u : levels - 1u;
  s_vdepth.GetDimensions(mip, w, h, levels);
  for (uint i = gi; i < w * h; i += 256u) {
    const float z = s_vdepth.Load(int3(int(i % w), int(i / w), int(mip)));
    if (z > 0.0 && z < 5.0e5) {  // 1e6 = sky
      const uint b = min(uint(max(log2(max(z, 16.0)) - 4.0, 0.0) * 4.0), 63u);  // 16 .. 2^20 units, 4 bins per doubling
      InterlockedAdd(g_hist[b], 1u);
      InterlockedAdd(g_total, 1u);
    }
  }
  GroupMemoryBarrierWithGroupSync();
  if (gi != 0u) return;
  float median = -1.0;
  if (g_total >= 16u) {
    const uint half_count = (g_total + 1u) / 2u;
    uint acc = 0u;
    for (uint b = 0u; b < 64u; ++b) {
      acc += g_hist[b];
      if (acc >= half_count) { median = exp2(4.0 + (float(b) + 0.5) / 4.0); break; }
    }
  }
  // The last smoothed median counts only if this is not a jump (first use, a gap, a cut) and it is finite and positive
  // (the texture starts undefined, and a first frame without a median used to copy that forward).
  const float4 prev = s_scale[uint2(1, 0)];
  const bool prev_ok = a_c2.w < 0.5 && !isinf(prev.x) && !isnan(prev.x) && prev.x > 0.0;
  float smoothed;
  if (median > 0.0) smoothed = prev_ok ? exp2(lerp(log2(prev.x), log2(median), saturate(a_c2.z))) : median;
  else smoothed = prev_ok ? prev.x : a_c2.x;  // no median this frame: keep the last, or start at the near values
  float t = 0.0;
  if (a_c3.x > 0.5) t = saturate((log2(smoothed) - log2(a_c2.x)) / max(log2(a_c2.y) - log2(a_c2.x), 1e-3));
  s_scale[uint2(0, 0)] = float4(lerp(a_c0.x, a_c0.y, t), lerp(a_c1.x, a_c1.y, t), lerp(a_c0.z, a_c0.w, t),
                                round(lerp(a_c1.z, a_c1.w, t)));
  s_scale[uint2(1, 0)] = float4(smoothed, t, median, float(g_total));
}
)";

// Temporal accumulation: this frame's indirect light blended into last frame's, reprojected with the motion vectors
// (bilinear, each tap kept only where its view z is within 5% of this pixel's - newly uncovered surfaces start afresh).
// 12% of each new frame: the per-frame noise pattern (S_NOISE moves while an upscaler runs) averages out over about
// 8 frames. S_PARAM bit 0: the history is valid. t0 = this frame's indirect light, t1 = history, t2 = motion vectors
// (full, render pixels, previous - current), t3 = view-depth pyramid; u0 = the new history (rgb, a = view z).
// Dispatch (half w + 7) / 8 x (half h + 7) / 8.
constexpr const char* kSsilTemporalCs = R"(
Texture2D<float4> s_cur : register(t0);
Texture2D<float4> s_hist : register(t1);
Texture2D<float2> s_motion : register(t2);
Texture2D<float> s_vdepth : register(t3);
RWTexture2D<float4> s_out : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= S_SIZE)) return;
  const int2 p = int2(id.xy);
  const float z = s_vdepth.Load(int3(p, 1));
  const float3 cur = s_cur.Load(int3(p, 0)).rgb;
  float4 o = float4(cur, z);
  if ((S_PARAM & 1u) != 0u && z < S_FAR * 0.5) {
    // The texel's own centre in UV plus the motion in UV: with no motion the history is read exactly at this texel
    // (the full-resolution pixel's centre was used before, a quarter texel off, which drifted through the feedback).
    const float2 prev = (float2(p) + 0.5) / float2(S_SIZE) + s_motion.Load(int3(min(p * 2 + 1, int2(S_SCENE) - 1), 0)) / float2(S_SCENE);
    if (all(prev >= 0.0) && all(prev < 1.0)) {
      const float2 hp = prev * float2(S_SIZE) - 0.5;
      const int2 b = int2(floor(hp));
      const float2 f = hp - float2(b);
      float3 sum = 0.0;
      float wsum = 0.0;
      for (int k = 0; k < 4; ++k) {
        const int2 o2 = int2(k & 1, k >> 1);
        const float4 h = s_hist.Load(int3(clamp(b + o2, int2(0, 0), int2(S_SIZE) - 1), 0));
        const float w = (o2.x ? f.x : 1.0 - f.x) * (o2.y ? f.y : 1.0 - f.y) * (abs(h.a - z) < 0.05 * z ? 1.0 : 0.0);
        sum += h.rgb * w;
        wsum += w;
      }
      if (wsum > 0.25) o.rgb = lerp(sum / wsum, cur, 0.12);
    }
  }
  s_out[p] = o;
}
)";

// "Only the difference": the indirect light minus 80% of its wide, depth-aware blur (per channel, nothing below 0) - the
// game's baked lighting already holds the even, average bounce; what is left is the local part (colour bleeding next to
// things). t0 = indirect light, t1 = its wide blur; u0 = the difference. Dispatch (half w + 7) / 8 x (half h + 7) / 8.
constexpr const char* kSsilDiffCs = R"(
Texture2D<float4> s_gi : register(t0);
Texture2D<float4> s_wide : register(t1);
RWTexture2D<float4> s_out : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= S_SIZE)) return;
  const float4 g = s_gi.Load(int3(id.xy, 0));
  s_out[id.xy] = float4(max(g.rgb - 0.8 * s_wide.Load(int3(id.xy, 0)).rgb, 0.0), g.a);
}
)";

// Depth-aware 5x5 blur at half resolution, taps S_PARAM pixels apart (two passes: 1, then 2). t0 = indirect light,
// t1 = view-depth pyramid (MIP 1 = this grid); u0 = blurred. Dispatch (half w + 7) / 8 x (half h + 7) / 8.
constexpr const char* kSsilBlurCs = R"(
Texture2D<float4> s_in : register(t0);
Texture2D<float> s_vdepth : register(t1);
RWTexture2D<float4> s_out : register(u0);
float SLoadZ(int2 p) {
  uint w, h, l;
  s_vdepth.GetDimensions(1u, w, h, l);
  return s_vdepth.Load(int3(clamp(p, int2(0, 0), int2(w, h) - 1), 1));
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= S_SIZE)) return;
  const int2 c = int2(id.xy), m = int2(S_SIZE) - 1;
  const float zc = SLoadZ(c);
  const float4 centre = s_in.Load(int3(c, 0));
  if (zc >= S_FAR * 0.5) { s_out[c] = centre; return; }
  const int step = int(max(S_PARAM, 1u));
  float4 sum = 0.0;
  float wsum = 0.0;
  for (int dy = -2; dy <= 2; ++dy) {
    for (int dx = -2; dx <= 2; ++dx) {
      const int2 p = clamp(c + int2(dx, dy) * step, int2(0, 0), m);
      const float z = SLoadZ(p);
      const float w = exp(-float(dx * dx + dy * dy) / 4.5) * saturate(1.0 - abs(z - zc) / (zc * 0.03));
      sum += s_in.Load(int3(p, 0)) * w;
      wsum += w;
    }
  }
  s_out[c] = wsum > 1e-4 ? sum / wsum : centre;
}
)";

// The composite: the scene + indirect light x albedo estimate x strength, into a copy of the whole target (the resolve
// and the upscaler read it instead of the target). The half-resolution light is brought up depth-aware. Albedo
// (S_ALBEDO): 0 = the scene colour's hue at a fixed 50% (a guess: no albedo buffer), 1 = grey 50%, 2 = the scene colour
// itself (brighter lit surfaces take more), 3 = local: the scene colour over the brightness of its surroundings (t3, the
// light blurred wide and depth-aware) x 50% - texture detail changes fast and lighting slowly, so a dark material on a
// lit floor stays dark while a light wall in shadow counts as light. t0 = scene colour, t1 = indirect light (half),
// t2 = view-depth pyramid, t3 = local light (half); u0 = output. Grid = the target's whole size; S_SCENE = the scene's. Dispatch (w + 7) / 8 x (h + 7) / 8.
constexpr const char* kSsilApplyCs = R"(
Texture2D<float4> s_scene : register(t0);
Texture2D<float4> s_gi : register(t1);
Texture2D<float> s_vdepth : register(t2);
Texture2D<float4> s_illum : register(t3);
RWTexture2D<float4> s_out : register(u0);
float SLoadZ(int2 p, uint mip) {
  uint w, h, l;
  s_vdepth.GetDimensions(mip, w, h, l);
  return s_vdepth.Load(int3(clamp(p, int2(0, 0), int2(w, h) - 1), mip));
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= S_SIZE)) return;
  const int2 p = int2(id.xy);
  const float4 c = s_scene.Load(int3(p, 0));
  if (any(id.xy >= S_SCENE)) { s_out[p] = c; return; }
  const float z = SLoadZ(p, 0u);
  if (z >= S_FAR * 0.5) { s_out[p] = c; return; }
  uint gw, gh;
  s_gi.GetDimensions(gw, gh);
  const float2 hp = (float2(p) + 0.5) * 0.5 - 0.5;
  const int2 b = int2(floor(hp));
  const float2 f = hp - float2(b);
  float3 gi = 0.0;
  float wsum = 0.0;
  for (int k = 0; k < 4; ++k) {
    const int2 o = int2(k & 1, k >> 1);
    const int2 q = clamp(b + o, int2(0, 0), int2(gw, gh) - 1);
    const float wb = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
    const float wd = saturate(1.0 - abs(SLoadZ(q, 1u) - z) / (z * 0.03));
    gi += s_gi.Load(int3(q, 0)).rgb * wb * wd;
    wsum += wb * wd;
  }
  // No tap at this pixel's depth (a thin gap, a silhouette): no indirect light here rather than the neighbours'.
  gi = wsum > 1e-4 ? gi / wsum : 0.0;
  const float3 g = gi * S_STRENGTH;  // the distance fade was applied by the main pass
  const float mx = max(c.r, max(c.g, c.b));
  float3 albedo = 0.5;
  if (S_ALBEDO == 0u) albedo = lerp(0.5.xxx, saturate(c.rgb / max(mx, 1e-4)) * 0.5, saturate(mx * 20.0));
  else if (S_ALBEDO == 2u) albedo = saturate(c.rgb);
  else if (S_ALBEDO == 3u) {
    const float3 local = s_illum.Load(int3(clamp(int2(id.xy) / 2, int2(0, 0), int2(gw, gh) - 1), 0)).rgb;
    albedo = min(c.rgb / max(SLum(local), 1e-3) * 0.5, 0.9);
  }
  float3 o = c.rgb + g * albedo;
  if (S_VIEW == 1u) o = g * 0.5;                                  // the indirect light alone (on grey)
  else if (S_VIEW == 2u && p.x < int(S_SCENE.x / 2u)) o = c.rgb;  // split: left without, right with
  s_out[p] = float4(o, c.a);
}
)";

}  // namespace xenos::renderer::builtin
