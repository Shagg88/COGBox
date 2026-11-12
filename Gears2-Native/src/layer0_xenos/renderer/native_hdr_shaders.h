// Native HDR presentation. Authored HLSL; no game-derived shader contents.
#pragma once
#include <string>
namespace xenos::renderer::native_hdr {
inline constexpr const char* kEncodeCs = R"(
Texture2D<float4> nativeImage : register(t0);
Texture2D<uint> referenceImage : register(t1);
RWTexture2D<uint> outputImage : register(u0);
cbuffer HdrPresentation : register(b0) {
  uint4 dimensions; // output width/height, native width/height
  float4 display;   // scene white nits, peak nits, native enabled, sharpening
  uint4 reserved0;
  uint4 reserved1;
};
float3 ReadNative(int2 pixel) {
  pixel = clamp(pixel, int2(0, 0), int2(dimensions.zw) - 1);
  float3 color = nativeImage.Load(int3(pixel, 0)).rgb;
  color = float3(isfinite(color.r) ? color.r : 0.0,
                 isfinite(color.g) ? color.g : 0.0,
                 isfinite(color.b) ? color.b : 0.0);
  return max(color, 0.0);
}
float3 BilinearNative(float2 pixel) {
  int2 origin = int2(floor(pixel));
  float2 weight = frac(pixel);
  return lerp(lerp(ReadNative(origin), ReadNative(origin + int2(1, 0)), weight.x),
              lerp(ReadNative(origin + int2(0, 1)), ReadNative(origin + 1), weight.x), weight.y);
}
float3 ResizeNative(float2 pixel) {
#ifdef NATIVE_HDR_MATCHING_SIZE
  // Pixel centres already coincide at native resolution. Avoid four loads,
  // interpolation and the resize loop for every output/sharpening sample.
  return ReadNative(int2(pixel));
#else
  float2 scale = float2(dimensions.zw) / float2(dimensions.xy);
  int2 samples = clamp(int2(ceil(scale)), int2(1, 1), int2(8, 8));
  float3 color = 0.0;
  // Positive, normalized weights preserve HDR peaks and do not create ringing.
  for (int y = 0; y < samples.y; ++y) for (int x = 0; x < samples.x; ++x) {
    float2 offset = (float2(x + 0.5, y + 0.5) / float2(samples) - 0.5) * scale;
    color += BilinearNative((pixel + 0.5) * scale - 0.5 + offset);
  }
  return color / float(samples.x * samples.y);
#endif
}
float3 EncodePq(float3 nits) {
  float3 luminance = saturate(nits / 10000.0);
  float3 power = pow(luminance, 0.1593017578125);
  return pow((0.8359375 + 18.8515625 * power) / (1.0 + 18.6875 * power), 78.84375);
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= dimensions.xy)) return;
  float3 linearColor;
  if (display.z > 0.5) {
    linearColor = ResizeNative(float2(id.xy));
    if (display.w > 0.0) {
      float3 north = ResizeNative(float2(id.xy) + float2(0, -1));
      float3 south = ResizeNative(float2(id.xy) + float2(0, 1));
      float3 west = ResizeNative(float2(id.xy) + float2(-1, 0));
      float3 east = ResizeNative(float2(id.xy) + float2(1, 0));
      float3 low = min(linearColor, min(min(north, south), min(west, east)));
      float3 high = max(linearColor, max(max(north, south), max(west, east)));
      linearColor = clamp(linearColor + display.w * (linearColor - (north + south + west + east) * 0.25), low, high);
    }
  } else {
    // Menus / unprofiled passes remain at reference white. Never infer extra
    // highlight energy from SDR white, including videos and reload overlays.
    uint packed = referenceImage.Load(int3(id.xy, 0));
    float3 encoded = float3(packed & 1023u, (packed >> 10u) & 1023u, (packed >> 20u) & 1023u) / 1023.0;
    linearColor = pow(max(encoded, 0.0), 2.2);
  }
  float3 nits = min(max(linearColor * display.x, 0.0), display.y);
  const float3x3 to2020 = float3x3(0.6274040, 0.3292820, 0.0433136,
                                   0.0690970, 0.9195400, 0.0113612,
                                   0.0163916, 0.0880132, 0.8955950);
  float3 pq = EncodePq(clamp(mul(to2020, nits), 0.0, display.y));
  uint3 code = uint3(round(saturate(pq) * 1023.0));
  outputImage[id.xy] = code.r | (code.g << 10u) | (code.b << 20u) | (3u << 30u);
}
)";

inline std::string MatchingSizeEncoder() {
  return std::string("#define NATIVE_HDR_MATCHING_SIZE 1\n") + kEncodeCs;
}

// Gamma-encoded floating scene proxy for the original material's texture
// fetches. Initialise the SDR MRT with the exact pre-effect scene resolve.
inline constexpr const char* kPrepareSceneUiCs = R"(
Texture2D<float4> scene : register(t0);
Texture2D<float4> reference : register(t1);
RWTexture2D<float4> proxy : register(u0);
RWTexture2D<float4> sdrLayer : register(u1);
cbuffer Size : register(b0) { uint4 dimensions; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= dimensions.xy)) return;
  float4 sdr = reference.Load(int3(id.xy, 0));
  float3 linearColor = max(scene.Load(int3(id.xy, 0)).rgb, 0.0);
  proxy[id.xy] = float4(pow(linearColor, 1.0 / 2.2), sdr.a);
  sdrLayer[id.xy] = sdr;
}
)";
inline constexpr const char* kCopyForUiCs = R"(
Texture2D<float4> scene : register(t0);
RWTexture2D<float4> layer : register(u0);
cbuffer Size : register(b0) { uint4 dimensions; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= dimensions.xy)) return;
  layer[id.xy] = scene.Load(int3(id.xy, 0));
}
)";
inline constexpr const char* kSceneUiBlendCs = R"(
Texture2D<float4> hdrLayer : register(t0);
Texture2D<float4> sdrLayer : register(t1);
Texture2D<float4> boundedResult : register(t2);
RWTexture2D<float4> outputImage : register(u0);
cbuffer Composite : register(b0) {
  uint4 dimensions;
  float4 settings; // peak / scene white
};
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= dimensions.xy)) return;
  float4 bounded = boundedResult.Load(int3(id.xy, 0));
  float3 h = pow(max(hdrLayer.Load(int3(id.xy, 0)).rgb, 0.0), 2.2);
  float3 s = pow(max(sdrLayer.Load(int3(id.xy, 0)).rgb, 0.0), 2.2);
  // Equal UI-only emission cancels. Only the material's response to the real
  // HDR scene survives, added to the game's bounded, vanilla-coloured result.
  float3 color = pow(max(bounded.rgb, 0.0), 2.2) + (h - s);
  color = float3(isfinite(color.r) ? color.r : 0.0,
                 isfinite(color.g) ? color.g : 0.0,
                 isfinite(color.b) ? color.b : 0.0);
  outputImage[id.xy] = float4(clamp(color, 0.0, settings.x), bounded.a);
}
)";
}  // namespace xenos::renderer::native_hdr
