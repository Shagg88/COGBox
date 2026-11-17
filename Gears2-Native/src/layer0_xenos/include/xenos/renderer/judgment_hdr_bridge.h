// Fingerprint-checked Judgment scene grading; authored glue, no game bytecode.
// Same LUT-domain continuation / PsychoV35 design as the Gears 3 profile. MIT.
#pragma once
#include <cstdint>
#include <string>
#include <xenos/renderer/psychov35_source.h>
#include <xenos/renderer/ue3_grade_bridge.h>

namespace xenos::renderer::native_hdr {
inline std::string JudgmentSceneSource(std::string source,uint32_t low,uint32_t high,bool alternate=false,bool optimized=true,bool calibrated=true,bool compressed_lut=true) {
  const auto constant=[=](uint32_t index) {
    if(low!=high && index>=high)index=low+index-high;
    return "xe_c["+std::to_string(index)+"]";
  };
  const auto replace=[&](const std::string& from,const std::string& to) {
    const auto at=source.find(from);
    if(at==std::string::npos || source.find(from,at+from.size())!=std::string::npos)return false;
    source.replace(at,from.size(),to);return true;
  };
  std::string grade=R"(
// Original exposed scene -> exponential/power neutral curve. The vanilla
// program below retains packed-light decoding, DOF, bloom and eye adaptation.
float3 NativeJudgmentNeutral(float3 exposed) {
  float3 q=saturate(exposed);
  float3 powerLog=log2(max(abs(q*$C250.y),1e-30))*$C250.x;
  float3 powered=exp2(clamp(powerLog,-126.0,80.0));
  float3 encoded=saturate((exp2(clamp(q*$C251.y,-126.0,80.0))*$C250.w)*powered);
  return pow(encoded,2.2);
}
float3 NativeJudgmentFiniteNeutral(float3 exposed) {
  const float pivot=.18;
  const float h=pivot/1024.0;
  float3 base=NativeJudgmentNeutral(pivot.xxx);
  float3 slope=max((NativeJudgmentNeutral((pivot+h).xxx)-NativeJudgmentNeutral((pivot-h).xxx))/(2.0*h),0.0);
  return NativeJudgmentNeutral(min(max(exposed,0.0),pivot))+slope*max(exposed-pivot,0.0);
}
// Batman AK's extension matches value and slope before the SDR shoulder.
// Judgment uses exp2(a*x) * abs(s*x)^p, so its derivative is analytic.
float3 NativeJudgmentExtendedNeutral(float3 exposed) {
  const float pivot=.18;
  float3 base=NativeJudgmentNeutral(pivot.xxx);
  float3 slope=max(base*2.2*($C250.x/pivot+$C251.y*0.6931471805599453),0.0);
  slope=select(base>=1.0,0.0,slope);
  return NativeJudgmentNeutral(min(max(exposed,0.0),pivot))+slope*max(exposed-pivot,0.0);
}
float3 NativeJudgmentReadLut(float3 linearInput) {
  float3 encoded=pow(saturate(linearInput),1.0/2.2);
  uint3 size;xe_tex_3d_4.GetDimensions(size.x,size.y,size.z);
  // Judgment's actual centre scale/bias, followed by the original Xbox fetch
  // adjustment. Interpolate the original encoded LUT, then apply its fetch
  // conversion and decode. No identity LUT or calibration texture is substituted.
  float3 coordinate=encoded*$C250.z+$C252.z+0.0014648438/float3(size);
  float4 sampled=NativeUe3Tetrahedral(xe_tex_3d_4,coordinate);
  sampled=xe_tex_post(sampled,4u);
  return pow(max(sampled.rgb,0.0),2.2);
}
float3 NativeJudgmentGradientLut(float3 linearInput) {
  linearInput=max(linearInput,0.0);
  float3 boundaryInput=saturate(linearInput);
  float3 excess=linearInput-boundaryInput;
  float distance=max(excess.r,max(excess.g,excess.b));
  float3 boundary=NativeJudgmentReadLut(boundaryInput);
  if(distance<=0.0)return boundary;
  uint3 size;xe_tex_3d_4.GetDimensions(size.x,size.y,size.z);
  float cell=1.0/max(float(max(size.x,max(size.y,size.z)))-1.0,1.0);
  float stepSize=1.0-pow(max(1.0-cell,0.0),2.2);
  float3 direction=excess/distance;
  float3 previous=NativeJudgmentReadLut(boundaryInput-direction*stepSize);
  float3 edgeSlope=(boundary-previous)/stepSize;
  float3 usefulSlope=edgeSlope;
  [unroll]for(int i=2;i<=4;++i) {
    float3 current=NativeJudgmentReadLut(boundaryInput-direction*(stepSize*i));
    float3 candidate=(previous-current)/stepSize;
    usefulSlope=select(abs(usefulSlope)<=1e-5,candidate,usefulSlope);previous=current;
  }
  return NativeUe3ContinueLut(boundary,edgeSlope,usefulSlope,distance,stepSize);
}
// Inspired by the max-channel compress -> vanilla LUT -> restore bridges
// in RenoDX Dishonored, Batman AK and Borderlands 2 (MIT). Preserve the
// Judgment LUT's encoded-domain addressing; only its linear input is bounded.
float3 NativeJudgmentCompressedLut(float3 linearInput) {
  linearInput=max(linearInput,0.0);
  float peak=max(linearInput.r,max(linearInput.g,linearInput.b));
  const float knee=.5;
  if(peak<=knee)return NativeJudgmentReadLut(linearInput);
  // Shifted Reinhard: identity below the knee, matching value/derivative at
  // the join, then approach LUT white without a per-channel hard clip.
  float distance=peak-knee;
  float bounded=knee+distance/(1.0+distance/(1.0-knee));
  float scale=bounded/peak;
  return NativeJudgmentReadLut(linearInput*scale)/scale;
}
float3 NativeJudgmentExtendedLut(float3 linearInput) {
  return $LUT_BRIDGE(linearInput);
}
float3 NativeJudgmentExtendedGrade(float3 exposed,float grain,out float neutralBrightness) {
  float3 neutral=max(NativeJudgmentExtendedNeutral(exposed),0.0);
  neutralBrightness=max(neutral.r,max(neutral.g,neutral.b));
  float3 encoded=pow(neutral,1.0/2.2);
  float3 grainBase=saturate(encoded);
  encoded=max(encoded+grain*((1.0-grainBase)*(1.0-grainBase)*$C3.w+$C251.x),0.0);
  return NativeJudgmentExtendedLut(pow(encoded,2.2));
}
float3 NativeJudgmentExtendedGrade(float3 exposed,float grain) {
  float neutralBrightness;
  return NativeJudgmentExtendedGrade(exposed,grain,neutralBrightness);
}
// The Gears 3 RenoDX Dishonored sample luminance boost, unchanged in scalar
// behavior: power .40, normalization .04. See RenoDX-Grading-LICENSE.txt (MIT).
float3 NativeJudgmentHdrBoost(float3 color,float strength) {
  color=max(color,0.0);float y=dot(color,float3(.2126,.7152,.0722));
  if(y<=0.0)return color;
  const float power=.40,normalization=.04;
  float weight=(2.0*power*y)/(y+2.0*power);
  float expanded=normalization*pow(y/normalization,1.0+power);
  float boosted=max(y,lerp(y,expanded,weight));
  return color*(1.0+(boosted/y-1.0)*max(strength,0.0));
}
)";
  const std::string lutToken="$LUT_BRIDGE";
  const auto lutAt=grade.find(lutToken);
  if(lutAt==std::string::npos)return {};
  grade.replace(lutAt,lutToken.size(),compressed_lut?"NativeJudgmentCompressedLut":"NativeJudgmentGradientLut");
  // The second captured permutation packs the same neutral/LUT parameters
  // differently, and folds exposure into c250.z before the first ceiling.
  if(alternate) {
    const auto parameter=[&](const std::string& from,const std::string& to) {
      size_t at=0;while((at=grade.find(from,at))!=std::string::npos) {
        grade.replace(at,from.size(),to);at+=to.size();
      }
    };
    parameter("abs(q*$C250.y)","abs(q*$C252.w)");
    parameter("q*$C251.y","q*$C251.w");
    parameter("+$C251.y*0.6931471805599453","+$C251.w*0.6931471805599453");
    parameter("))*$C250.w","))*$C251.x");
    parameter("encoded*$C250.z+$C252.z","encoded*$C250.y+$C251.y");
    parameter("*$C3.w+$C251.x","*$C3.w+$C252.y");
  }
  for(unsigned index:{0u,3u,250u,251u,252u}) {
    const auto token="$C"+std::to_string(index);size_t at=0;
    while((at=grade.find(token,at))!=std::string::npos) {
      const auto value=constant(index);grade.replace(at,token.size(),value);at+=value.size();
    }
  }
  if(!replace("XePsOut main(XePsIn xe_in) {",grade+R"(
XePsOut main(XePsIn xe_in) {
  float3 nativeScene=0.0;
  float3 nativeEncodedGrade=0.0;
  float nativeGrain=0.0;
)"))return {};
  const std::string capture=alternate ? "float4 s0 = r[1].xxzy;\n        float4 s1 = "+constant(250)+".zzzz;" :
      "float4 s0 = r[1].wwww;\n        float4 s1 = r[1].xxzy;";
  const std::string scene=alternate ? "r[1].xzy * "+constant(250)+".z" : "r[1].xzy * r[1].w";
  const std::string grain=alternate ? "r[3].x + "+constant(252)+".x" : "r[3].y";
  if(!replace(capture,"nativeScene = max("+scene+", 0.0);\n        nativeGrain = "+grain+";\n        "+capture))return {};
  const std::string reference="float4 s0 = r[1].xyzz;\n        float4 s1 = "+constant(0)+".xxxx;";
  if(!replace(reference,"nativeEncodedGrade = max(r[1].xyz, 0.0);\n        "+reference))return {};
  // Output 0 remains HDR for compatibility with the replay path. Output 1
  // retains the exact original encoded result for the paired single draw.
  if(!replace("  o.color1 = xe_oc[1] * xe_color_exp_scale.y;",
      "  o.color1 = o.color0;"))return {};
  const std::string begin="  // Linear-light composition in scene reference-white units.\n";
  const std::string end="  o.color0.rgb = nativeColor;";
  const auto start=source.find(begin),finish=source.find(end);
  if(start==std::string::npos || finish==std::string::npos || finish<start ||
     source.find(begin,start+begin.size())!=std::string::npos || source.find(end,finish+end.size())!=std::string::npos)return {};
  source.replace(start,finish+end.size()-start,
      "  // Judgment's extended curve / actual LUT, then the shared primary display mapper.\n"
      "  float nativePeak=max(asfloat(xe_sys2.x),1e-4);\n"
      "  float nativeHighlights=xe_sys2.z!=0u?asfloat(xe_sys2.z)-1.0:1.0;\n"
      "  float3 nativeVanillaGrade=pow(max(nativeEncodedGrade,0.0),2.2);\n"
      "  float nativeFade=pow(max("+constant(0)+".x*xe_color_exp_scale.x,0.0),2.2);\n"
      + std::string(optimized ? std::string("  [branch] if(all(nativeVanillaGrade==0.0) || nativeFade==0.0) "
                                "o.color0.rgb=0.0;\n")
      + std::string(calibrated ? "  else "
                                  "if(max(nativeScene.r,max(nativeScene.g,nativeScene.b))<=0.18)\n"
                                : "  else "
                                  "if(max(nativeScene.r,max(nativeScene.g,nativeScene.b))<=0.5)"
                                  "\n") +
                        "    "
                        "o.color0.rgb=NativeFitHdr(nativeVanillaGrade*max(nativeFade,1.0),"
                        "nativePeak)*min(nativeFade,1.0);\n"
                        "  else {\n" : "  {\n") +
      "    float nativeNeutralBrightness;\n"
          "    float3 "
          "nativeExtendedGrade=NativeJudgmentHdrBoost(NativeJudgmentExtendedGrade(nativeScene,"
          "nativeGrain,nativeNeutralBrightness),nativeHighlights);\n"
          // Judgment captures exposed lighting. Blend on its continued neutral
      // grade rather than applying Gears 3's raw scene thresholds to it.
      "    o.color0.rgb=NativeUe3MapExtendedGrade("+std::string(calibrated?"nativeNeutralBrightness.xxx":"nativeScene")+
          ",nativeExtendedGrade,nativeVanillaGrade,nativePeak,nativeFade,.75,max(12.0,nativePeak*2."
          "0));\n"
          "  }\n");
  // Match the original UNORM target's alpha precision before FP16 storage.
  // The later FXAA pass must use SDR luma, never recomputed HDR luminance.
  if(!replace("  return o;", "  o.color0.a=round(saturate(o.color0.a)*255.0)/255.0;\n  return o;"))return {};
  return std::string(kPsycho35Hlsl)+kUe3GradeHlsl+source;
}
} // namespace xenos::renderer::native_hdr
