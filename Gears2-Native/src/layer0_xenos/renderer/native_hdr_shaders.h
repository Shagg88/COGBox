// Native HDR presentation. Authored HLSL; no game-derived shader contents.
#pragma once
#include <string>
namespace xenos::renderer::native_hdr {
// Same EDRAM address mapping as the renderer's colour alias transfer. Pixels
// outside the visible HDR extent retain their authoritative original colour.
inline constexpr const char* kTransferCompanionPs = R"(
Texture2D<float4> companion : register(t0);
Texture2D<float4> reference : register(t1);
cbuffer Alias : register(b0) { uint4 t0; uint4 t1; uint4 t2; uint4 t3; };
float4 main(float4 pos : SV_Position) : SV_Target0 {
  uint rs=max(t3.x,1u),rss=max(t3.y,1u),tw=max(t1.x,1u);
  uint2 hp=uint2(pos.xy),p=hp/rs,sub=hp%rs;
  uint tile=(t0.x+(p.y/16u)*t0.y+p.x/tw)%2048u;
  uint rel=(tile+2048u-t0.z)%2048u;
  uint2 q=uint2((rel%t0.w)*tw+p.x%tw,(rel/t0.w)*16u+p.y%16u)*rss+(sub*rss)/rs;
  uint width=t2.z,height=t2.w;
  if(all(q<uint2(width,height))) return companion.Load(int3(q,0));
  float4 original=reference.Load(int3(q,0));
  return float4(pow(max(original.rgb,0.0),2.2),saturate(original.a));
}
)";
inline constexpr const char* kSeedReferenceCs = R"(
Texture2D<float4> source : register(t0);
RWTexture2D<float4> companion : register(u0);
cbuffer Region : register(b0) { uint4 region; };
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= region.xy)) return;
  uint2 pixel = id.xy + region.zw;
  float4 color = source.Load(int3(pixel,0));
  companion[pixel] = float4(pow(max(color.rgb,0.0),2.2), saturate(color.a));
}
)";
inline constexpr const char* kSceneProxyCs = R"(
Texture2D<float4> scene : register(t0);
Texture2D<float4> reference : register(t1);
RWTexture2D<float4> proxy : register(u0);
cbuffer Size : register(b0) { uint4 dimensions; uint4 options; float4 transform; };
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= dimensions.xy)) return;
  uint2 pixel=id.xy*dimensions.z/max(dimensions.w,1u);
  float4 original=reference.Load(int3(id.xy,0));
  float4 saved=0;
  bool covered=all(pixel<options.zw);
  if(covered) saved=scene.Load(int3(pixel,0));
  float3 color=pow(max(original.rgb,0.0),2.2);
  if(covered && (options.x!=0 || saved.a>0.5)) {
    color=saved.rgb*transform.x;
    if(options.y!=0)color=color.bgr;
  }
  color=float3(isfinite(color.r)?color.r:0,isfinite(color.g)?color.g:0,isfinite(color.b)?color.b:0);
  proxy[id.xy]=float4(pow(max(color,0.0),1.0/2.2),saturate(original.a));
}
)";
inline constexpr const char* kResolveCompanionCs = R"(
Texture2D<float4> source : register(t0);
RWTexture2D<float4> snapshot : register(u0);
cbuffer Copy : register(b0) {
  uint4 region; // copied width/height, destination x/y (host pixels)
  int4 origin;  // source guest x/y, source scale, destination scale
  uint4 extent; // source visible width/height, source available, unused
  float4 transform; // linear gain, swap red/blue, unused
};
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
  if(any(id.xy>=region.xy)) return;
  int2 pixel=origin.xy*origin.z+int2(id.xy)*origin.z/origin.w;
  bool valid=extent.z!=0 && all(pixel>=0) && all(pixel<int2(extent.xy));
  float3 color=0;
  if(valid) {
    color=max(source.Load(int3(pixel,0)).rgb,0.0)*transform.x;
    if(transform.y!=0) color=color.bgr;
  }
  snapshot[id.xy+region.zw]=float4(min(color,65504.0),valid?1.0:0.0);
}
)";
inline constexpr const char* kResolvedViewCs = R"(
Texture2D<float4> snapshot : register(t0);
Texture2D<float4> reference : register(t1);
Texture2D<uint> packedReference : register(t2);
RWTexture2D<float4> viewImage : register(u0);
cbuffer View : register(b0) { uint4 dimensions; uint4 options; float4 transform; uint4 extent; };
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
  if(any(id.xy>=dimensions.xy)) return;
  uint2 pixel=id.xy*dimensions.z/max(dimensions.w,1u);
  uint width,height;snapshot.GetDimensions(width,height);
  float4 saved=0;
  bool covered=all(pixel<min(uint2(width,height),extent.xy));
  if(covered) saved=snapshot.Load(int3(pixel,0));
  float4 original;
  if(options.x!=0) {
    uint word=packedReference.Load(int3(id.xy,0));
    original=float4(float3(word&1023u,(word>>10u)&1023u,(word>>20u)&1023u)/1023.0,1.0);
  } else original=reference.Load(int3(id.xy,0));
  float3 color=pow(max(original.rgb,0.0),2.2);
  if(covered && (options.z!=0 || saved.a>0.5)) {
    // The reference is already in the fetch/presentation channel order. Map
    // only the stored HDR channels; never swizzle the reference a second time.
    for(uint c=0;c<3;++c) {
      uint selector=(options.y>>(c*3))&7u;
      if(selector<3u) color[c]=saved[selector]*transform.x;
      else if(selector==4u) color[c]=0;
      else if(selector==5u) color[c]=1;
    }
  }
  viewImage[id.xy]=float4(max(color,0.0),saturate(original.a));
}
)";
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
  uint2 pixel = id.xy + dimensions.zw;
  layer[pixel] = scene.Load(int3(pixel, 0));
}
)";
// Pair evaluation starts from the current pre-effect background. Its sampled
// scene input may be an older resolve and is prepared separately from that
// resolve's frozen native snapshot.
inline constexpr const char* kPrepareSceneOverlayCs = R"(
Texture2D<float4> scene : register(t0);
Texture2D<float4> reference : register(t1);
RWTexture2D<float4> highLayer : register(u0);
RWTexture2D<float4> lowLayer : register(u1);
cbuffer Size : register(b0) { uint4 dimensions; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= dimensions.xy)) return;
  uint2 pixel=id.xy+dimensions.zw;
  float4 low = reference.Load(int3(pixel, 0));
  highLayer[pixel] = float4(pow(max(scene.Load(int3(pixel, 0)).rgb, 0.0), 1.0 / 2.2), low.a);
  lowLayer[pixel] = low;
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
  uint2 pixel=id.xy+dimensions.zw;
  float4 bounded = boundedResult.Load(int3(pixel, 0));
  float3 h = pow(max(hdrLayer.Load(int3(pixel, 0)).rgb, 0.0), 2.2);
  float3 s = pow(max(sdrLayer.Load(int3(pixel, 0)).rgb, 0.0), 2.2);
  // Equal UI-only emission cancels. Carry the remaining native scene energy
  // as positive luminance in the ORIGINAL material's color direction. Adding
  // h-s per channel can cross zero in just one channel as the effect changes,
  // turning a red SDR reference cyan for a frame.
  const float3 luma = float3(0.2126, 0.7152, 0.0722);
  float3 base = pow(max(bounded.rgb, 0.0), 2.2);
  float baseY = dot(base, luma);
  float extraY = max(dot(h - s, luma), 0.0);
  float3 color = baseY > 1e-6 ? base * (1.0 + extraY / baseY) : base;
  color = float3(isfinite(color.r) ? color.r : 0.0,
                 isfinite(color.g) ? color.g : 0.0,
                 isfinite(color.b) ? color.b : 0.0);
  float peak = max(settings.x, 1e-6);
  float brightest = max(color.r, max(color.g, color.b));
  color *= peak / max(peak, brightest);
  outputImage[pixel] = float4(max(color, 0.0), bounded.a);
}
)";
}  // namespace xenos::renderer::native_hdr
