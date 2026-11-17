// Selective scene-reading effects. Guest storage remains unchanged. MIT.
#pragma once
#include <cstdint>
#include <xenos/renderer/gpu.h>

namespace xenos::renderer::native_hdr {
struct SceneEffectProfile { uint64_t hash, fingerprint; uint32_t words; };
inline const SceneEffectProfile* FindSceneEffect(uint64_t hash) {
  static constexpr SceneEffectProfile profiles[] = {
    {0x8356D7C3D65FE328ull,0xBB708F8C299FD402ull,96},
    {0x212F89E82422D29Dull,0x24D849D9DA7DFED0ull,81},
      // Gears 1 final motion blur: scene t0, original depth t1 and velocity t2.
      {0x5712BE71ADB606F5ull, 0x73824EB49CB1E8EAull, 123},
    // Judgment FXAA. Original alpha luma and edge/UV logic stay intact.
    {0x8309620DF190ABDFull,0xBD5BB78CFE396879ull,135},
    {0x16CBC2E3D5BEC0A5ull,0x707DDFD1C3744044ull,345},
  };
  for(const auto& p:profiles)if(p.hash==hash)return &p;
  return nullptr;
}
struct SceneLease {
  uint64_t frame=0, write_generation=0, memory_version=0;
  uint32_t owner_id=0;
  bool Current(uint64_t f,uint32_t owner,uint64_t writes,uint64_t memory) const {
    return owner_id && frame==f && owner_id==owner && write_generation==writes && memory_version==memory;
  }
};
// Only complete, untransformed RGBA8 resolves are admitted in this first slice.
inline bool DirectSceneResolve(uint32_t format,int32_t exponent,bool swap,uint32_t endian,
    int32_t x,int32_t y,uint32_t dx,uint32_t dy,uint32_t w,uint32_t h,uint32_t pitch) {
  (void)swap; // R/B copy swaps are composed with fetch selectors, never guessed.
  return format==6 && !exponent && endian<4 && !x && !y && !dx && !dy && w && h && w==pitch;
}
inline uint8_t SceneComponent(uint32_t selector,bool copy_swap) {
  if(selector>=4)return selector==4?4:5;
  return uint8_t(copy_swap && (selector==0 || selector==2)?2-selector:selector);
}
inline bool SceneFetchCompatible(const uint32_t* f,uint32_t endian,uint32_t pitch) {
  // Unsigned RGBA8, no exponent adjustment, no extra mip levels. The caller
  // validates the bound allocation and maps raw fetch selectors onto FP16 RGBA.
  return f && (f[0]>>31) && ((f[0]>>22)&511)*32==pitch && ((f[5]>>9)&3)==1 && !(f[1]&(1u<<10)) &&
      !(f[3]&1) && ((f[3]>>10)&7)==3 && (f[1]&63)==6 && ((f[1]>>6)&3)==endian && !(f[0]&0x3FCu) &&
      !(f[3]&(63u<<13)) && !((f[4]>>2)&15u) && !((f[4]>>6)&15u);
}
inline constexpr uint64_t kSceneEffectBudget=128ull<<20;
inline bool SceneEffectAllocationFits(uint64_t current,uint64_t old,uint32_t w,uint32_t h) {
  const uint64_t bytes=uint64_t(w)*h*8;
  return w && h && old<=current && bytes<=kSceneEffectBudget && current-old<=kSceneEffectBudget-bytes;
}
// Material shaders keep their original encoded-domain filtering and alpha.
// Convert once on actual consumption, over visible pixels only, never padding.
inline constexpr const char* kEncodeSceneForEffects=R"(
Texture2D<float4> source : register(t0);
RWTexture2D<float4> dest : register(u0);
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
  uint w,h;dest.GetDimensions(w,h);if(p.x>=w||p.y>=h)return;
  float4 v=source.Load(int3(p.xy,0));
  dest[p.xy]=float4(pow(max(v.rgb,0.0),1.0/2.2),saturate(v.a));
}
)";
// FXAA uses the original 8-bit luma in alpha. The Judgment scene epilogue
// quantizes it before FP16 storage; restore that lattice before filtering.
// RGB stays unbounded. No extra full-resolution texture read is needed.
inline constexpr const char* kEncodeSceneForFxaa=R"(
Texture2D<float4> source : register(t0);
RWTexture2D<float4> dest : register(u0);
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
  uint w,h;dest.GetDimensions(w,h);if(p.x>=w||p.y>=h)return;
  float4 v=source.Load(int3(p.xy,0));
  dest[p.xy]=float4(pow(max(v.rgb,0.0),1.0/2.2),round(saturate(v.a)*255.0)/255.0);
}
)";
} // namespace xenos::renderer::native_hdr
