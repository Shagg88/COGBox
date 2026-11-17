// Authored Judgment bridge/FXAA properties; no game shader data. MIT.
#pragma once
#include <xenos/renderer/judgment_hdr_bridge.h>
#include <xenos/renderer/hdr_scene_effect.h>
static int JudgmentCheck(gpu::Backend& backend,xenos::renderer::ShaderCompiler& compiler,bool alternate=false,bool capturedCurve=false) {
  constexpr unsigned count=24;
  const float levels[count]={0,.01f,.18f,.179f,.181f,.5f,1,2,4,8,16,64,65000,2,4,8,.2f,.25f,.3f,.35f,.4f,.6f,.8f,1.1f};
  std::string source=R"(
cbuffer Params:register(b0) {uint4 xe_sys2;float4 xe_color_exp_scale;};
Texture2D<float4> scene:register(t0);
Texture3D<float4> xe_tex_3d_4:register(t1);
RWTexture2D<float4> output:register(u0);
float4 xe_tex_post(float4 v,uint slot){return v;}
struct XePsIn{uint pixel;};struct XePsOut{float4 color0;float4 color1;};
static const float4 xe_c[256]={
)";
  for(unsigned i=0;i<256;++i) {
    if(i==0)source+="float4(1,0,0,0),";
    else if(i==250 && capturedCurve)source+=alternate?"float4(.564518631,.9375,1,0),":"float4(.564518631,1.70000005,.9375,1.44580638),";
    else if(i==251 && capturedCurve)source+=alternate?"float4(1.44580638,.03125,0,-.98255384),":"float4(0,-.98255384,1,0),";
    else if(i==252 && capturedCurve)source+=alternate?"float4(0,0,0,1.70000005),":"float4(0,0,.03125,0),";
    else if(i==250 && alternate)source+="float4(0.4545454545,0.9375,1,0),";
    else if(i==251 && alternate)source+="float4(1,0.03125,0,0),";
    else if(i==252 && alternate)source+="float4(0,0,0,1),";
    else if(i==250)source+="float4(0.4545454545,1,0.9375,1),";
    else if(i==251)source+="float4(0,0,1,0),";
    else if(i==252)source+="float4(0,0,0.03125,0),";
    else source+="float4(0,0,0,0),";
  }
  source+=R"(};
XePsOut main(XePsIn xe_in) {
  float3 value=scene.Load(int3(xe_in.pixel,0,0)).rgb;
  float4 r[4];r[1]=float4(value.rbg,1);r[3]=0.0;
  {
    float4 s0 = r[1].wwww;
        float4 s1 = r[1].xxzy;
  }
  r[1].xyz=pow(select(value<=0.0,0.0,NativeJudgmentNeutral(value))*float3(.5,1,.25),1.0/2.2);
  {
    float4 s0 = r[1].xyzz;
        float4 s1 = xe_c[0].xxxx;
  }
  XePsOut o;o.color0=float4(pow(select(value<=0.0,0.0,NativeJudgmentNeutral(value))*float3(.5,1,.25),1.0/2.2)*xe_color_exp_scale.x,.375);
  float4 xe_oc[2];xe_oc[1]=0;
  o.color1 = xe_oc[1] * xe_color_exp_scale.y;
  // Linear-light composition in scene reference-white units.
  float3 nativeColor=0;
  o.color0.rgb = nativeColor;
  return o;
}
)";
  if(!hdr::JudgmentSceneSource("wrong interface",256,256).empty())return 110;
  const auto duplicate=source+"\nXePsOut main(XePsIn xe_in) {";
  if(!hdr::JudgmentSceneSource(duplicate,256,256).empty())return 110;
  if(alternate) {
    const std::string capture="float4 s0 = r[1].wwww;\n        float4 s1 = r[1].xxzy;";
    const auto marker=source.find(capture);if(marker==std::string::npos)return 110;
    source.replace(marker,capture.size(),"float4 s0 = r[1].xxzy;\n        float4 s1 = xe_c[250].zzzz;");
  }
  source=hdr::JudgmentSceneSource(source,256,256,alternate);
  if(source.empty()||source.find("$C")!=std::string::npos)return 111;
  const auto at=source.find("XePsOut main(");source.replace(at,strlen("XePsOut main"),"XePsOut TestMaterial");
  source+=R"(
