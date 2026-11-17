// Authored glue for the two fingerprint-checked Judgment FXAA permutations. MIT.
#pragma once
#include <cstdint>
#include <string>
namespace xenos::renderer::native_hdr {
inline constexpr const char* kJudgmentFxaaSampling=R"(
XE_VK(31,1) Texture2D<float4> nativeFxaaScene:register(t31,space0);
int NativeFxaaAddress(int p,int n,uint mode) {
  if(mode==0u)return ((p%n)+n)%n;
  return clamp(p,0,n-1);
}
float3 NativeFxaaTexel(int2 p) {
  p=int2(NativeFxaaAddress(p.x,int(nativeFxaaParams.x),nativeFxaaParams.z&255u),
         NativeFxaaAddress(p.y,int(nativeFxaaParams.y),nativeFxaaParams.z>>8));
  float3 v=pow(max(nativeFxaaScene.Load(int3(p,0)).rgb,0.0),1.0/2.2);
  // Match the old encoded FP16 proxy's texel precision, without generating it.
  return f16tof32(f32tof16(v));
}
float3 NativeFxaaSample(float2 uv) {
  float2 p=uv*float2(nativeFxaaParams.xy)-0.5;
  int2 base=int2(floor(p));float2 f=round(frac(p)*256.0)/256.0;
  return lerp(lerp(NativeFxaaTexel(base),NativeFxaaTexel(base+int2(1,0)),f.x),
              lerp(NativeFxaaTexel(base+int2(0,1)),NativeFxaaTexel(base+1),f.x),f.y);
}
)";
inline std::string JudgmentFxaaPairSource(std::string source,bool high,uint32_t low,uint32_t hi) {
  const auto replace=[&](const std::string& a,const std::string& b){
    const auto at=source.find(a);if(at==std::string::npos||source.find(a,at+a.size())!=std::string::npos)return false;
    source.replace(at,a.size(),b);return true;
  };
  if(!replace("  uint4 xe_sys1;","  uint4 xe_sys1;\n  uint4 nativeFxaaParams;"))return {};
  std::string declarations="float2 nativeUv0=0.0,nativeUv1=0.0,nativeUv2=0.0,nativeUv3=0.0,nativeUv4=0.0;";
  if(!replace("XePsOut main(XePsIn xe_in) {",std::string(kJudgmentFxaaSampling)+"\nXePsOut main(XePsIn xe_in) {\n  "+declarations))return {};
  const std::string sample="float4 tv = xe_tex_post(xe_tex_2d_0.SampleLevel(xe_sampler_0, tc, 0), 0u);";
  size_t pos=0;unsigned colors=0;
  while((pos=source.find(sample,pos))!=std::string::npos) {
    const auto end=source.find('}',pos);if(end==std::string::npos)return {};
    const auto block=source.substr(pos,end-pos);
    if(block.find("= tv.x;")!=std::string::npos || block.find("= tv.y;")!=std::string::npos || block.find("= tv.z;")!=std::string::npos) {
      const std::string save="\n          nativeUv"+std::to_string(high?0:colors)+"=tc;";
      pos+=sample.size();source.insert(pos,save);pos+=save.size();++colors;
    } else pos+=sample.size();
  }
  if(colors!=(high?2u:5u))return {};
  std::string output;
  if(high)output="  float3 nativeEncoded=xe_tex_post(float4(NativeFxaaSample(nativeUv0),1.0),0u).rgb;\n";
  else {
    const uint32_t index=low!=hi && 255>=hi?low+255-hi:255;
    const auto c="xe_c["+std::to_string(index)+"]";
    output="  float3 nativeEncoded=xe_tex_post(float4(NativeFxaaSample(nativeUv0),1.0),0u).rgb;\n"
      "  [branch] if(p0) {\n"
        "    float3 "
        "inner=xe_tex_post(float4(NativeFxaaSample(nativeUv3),1.0),0u).rgb+xe_tex_post(float4("
        "NativeFxaaSample(nativeUv4),1.0),0u).rgb;\n"
        "    [branch] if(r[0].w==0.0) {\n"
        "      float3 "
        "outer=xe_tex_post(float4(NativeFxaaSample(nativeUv1),1.0),0u).rgb+xe_tex_post(float4("
        "NativeFxaaSample(nativeUv2),1.0),0u).rgb;\n"
        "      nativeEncoded=(outer+inner)*"+c+".y;\n"
      "    } else nativeEncoded=inner*"+c+".x;\n  }\n";
  }
  output+="  o.color1=float4(pow(max(nativeEncoded,0.0),2.2),o.color0.a);\n  return o;";
  if(!replace("  return o;",output))return {};
  return source;
}
} // namespace xenos::renderer::native_hdr
