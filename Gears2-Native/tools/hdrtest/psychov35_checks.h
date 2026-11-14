// GPU tests of the production source transforms with authored scene inputs.
// This file contains no game shader data. Included after the test utilities.
#include <array>
#include "ue3_grade_checks.h"
#include "tone_extension_checks.h"
#include "gears2_peak_checks.h"
static int Psycho35SceneCheck(gpu::Backend& backend, xenos::renderer::ShaderCompiler& compiler, bool gears3) {
  if(!gears3)if(const int result=Ue3GradeBridgeCheck(backend,compiler))return result;
  constexpr unsigned count = 36;
  const float energies[count][3] = {{-1,-.5f,-2}, {0,0,0}, {.18f,.18f,.18f}, {.01f,.01f,.01f},
      {1,1,1}, {8,8,8}, {64,64,64}, {65000,65000,65000}, {64,0,0}, {0,64,0}, {0,0,64}, {8,2,.25f},
      {64,64,64}, {0,0,0}, {0,0,0}, {2,0,0}, {0,2,0}, {0,0,2},
      {1.1f,1.1f,1.1f}, {1.5f,1.5f,1.5f}, {3,3,3}, {4,4,4},
      {1.7f,1,.25f}, {1.7f,1.7f,1.7f}, {.49f,.49f,.49f}, {.5f,.5f,.5f},
      {.51f,.51f,.51f},{.6f,.6f,.6f},{.75f,.75f,.75f},{.9f,.9f,.9f},{.99f,.99f,.99f},{1,1,1},
      {.18f,.18f,.18f},{8,2,.25f},{1.7f,1,.25f},{4,4,4}};
  for (float exposure : {.25f, 1.f, 1.4f, 4.f}) {
    std::string source = R"(
cbuffer Params : register(b0) { uint4 xe_sys2; float4 xe_color_exp_scale; };
Texture2D<float4> scene : register(t0);
Texture3D<float4> xe_tex_3d_3 : register(t1);
RWTexture2D<float4> output : register(u0);
float4 xe_tex_post(float4 value, uint slot) { return value; }
struct XePsIn { uint pixel; };
struct XePsOut { float4 color0; };
static const float4 xe_c[256] = {
)";
    for (unsigned i = 0; i < 256; ++i) {
      if (i == 0) source += "float4(1,0,0,0),";
      else if (i == 4) source += "float4(" + std::to_string(exposure) + ",1,0,1),";
      else if (i == 5) source += gears3 ? "float4(1,0,0,0)," : "float4(0,0,0,1),";
      else if (i == 7 && !gears3) source += "float4(" + std::to_string(exposure) + "," +
          std::to_string(exposure) + "," + std::to_string(exposure) + ",1),";
      else if (i == 9 && !gears3) source += "float4(.5,1,.25,0.45454545),";
      else if (i == 6 || i == 7) source += "float4(1,1,1,1),";
      else if (i == 252) source += "float4(.9375,0.45,0,0),";
      else if (i == 253) source += "float4(0,0,1,.03125),";
      else source += "float4(0,0,0,0),";
    }
    source += "};\nXePsOut main(XePsIn xe_in) {\n  float4 r[4];\n";
    if (gears3) {
      source += R"(
  r[3] = scene.Load(int3(xe_in.pixel,0,0)).rbga;
  r[2] = 0.0;
  {
    float4 s0 = r[3].xzyy;
        float4 s1 = xe_c[4].xxxx;
  }
  r[1] = float4(pow(max(NativeGears3Neutral(max(r[3].xzy,0.0)) * float3(.5,1,.25),0.0),1.0/2.2),1);
)";
    } else {
      source += R"(
  {
    float4 tv = scene.Load(int3(xe_in.pixel,0,0));
    r[3] = tv;
    r[3].w = tv.w;
  }
  r[0] = float4(1,scene.Load(int3(xe_in.pixel,0,0)).rgb);
  // Reproduce the empty blur kernel's infinite reciprocal. The existing
  // central sample must supply the same result as its valid control pixel.
  if(xe_in.pixel>=32) r[0].x=asfloat(0x7f800000u);
  {
    float4 s0 = r[0].yzww;
        float4 s1 = r[0].xxxx;
        float4 s2 = -(xe_c[5].xyzz);
    r[0].xyz = saturate(s0.xyz * s1.xyz + s2.xyz);
  }
  r[1] = float4(pow(max(r[0].xyz,0.0),xe_c[7].xyz),1);
  {
    float4 s0 = r[1].xxyz;
        float4 s1 = xe_c[5].wwww;
    r[0].xyz = pow(max(r[1].xyz * float3(.5,1,.25),0.0),1.0/2.2);
  }
)";
    }
    const std::string reg = gears3 ? "r[1]" : "r[0]";
    // Bright underlying lighting with a black grading result must stay black.
    // A grain/black offset with no scene energy stays bounded at its SDR level.
    source += "  if (xe_in.pixel == 12) " + reg + ".xyz = 0.0;\n";
    source += "  if (xe_in.pixel == 13) " + reg + ".xyz = pow(0.01,1.0/2.2);\n";
    source += "  {\n    float4 s0 = " + reg + ".xyzz;\n        float4 s1 = xe_c[0].xxxx;\n  }\n";
    source += R"(
  XePsOut o;
  o.color0 = float4(0,0,0,1);
  // Linear-light composition in scene reference-white units.
  float3 nativeColor = 0.0;
  o.color0.rgb = nativeColor;
  return o;
}
)";
    // Exercise fail-closed behavior as well as the production transformation.
    if (!hdr::Psycho35SceneSource("wrong interface",256,256,gears3).empty()) return 30;
    source = hdr::Psycho35SceneSource(source,256,256,gears3);
    if (source.empty()) { std::puts("FAIL PsychoV35 source transform"); return 31; }
    const auto main = source.find("XePsOut main(XePsIn xe_in)");
    source.replace(main, std::strlen("XePsOut main"), "XePsOut TestMaterial");
    source += R"(