[numthreads(8,1,1)]void main(uint3 p:SV_DispatchThreadID){
  if(p.x>=24)return;XePsIn i;i.pixel=p.x;
  float3 v=scene.Load(int3(p.xy,0)).rgb;
  if(xe_sys2.y==1)output[p.xy]=float4(NativeJudgmentExtendedNeutral(v),1);
  else if(xe_sys2.y==2)output[p.xy]=float4(NativeJudgmentExtendedLut(v),1);
  else if(xe_sys2.y==6)output[p.xy]=float4(NativeJudgmentCompressedLut(v),1);
  else if(xe_sys2.y==7)output[p.xy]=float4(NativeJudgmentGradientLut(v).g,NativeJudgmentCompressedLut(v).g,0,1);
  else if(xe_sys2.y==8)output[p.xy]=float4(NativeJudgmentFiniteNeutral(v).g,NativeJudgmentExtendedNeutral(v).g,0,1);
  else if(xe_sys2.y==4) {
    float3 originalGrade=NativeJudgmentNeutral(v)*float3(.5,1,.25);
    float3 extended=NativeJudgmentHdrBoost(NativeJudgmentExtendedGrade(v,0.0),1.0);
    output[p.xy]=float4(NativeUe3MapExtendedGrade(v,extended,originalGrade,asfloat(xe_sys2.x),1.0,.75,max(12.0,asfloat(xe_sys2.x)*2.0)),1);
  }
  else if(xe_sys2.y==3)output[p.xy]=TestMaterial(i).color1;
  else output[p.xy]=TestMaterial(i).color0;
}
)";
  std::vector<uint8_t> bytes;std::string error;
  if(!compiler.Compile(source,gpu::ShaderStage::kCompute,backend.GetShaderIL(),bytes,error)) {
    std::printf("FAIL Judgment bridge compile: %s\n",error.c_str());return 112;
  }
  auto cs=backend.CreateShader(gpu::ShaderStage::kCompute,bytes.data(),bytes.size());
  const auto pipeline=backend.CreateComputePipeline(cs);
  gpu::TextureDesc d;d.width=count;d.height=1;d.format=gpu::Format::kR32G32B32A32Float;
  d.usage=gpu::kUsageStorage|gpu::kUsageSampled;
  const auto input=backend.CreateTexture(d),output=backend.CreateTexture(d);
  d.dimension=gpu::TextureDimension::k3D;d.width=d.height=d.depth_or_layers=16;d.usage=gpu::kUsageSampled;
  const auto lut=backend.CreateTexture(d);if(!pipeline||!input||!output||!lut)return 113;
  float data[count*4]{};for(unsigned i=0;i<count;++i)for(unsigned c=0;c<3;++c)data[4*i+c]=levels[i];
  data[13*4+1]=data[13*4+2]=0;data[14*4]=data[14*4+2]=0;data[15*4]=data[15*4+1]=0;
  backend.UploadTexture(input,0,0,data,count*16,1,1);
  std::vector<float> lutData(16*16*16*4);
  const float tint[]={.5f,1.f,.25f};
  for(unsigned z=0;z<16;++z)for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){
    const unsigned xyz[]={x,y,z};for(unsigned c=0;c<3;++c)
      lutData[4*(x+16*(y+16*z))+c]=float(xyz[c])/15.f*std::pow(tint[c],1.f/2.2f);
  }
  backend.UploadTexture(lut,0,0,lutData.data(),16*16,16,16);
  const auto neutral=[&](float x) {
    x=std::clamp(x,0.f,1.f);
    if(!capturedCurve)return x;
    return std::pow(std::clamp(std::exp2(x*-.98255384f)*1.44580638f*std::pow(x*1.70000005f,.564518631f),0.f,1.f),2.2f);
  };
  const float pivot=.18f,dh=pivot/1024.f;
  const float slope=std::max((neutral(pivot+dh)-neutral(pivot-dh))/(2*dh),0.f);
  float calibratedValues[count*4]{};
  for(unsigned mode=0;mode<(capturedCurve?5u:4u);++mode)for(float peak:{1.f,4.f,19.f,20.f,80.f}) {
    float full[count*4]{};
    for(float fade:{1.f,.5f,0.f}) {
      gpu::ComputeBindings b;b.constants[0]=std::bit_cast<uint32_t>(peak);b.constants[1]=mode;
      b.constants[2]=std::bit_cast<uint32_t>(2.f);b.constants[4]=std::bit_cast<uint32_t>(std::pow(fade,1.f/2.2f));
      b.constant_count=8;b.srv[0]=input;b.srv[1]=lut;b.uav_texture=output;
      backend.Dispatch(pipeline,b,(count+7)/8,1,1);
      const auto rb=backend.ReadbackTexture(output,{0,0,count,1});backend.WaitForFence(backend.Submit());
      uint32_t pitch=0;const auto* raw=backend.ReadbackData(rb,pitch);float actual[count*4]{};
      if(raw)std::memcpy(actual,raw,sizeof(actual));backend.ReleaseReadback(rb);if(!raw)return 114;
      for(unsigned i=0;i<count;++i)for(unsigned c=0;c<3;++c) {
        const float v=actual[4*i+c];
        if(mode==3) {
          const float expected=std::pow(neutral(data[4*i+c])*tint[c],1.f/2.2f)*std::pow(fade,1.f/2.2f);
          if(!std::isfinite(v)||std::abs(v-expected)>.00002f || std::abs(actual[4*i+3]-.375f)>1e-6f)return 130;
          continue;
        }
        if(!std::isfinite(v)||v<0 || (mode==0 && v>peak+.002f))return 115;
        if(mode==0) {
          if(std::abs(actual[4*i+3]-(96.f/255.f))>1e-6f)return 116;
          if(i==0 && v!=0)return 117;
          if(fade==1)full[4*i+c]=v;
          else if(std::abs(v-full[4*i+c]*fade)>.0002f*peak)return 118;
          if(fade==1 && (i==1||i==2) && std::abs(v-neutral(data[4*i+c])*tint[c])>.001f)return 119;
        } else if(mode==1) {
          const float expected=neutral(std::min(data[4*i+c],pivot))+slope*std::max(data[4*i+c]-pivot,0.f);
          if(std::abs(v-expected)>std::max(.0002f,expected*.003f))return 120;
        }
        else if(mode==2 && i>=1 && i<=12 && c==1 && v<actual[4*(i-1)+c] && i!=3)return 121;
      }
      if(capturedCurve && peak==19.f && fade==1.f) {
        if(mode==0)std::copy_n(actual,count*4,calibratedValues);
        if(mode==4) {
          for(unsigned i:{19u,20u}) {
            const float before=actual[i*4+1],after=calibratedValues[i*4+1];
            std::printf(
                  "Judgment grade calibration permutation %u exposed %.2f: green %.3f -> %.3f "
                  "reference whites\n",alternate,levels[i],before,after);
            if(after<before*1.02f)return 131;
          }
        }
      }
      if(mode==0 && fade==1 && peak>1 && actual[11*4+1]<=1)return 122;
      if(mode==0 && fade==1 && peak==20.f) {
        const float strongest=std::max({actual[12*4],actual[12*4+1],actual[12*4+2]});
        if(strongest<19.8f)return 129;
        std::printf(
              "PASS Judgment permutation %u: 4000-nit configured peak, strongest %.1f nits at "
              "200-nit scene white\n",alternate,strongest*200.f);
      }
    }
  }
  auto inspect=[&](unsigned mode,float* actual) {
    gpu::ComputeBindings b;b.constants[0]=std::bit_cast<uint32_t>(19.f);b.constants[1]=mode;b.constants[4]=std::bit_cast<uint32_t>(1.f);
    b.constant_count=8;b.srv[0]=input;b.srv[1]=lut;b.uav_texture=output;
    backend.Dispatch(pipeline,b,(count+7)/8,1,1);const auto rb=backend.ReadbackTexture(output,{0,0,count,1});backend.WaitForFence(backend.Submit());
    uint32_t pitch=0;const auto* raw=backend.ReadbackData(rb,pitch);if(raw)std::memcpy(actual,raw,count*16);backend.ReleaseReadback(rb);return raw!=nullptr;
  };
  float bridgeValues[count*4]{};
  if(!inspect(6,bridgeValues))return 132;
  for(unsigned i=0;i<count;++i)for(unsigned c=0;c<3;++c) {
    const float expected=data[i*4+c]*tint[c];
    if(!std::isfinite(bridgeValues[i*4+c])||std::abs(bridgeValues[i*4+c]-expected)>std::max(.001f,expected*.003f))return 133;
  }
  if(!inspect(8,bridgeValues))return 132;
  for(unsigned i=0;i<count;++i)if(std::abs(bridgeValues[i*4]-bridgeValues[i*4+1])>std::max(.0002f,std::abs(bridgeValues[i*4])*.003f))return 134;
  // A broad clipped LUT edge defeats local boundary-gradient extrapolation.
  // Compress/sample/restore must still retain the upstream lighting's range.
  for(unsigned z=0;z<16;++z)for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const unsigned xyz[]={x,y,z};for(unsigned c=0;c<3;++c)
      lutData[4*(x+16*(y+16*z))+c]=std::min(float(xyz[c])/15.f,.2f)*std::pow(tint[c],1.f/2.2f);
  }
  backend.UploadTexture(lut,0,0,lutData.data(),16*16,16,16);
  if(!inspect(7,bridgeValues))return 132;
  const float plateau=bridgeValues[11*4],restored=bridgeValues[11*4+1];
  if(!std::isfinite(restored)||restored<plateau*10.f)return 135;
  std::fill(lutData.begin(),lutData.end(),0.f);backend.UploadTexture(lut,0,0,lutData.data(),16*16,16,16);
  if(!inspect(6,bridgeValues))return 132;
  for(unsigned i=0;i<count;++i)for(unsigned c=0;c<3;++c)if(bridgeValues[i*4+c]!=0.f)return 136;
  std::printf(
      "PASS RenoDX-inspired Judgment bridge: identity/tinted LUT extends to 65000; flat-edge grade "
      "%.5f -> %.5f at input 64; black LUT stays black; analytic slope matches finite reference\n",plateau,restored);
  // The encode pass restores SDR alpha precision without clamping HDR RGB.
  bytes.clear();
  if(!compiler.Compile(hdr::kEncodeSceneForFxaa,gpu::ShaderStage::kCompute,backend.GetShaderIL(),bytes,error))return 123;
  cs=backend.CreateShader(gpu::ShaderStage::kCompute,bytes.data(),bytes.size());
  const auto encode=backend.CreateComputePipeline(cs);if(!encode)return 124;
  for(unsigned i=0;i<count;++i)data[4*i+3]=Half(ToHalf(float((i%16)*17)/255.f));
  backend.UploadTexture(input,0,0,data,count*16,1,1);
  gpu::ComputeBindings b;b.srv[0]=input;b.uav_texture=output;
  backend.Dispatch(encode,b,(count+7)/8,1,1);
  const auto rb=backend.ReadbackTexture(output,{0,0,count,1});backend.WaitForFence(backend.Submit());
  uint32_t pitch=0;const auto* raw=backend.ReadbackData(rb,pitch);float actual[count*4]{};
  if(raw)std::memcpy(actual,raw,sizeof(actual));backend.ReleaseReadback(rb);if(!raw)return 126;
  for(unsigned i=0;i<count;++i) {
    if(std::abs(actual[4*i+3]-float((i%16)*17)/255.f)>1e-6f)return 127;
    for(unsigned c=0;c<3;++c)if(std::abs(actual[4*i+c]-std::pow(data[4*i+c],1.f/2.2f))>
        std::max(.0001f,actual[4*i+c]*.0001f))return 128;
  }
  backend.DestroyTexture(input);backend.DestroyTexture(output);backend.DestroyTexture(lut);
  std::printf(
      "PASS Judgment permutation %u: pre-clamp RGB mapping, original diffuse grade, extended "
      "LUT/highlights, finite 65000-range lighting, peak limits, alpha luma, linear fades, "
      "fail-closed source guards\n",alternate);
  return 0;
}
