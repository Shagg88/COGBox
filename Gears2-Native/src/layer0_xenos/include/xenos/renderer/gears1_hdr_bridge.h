// Captured Gears 1 UE3 analytic grade bridge. No game shader data. MIT.
#pragma once
#include <cstdint>
#include <string>
#include <xenos/renderer/psychov35_source.h>
#include <xenos/renderer/ue3_grade_bridge.h>
namespace xenos::renderer::native_hdr {
inline std::string Gears1SceneSource(std::string source, uint32_t low, uint32_t high) {
  const auto constant = [=](uint32_t index) {
    if (low != high && index >= high)
      index = low + index - high;
    return "xe_c[" + std::to_string(index) + "]";
  };
  const auto replace = [&](const std::string& from, const std::string& to) {
    const auto at = source.find(from);
    if (at == std::string::npos || source.find(from, at + from.size()) != std::string::npos)
      return false;
    source.replace(at, from.size(), to);
    return true;
  };
  // Same strategy as the user's RenoDX PC Redux mod: keep the DOF/bloom
  // weights, shadow offsets, channel grade, overlay and gamma; bypass only
  // the two upper RGB clamps in a parallel analytic path before display mapping.
  std::string grade = R"(
float3 NativeGears1Grade(float3 scene,bool bounded=false) {
  float3 shifted=max(scene-$C3.yxz,0.0);
  if(bounded)shifted=saturate(shifted);
  float3 scaled=abs(shifted.zxy*$C4.zyx);
  float3 contrast=pow(max(scaled,0.0),$C5.zyx).zyx;
  float luminance=dot(contrast.zxy,$C6.zxy);
  float3 mixed=contrast*$C3.w+$C8.xyz+luminance;
  float3 output=max(mixed*$C7.xyz,0.0);
  return pow(bounded?saturate(output):output,($C7.w*2.2).xxx);
}
// The clipped SDR companion cannot supply the hue of a bright muzzle flash.
// Roll off luminance with PsychoV35, then apply one RGB scale to the actual
// extended grade. Preserve the authored shadow/diffuse reference unchanged.
float3 NativeGears1Map(float3 extendedGrade,float3 reference,float peak) {
  reference=max(reference,0.0);
  if(all(reference==0.0))return 0.0;
  extendedGrade=max(extendedGrade,0.0);
  float y=dot(extendedGrade,float3(0.2126,0.7152,0.0722));
  float referenceY=dot(reference,float3(0.2126,0.7152,0.0722));
  float mappedY=NativeUe3DisplayMap(y.xxx,peak,max(12.0,peak*2.0)).x;
  float3 mapped=y>1e-8?extendedGrade*(max(mappedY,referenceY)/y):reference;
  float weight=smoothstep(0.5,1.0,max(extendedGrade.r,max(extendedGrade.g,extendedGrade.b)));
  return NativeFitHdr(lerp(reference,mapped,weight),peak);
}
)";
  for (unsigned index : {3u, 4u, 5u, 6u, 7u, 8u}) {
    const auto token = "$C" + std::to_string(index);
    size_t at = 0;
    while ((at = grade.find(token, at)) != std::string::npos) {
      const auto value = constant(index);
      grade.replace(at, token.size(), value);
      at += value.size();
    }
  }
  if (!replace("XePsOut main(XePsIn xe_in) {",
               grade + "\nXePsOut main(XePsIn xe_in) {\n  float3 nativeGears1Scene=0.0;\n  bool "
                       "nativeGears1Coverage=true;\n"))
    return {};
  const auto capture =
      "float4 s0 = r[1].yzxx;\n        float4 s1 = r[0].xxxx;\n        float4 s2 = -(" +
      constant(3) + ".yxzz);";
  if (!replace(capture,
               "nativeGears1Coverage=r[0].x>0.0 && isfinite(r[0].x) && "
               "all(isfinite(r[1].yzx*r[0].x));\n        "
               "nativeGears1Scene=NativeNormalizeScene(r[1].yzx,r[0].x,0.0,r[3].yxz);\n        " +
                   capture))
    return {};
  const std::string begin = "  // Linear-light composition in scene reference-white units.\n";
  const std::string end = "  o.color0.rgb = nativeColor;";
  const auto start = source.find(begin), finish = source.find(end);
  if (start == std::string::npos || finish == std::string::npos || finish < start ||
      source.find(begin, start + begin.size()) != std::string::npos ||
      source.find(end, finish + end.size()) != std::string::npos)
    return {};
  source.replace(
      start, finish + end.size() - start,
      "  o.color1=o.color0; // exact original encoded SDR companion\n"
      "  float3 nativeReference=pow(max(o.color0.rgb,0.0),2.2);\n"
      "  "
      "if(!nativeGears1Coverage)nativeReference=NativeGears1Grade(nativeGears1Scene,true)*pow(max("
      "xe_color_exp_scale.x,0.0),2.2);\n"
      "  float3 "
      "nativeGrade=NativeGears1Grade(nativeGears1Scene)*pow(max(xe_color_exp_scale.x,0.0),2.2);\n"
      "  float nativePeak=max(asfloat(xe_sys2.x),1e-4);\n"
      "  "
      "o.color0.rgb=NativeGears1Map(nativeGrade,nativeReference,nativePeak);\n");
  return std::string(kPsycho35Hlsl) + kUe3GradeHlsl + source;
}
}  // namespace xenos::renderer::native_hdr