[numthreads(8,1,1)] void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= 36) return;
  if (id.x == 14) {
    // A diffuse pixel keeps the vanilla grade instead of inheriting a darker
    // or differently colored reconstructed reference.
    output[id.xy] = float4(NativeUe3MapExtendedGrade(0.18.xxx,
        float3(.0001,.001,.002),float3(.3,.5,.1),asfloat(xe_sys2.x),
        pow(max(xe_color_exp_scale.x,0.0),2.2)),1);
    return;
  }
  XePsIn input; input.pixel = id.x;
  output[id.xy] = TestMaterial(input).color0;
}
)";
    if(!gears3) {
      const auto at=source.find("  if (id.x == 14)");
      source.insert(at,R"(
  if(xe_sys2.y==5) {
    float3 value=max(scene.Load(int3(id.xy,0)).rgb,0.0);
    float peak=asfloat(xe_sys2.x);
    output[id.xy]=float4(NativeUe3MapExtendedGrade(value,
        NativeGears2PeakInput(value,peak),saturate(value),peak,1.0,0.75,max(12.0,peak*2.0)),1);return;
  }
  if(xe_sys2.y==6) {
    float peak=asfloat(xe_sys2.x),h=0.001;
    float a=NativeGears2PeakInput((1.0-h).xxx,peak).x;
    float b=NativeGears2PeakInput(1.0.xxx,peak).x;
    float c=NativeGears2PeakInput((1.0+h).xxx,peak).x;
    output[id.xy]=float4((b-a)/h,(c-b)/h,b,1);return;
  }
  if(xe_sys2.y==7 || xe_sys2.y==8) {
    const float3 grades[6]={float3(.9,.5,.1),float3(.15,.4,.95),
        float3(1,.2,.01),float3(.65,.1,.8),float3(.5,.5,.5),0.0.xxx};
    const float levels[6]={.18,.5,1.5,3,8,64};
    float3 reference=grades[id.x%6];float level=levels[id.x/6];
    float3 value=reference*level;float peak=asfloat(xe_sys2.x);
    output[id.xy]=float4(NativeUe3MapExtendedGrade(value,
        NativeGears2PeakInput(value,peak),reference*min(level,1.0),peak,1.0,
        xe_sys2.y==7?0.75:0.0,max(12.0,peak*2.0)),1);return;
  }
  if(xe_sys2.y==9) {
    float3 value=max(scene.Load(int3(id.xy,0)).rgb,0.0);
    float3 expanded=NativeGears2PeakInput(value,asfloat(xe_sys2.x));
    output[id.xy]=float4(select(value>0.0,expanded/max(value,1e-6),1.0.xxx),1);return;
  }
  if(xe_sys2.y==10) {
    float3 value=max(scene.Load(int3(id.xy,0)).rgb,0.0);
    float y=dot(value,float3(.2126,.7152,.0722));
    float gain=y>0.0?NativeGears2RenoDxBoost(y,0.20)/y:1.0;
    output[id.xy]=float4(gain.xxx,1);return;
  }
)");
    }
    if(gears3) {
      const auto at=source.find("  if (id.x == 14)");
      source.insert(at,R"(
  if(xe_sys2.y==1) {
    output[id.xy]=float4(NativeGears3ExtendedLut(scene.Load(int3(id.xy,0)).rgb),1);return;
  }
  if(xe_sys2.y==2) {
    output[id.xy]=float4(NativeGears3ExtendedNeutral(max(scene.Load(int3(id.xy,0)).rgb,0.0))/65536.0,1);return;
  }
  if(xe_sys2.y==3) {
    float p=0.18,h=p*0.001;
    float a=NativeGears3ExtendedNeutral((p-h).xxx).x;
    float b=NativeGears3ExtendedNeutral(p.xxx).x;
    float c=NativeGears3ExtendedNeutral((p+h).xxx).x;
    output[id.xy]=float4((b-a)/h,(c-b)/h,b,1);return;
  }
  if(xe_sys2.y==4) {
    float a=NativeGears3ExtendedLut(float3(.999,.2,.2)).x;
    float b=NativeGears3ExtendedLut(float3(1,.2,.2)).x;
    float c=NativeGears3ExtendedLut(float3(1.001,.2,.2)).x;
    output[id.xy]=float4(a,b,c,1);return;
  }
  if(xe_sys2.y==5) {
    float3 value=max(scene.Load(int3(id.xy,0)).rgb,0.0);
    float3 expanded=NativeGears3HdrBoost(value,0.20);
    output[id.xy]=float4(select(value>0.0,expanded/max(value,1e-6),1.0.xxx),1);return;
  }
  if(xe_sys2.y==6 || xe_sys2.y==7) {
    float3 value=max(scene.Load(int3(id.xy,0)).rgb,0.0);
    float3 graded=NativeGears3ExtendedGrade(value,0.0);
    if(xe_sys2.y==7) graded=NativeGears3HdrBoost(graded);
    output[id.xy]=float4(NativeUe3MapExtendedGrade(value,graded,
        NativeGears3Neutral(value)*float3(.5,1,.25),asfloat(xe_sys2.x),1.0,0.75,
        max(12.0,asfloat(xe_sys2.x)*2.0)),1);return;
  }
  if(xe_sys2.y==10) {
    float3 value=max(scene.Load(int3(id.xy,0)).rgb,0.0);
    float peak=asfloat(xe_sys2.x);
    output[id.xy]=float4(NativeUe3MapExtendedGrade(value,NativeGears3HdrBoost(value),
        saturate(value),peak,1.0,0.75,max(12.0,peak*2.0)),1);return;
  }
)");
    }
    std::vector<uint8_t> bytes; std::string error;
    if (!compiler.Compile(source,gpu::ShaderStage::kCompute,backend.GetShaderIL(),bytes,error)) {
      std::printf("FAIL PsychoV35 compile: %s\n",error.c_str()); return 32;
    }
    const auto shader = backend.CreateShader(gpu::ShaderStage::kCompute,bytes.data(),bytes.size());
    const auto pipeline = shader ? backend.CreateComputePipeline(shader) : 0;
    gpu::TextureDesc desc; desc.width=count; desc.height=1; desc.format=gpu::Format::kR16G16B16A16Float;
    desc.usage=gpu::kUsageSampled|gpu::kUsageStorage;
    const auto input=backend.CreateTexture(desc), output=backend.CreateTexture(desc);
    desc.dimension=gpu::TextureDimension::k3D;desc.width=desc.height=desc.depth_or_layers=16;desc.usage=gpu::kUsageSampled;
    const auto lut=backend.CreateTexture(desc);
    if (!pipeline || !input || !output || !lut) return 33;
    std::vector<uint16_t> lutData(16*16*16*4);
    const float tint[3]={.5f,1.f,.25f};
    for(unsigned z=0;z<16;++z)for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      const unsigned xyz[]={x,y,z};
      for(unsigned c=0;c<3;++c)lutData[4*(x+16*(y+16*z))+c]=ToHalf(float(xyz[c])/15.f*std::pow(tint[c],1.f/2.2f));
    }
    backend.UploadTexture(lut,0,0,lutData.data(),16*8,16,16);
    uint16_t data[count*4]{};
    for (unsigned i=0;i<count;++i) for(unsigned c=0;c<3;++c) data[4*i+c]=ToHalf(energies[i][c]);
    backend.UploadTexture(input,0,0,data,count*8,1,1);
    if(gears3)if(const int result=ToneExtensionCheck(backend,pipeline,input,lut,output,
        count,exposure,data,lutData))return result;
    if(gears3)if(const int result=Gears3BoostCheck(backend,pipeline,input,lut,output,count,data))return result;
    if(!gears3 && exposure==1.f)if(const int result=Gears2PeakCheck(backend,pipeline,input,output,count,data))return result;
    if(!gears3 && exposure==1.f)if(const int result=Gears2ColorCheck(backend,pipeline,input,output,count))return result;
    for (float peak : {1.f,4.f,19.f,80.f}) {
      float fullBrightness[count*4]{};
      for (float fade : {1.f,.5f,.001f,0.f}) {
        gpu::ComputeBindings b; b.constant_count=8;
        b.constants[0]=std::bit_cast<uint32_t>(peak);
        b.constants[4]=std::bit_cast<uint32_t>(std::pow(fade,1.f/2.2f));
        b.srv[0]=input;b.srv[1]=lut; b.uav_texture=output;
        backend.Dispatch(pipeline,b,(count+7)/8,1,1);
        const auto rb=backend.ReadbackTexture(output,{0,0,count,1}); backend.WaitForFence(backend.Submit());
        uint32_t pitch=0; const auto* result=backend.ReadbackData(rb,pitch); uint16_t actual[count*4]{};
        if(result) std::memcpy(actual,result,sizeof(actual)); backend.ReleaseReadback(rb); if(!result) return 34;
        for(unsigned i=0;i<count;++i) for(unsigned c=0;c<3;++c) {
          const float value=Half(actual[4*i+c]);
          if(!std::isfinite(value) || value<0 || value>peak+.04f || ((i<2 || i==12) && value!=0) ||
              (i==13 && std::abs(value-.01f*fade)>.0001f)) {
            std::printf("FAIL PsychoV35 range game %d pixel %u channel %u: %g (peak %g)\n",gears3?3:2,i,c,value,peak); return 35;
          }
          const float gradeReference[]={.3f,.5f,.1f};
          if(!gears3 && i>=32) {
            const unsigned control[]={2,11,22,21};
            const float expected=Half(actual[4*control[i-32]+c]);
            if(std::abs(value-expected)>std::max(.003f,expected/512.f)) {
              std::printf("FAIL Gears 2 empty blur central fallback pixel %u: %g != %g\n",i,value,expected);return 49;
            }
          }
          if(i==14 && std::abs(value-gradeReference[c]*fade)>.001f) {
            std::printf("FAIL SDR grade dark-hole protection channel %u: %g\n",c,value); return 42;
          }
          if(fade==1.f) fullBrightness[4*i+c]=value;
          else if(std::abs(value-fullBrightness[4*i+c]*fade)>std::max(.003f,peak*.001f) || (fade==0 && value!=0)) {
            std::printf("FAIL PsychoV35 fade %g pixel %u: %g != %g\n",fade,i,value,fullBrightness[4*i+c]*fade); return 36;
          }
        }
        if(fade==1.f) {
          for(unsigned i=2;i<count;++i) {
            if(i>=12 && i<=14)continue;
            const float tint[]={.5f,1.f,.25f}; float reference[3]{};
            for(unsigned c=0;c<3;++c) {
              const float x=std::max(Half(data[4*i+c]),0.f);
              reference[c]=tint[c]*(gears3?std::pow(x/(x+exposure),2.2f):std::pow(std::min(x,1.f),exposure));
            }
            const float referenceMax=std::max({reference[0],reference[1],reference[2]});
            const float actualMax=std::max({Half(actual[4*i]),Half(actual[4*i+1]),Half(actual[4*i+2])});
            // Diffuse/shadow values remain exact SDR. HDR preserves the
            // reference's hue while allowing a smoother saturation roll-off.
            const float sourceMax=std::max({energies[i][0],energies[i][1],energies[i][2]});
            if(sourceMax<=.5f && referenceMax>1e-5f && actualMax>1e-5f)for(unsigned c=0;c<3;++c) {
              if(std::abs(Half(actual[4*i+c])/actualMax-reference[c]/referenceMax)>.004f) {
                std::printf("FAIL SDR grading chromaticity pixel %u channel %u\n",i,c); return 43;
              }
            }
            auto lab=[](float r,float g,float b) {
              const float l=std::cbrt(.4122214708f*r+.5363325363f*g+.0514459929f*b);
              const float m=std::cbrt(.2119034982f*r+.6806995451f*g+.1073969566f*b);
              const float s=std::cbrt(.0883024619f*r+.2817188376f*g+.6299787005f*b);
              return std::array<float,3>{.2104542553f*l+.7936177850f*m-.0040720468f*s,
                1.9779984951f*l-2.4285922050f*m+.4505937099f*s,.0259040371f*l+.7827717662f*m-.8086757660f*s};
            };
            const auto expectedLab=lab(reference[0],reference[1],reference[2]);
            const auto actualLab=lab(Half(actual[4*i]),Half(actual[4*i+1]),Half(actual[4*i+2]));
            const float ec=std::hypot(expectedLab[1],expectedLab[2]),ac=std::hypot(actualLab[1],actualLab[2]);
            if(ec>expectedLab[0]*.01f && ac>actualLab[0]*.01f) {
              const float cosine=(expectedLab[1]*actualLab[1]+expectedLab[2]*actualLab[2])/(ec*ac);
              if(cosine<.999f) {std::printf("FAIL UE3 vanilla hue pixel %u cosine %g\n",i,cosine);return 48;}
              if(gears3 && sourceMax>=1.f && ac/actualLab[0]<.65f*(ec/expectedLab[0])) {
                std::puts("FAIL Gears 3 boosted highlight lost SDR relative chroma");return 97;
              }
            }
          }
          // Include the captured contrast 1.4 and extreme contrast 4: real
          // over-white emitters must retain HDR in each primary color.
          if(!gears3 && peak>=19.f)for(unsigned i=8;i<11;++i) {
            const float emitterPeak=std::max({Half(actual[4*i]),Half(actual[4*i+1]),Half(actual[4*i+2])});
            if(emitterPeak<=1.01f) {
              std::printf("FAIL Gears 2 colored light remains SDR pixel %u: %g\n",i,emitterPeak);return 45;
            }
          }
          const float shadowInput=Half(data[3*4]);
          const float shadowReference=gears3?std::pow(shadowInput/(shadowInput+exposure),2.2f):std::pow(shadowInput,exposure);
          if(std::abs(Half(actual[3*4+1])-shadowReference)>.001f) return 44;
          for(unsigned i=2;i<4;++i) {
            const float green=Half(actual[4*i+1]);
            if(std::abs(Half(actual[4*i])-green*.5f)>std::max(.000001f,green*.003f) ||
               std::abs(Half(actual[4*i+2])-green*.25f)>std::max(.000001f,green*.003f)) {
              std::printf("FAIL PsychoV35 grading pixel %u: %g %g %g\n",i,
                  Half(actual[4*i]),green,Half(actual[4*i+2])); return 37;
            }
          }
          if(peak>1 && Half(actual[7*4+1])<peak*.95f) return 38;
          for(unsigned i=25;i<32;++i) {
            const float previous=Half(actual[4*(i-1)+1]),current=Half(actual[4*i+1]);
            if(current+std::max(.001f,previous/512.f)<previous) {
              std::printf("FAIL UE3 diffuse/HDR transition pixel %u: %g -> %g\n",i,previous,current);return 47;
            }
          }
          if(!gears3 && peak>=19.f) {
            // Modest highlights must retain separation instead of jumping
            // straight to peak; much brighter lighting can still reach it.
            const float a=Half(actual[18*4+1]), b=Half(actual[19*4+1]);
            const float c=Half(actual[20*4+1]), d=Half(actual[21*4+1]);
            // At the finite white, backend math can differ by one FP16 ULP.
            if(!(a<b && b<c && c<=d+peak/1024.f) || b>=peak*.8f) {
              std::printf("FAIL Gears 2 highlight roll-off/detail: %g %g %g %g of %g\n",a,b,c,d,peak);return 46;
            }
          }
          // Strong 8-white lighting can now reach peak by design. Preserve
          // headroom for a genuinely modest 1.5-white light instead.
          if(gears3 && peak>=19 && Half(actual[19*4+1])>=peak*.8f) {
            std::printf("FAIL UE3 modest lighting forced to display peak: %g of %g\n",Half(actual[19*4+1]),peak); return 41;
          }
          const float neutralMid=gears3?std::pow(.18f/(.18f+exposure),2.2f):std::pow(.18f,exposure);
          if(std::abs(Half(actual[2*4+1])-neutralMid)>.002f) {
            std::printf("FAIL PsychoV35 mid-gray: %g != %g\n",Half(actual[2*4+1]),neutralMid); return 39;
          }
        }
      }
    }
    backend.DestroyTexture(input); backend.DestroyTexture(output);backend.DestroyTexture(lut);
    std::printf("PASS Gears %d UE3/PsychoV35 curve %.2f: SDR diffuse/shadows, highlight separation, HDR headroom without forced early peak, saturated/negative/FP16 inputs, peak fit, fades, black grading, grain\n",gears3?3:2,exposure);
  }
  return 0;
}
