// "Match my monitor": the renderer's present resample (builtin_shaders.h kPresentResampleCs), copied for gn_vktest.

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
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= c0.z || id.y >= c0.w) return;
  const float2 s = float2(c0.xy) / float2(c0.zw);  // source pixels per output pixel
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
