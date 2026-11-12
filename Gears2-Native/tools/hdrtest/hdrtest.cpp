// GPU range checks for the real AA wrappers and native HDR encoder; no game data.
#include <xenos/renderer/gpu.h>
#include <xenos/renderer/shader_cache.h>
#include <xenos/renderer/upscaler.h>
#include <xenos/renderer/native_hdr.h>
#include <rex/cvar.h>
#include "../../src/layer0_xenos/renderer/native_hdr_shaders.h"
#include "../../src/layer0_xenos/renderer/builtin_shaders.h"
#include "../../src/layer0_xenos/renderer/smaa/smaa_passes.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace gpu = xenos::renderer::gpu;
namespace up = xenos::renderer::upscale;
namespace hdr = xenos::renderer::native_hdr;
namespace smaa = xenos::renderer::smaa;

static float Half(uint16_t h) {
  const unsigned e=(h>>10)&31, m=h&1023;
  return (h&32768?-1.0f:1.0f)*(e?std::ldexp(1.0f+float(m)/1024.0f,int(e)-15):std::ldexp(float(m),-24));
}
static double Pq(double nits) {
  const double p=std::pow(std::clamp(nits/10000.0,0.0,1.0),0.1593017578125);
  return std::pow((0.8359375+18.8515625*p)/(1.0+18.6875*p),78.84375);
}
static uint16_t ToHalf(float value) {
  const auto bits=std::bit_cast<uint32_t>(value);
  const auto exponent=int((bits>>23)&255)-127+15;
  if(exponent<=0)return uint16_t((bits>>16)&0x8000);
  return uint16_t(((bits>>16)&0x8000)|((unsigned(exponent)<<10)+(((bits&0x7fffff)+0x1000)>>13)));
}
static int UiCompositeCheck(gpu::Backend& backend,xenos::renderer::ShaderCompiler& compiler) {
  auto compute=[&](const char* source) {
    std::vector<uint8_t> bytes;std::string error;
    if(!compiler.Compile(source,gpu::ShaderStage::kCompute,backend.GetShaderIL(),bytes,error)) {
      std::printf("FAIL UI shader: %s\n",error.c_str());return gpu::PipelineHandle(0);
    }
    const auto s=backend.CreateShader(gpu::ShaderStage::kCompute,bytes.data(),bytes.size());
    return s?backend.CreateComputePipeline(s):0;
  };
  constexpr unsigned count=6;
  gpu::TextureDesc d;d.width=count;d.height=1;d.format=gpu::Format::kR16G16B16A16Float;
  d.usage=gpu::kUsageSampled|gpu::kUsageStorage|gpu::kUsageRenderTarget;
  const auto high=backend.CreateTexture(d), low=backend.CreateTexture(d), output=backend.CreateTexture(d);
  d.format=gpu::Format::kR8G8B8A8Unorm;
  const auto baseline=backend.CreateTexture(d);
  const auto mix=compute(hdr::kSceneUiBlendCs);
  if(!high||!low||!baseline||!output||!mix)return 20;
  // No UI, opaque overbright UI, world + additive UI, original tint,
  // half-coverage UI, and negative values before gamma.
  const float hLinear[count][3]={{4,2,1},{8,4,2},{5,2.5f,1.25f},{.25f,.5f,.125f},{4,2,1},{0,0,0}};
  const float sLinear[count][3]={{1,.5f,.25f},{8,4,2},{2,1.5f,1.25f},{.25f,.5f,.125f},{1,.5f,.25f},{0,0,0}};
  uint16_t h[count*4]{},s[count*4]{};uint8_t base[count*4]{};float expected[count*4]{};
  for(unsigned i=0;i<count;++i) {
    for(unsigned c=0;c<3;++c) {
      float he=std::pow(hLinear[i][c],1.f/2.2f),se=std::pow(sLinear[i][c],1.f/2.2f);
      if(i==4) {const float ui[3]={2,.5f,.25f};he=(he+ui[c])*.5f;se=(se+ui[c])*.5f;}
      if(i==5)he=se=-8;
      h[i*4+c]=ToHalf(he);s[i*4+c]=ToHalf(se);
      base[i*4+c]=uint8_t(std::round(std::clamp(se,0.f,1.f)*255.f));
      expected[i*4+c]=std::clamp(std::pow(float(base[i*4+c])/255.f,2.2f)+
          std::pow(std::max(Half(h[i*4+c]),0.f),2.2f)-std::pow(std::max(Half(s[i*4+c]),0.f),2.2f),0.f,20.f);
    }
    h[i*4+3]=s[i*4+3]=0x3c00;base[i*4+3]=255;expected[i*4+3]=1;
  }
  backend.UploadTexture(high,0,0,h,count*8,1,1);backend.UploadTexture(low,0,0,s,count*8,1,1);
  backend.UploadTexture(baseline,0,0,base,count*4,1,1);
  gpu::ComputeBindings b;b.constants[0]=count;b.constants[1]=1;b.constants[4]=std::bit_cast<uint32_t>(20.f);
  b.constant_count=8;b.srv[0]=high;b.srv[1]=low;b.srv[2]=baseline;b.uav_texture=output;
  backend.Dispatch(mix,b,1,1,1);
  const auto rb=backend.ReadbackTexture(output,{0,0,count,1});backend.WaitForFence(backend.Submit());uint32_t pitch=0;
  const auto* bytes=backend.ReadbackData(rb,pitch);uint16_t actual[count*4]{};
  if(bytes)std::memcpy(actual,bytes,sizeof(actual));backend.ReleaseReadback(rb);if(!bytes)return 22;
  for(unsigned i=0;i<count*4;++i) if(!std::isfinite(Half(actual[i]))||std::abs(Half(actual[i])-expected[i])>.006f) {
    std::printf("FAIL UI component %u: %g != %g\n",i,Half(actual[i]),expected[i]);return 23;
  }
  if(Half(actual[0])<3.9f||Half(actual[4])>1.001f||Half(actual[8])<3.9f)return 24;
  std::puts("PASS UI isolation: world HDR retained, overbright UI bounded, vanilla tint, partial coverage, negative-safe gamma");
  return 0;
}
// Exercise the same source transformation as the captured Gears 3 material,
// using authored inputs so this test contains no game shader data.
static int Gears3SceneCheck(gpu::Backend& backend,xenos::renderer::ShaderCompiler& compiler) {
  for (float exposure : {0.25f, 1.0f, 4.0f}) {
    const hdr::Gears3Curve curve{exposure,1,0,1,0.45f,1,1};
    const auto [pivot,slope]=curve.Extension();
    if(!std::isfinite(pivot)||!std::isfinite(slope)||pivot<=0||slope<=0)return 30;
    hdr::Gears3CurveCache curveCache;
    if(curveCache.Get(curve)!=curve.Extension() || curveCache.Get(curve)!=curve.Extension())return 40;
    // Exposure/curve edits must invalidate the cached tangent immediately.
    for(unsigned component=0;component<7;++component) {
      auto changed=curve;
      float* fields[]={&changed.offset,&changed.scale,&changed.shadow_start,&changed.shadow_scale,
                       &changed.power,&changed.weight_scale,&changed.blend};
      *fields[component]+=0.05f;
      if(curveCache.Get(changed)!=changed.Extension() || curveCache.Get(curve)!=curve.Extension())return 40;
    }
    std::string source=R"(
cbuffer Params : register(b0) { uint4 xe_sys2; float4 xe_color_exp_scale; };
Texture2D<float4> scene : register(t0);
RWTexture2D<float4> output : register(u0);
struct XePsIn { uint pixel; };
struct XePsOut { float4 color0; };
static const float4 xe_c[256] = {
)";
    for(unsigned i=0;i<256;++i) {
      if(i==0)source+="float4(1,0,0,0),";
      else if(i==4)source+="float4("+std::to_string(exposure)+",1,0,1),";
      else if(i==5)source+="float4(1,0,0,0),";
      else if(i==252)source+="float4(0,0.45,0,0),";
      else if(i==253)source+="float4(0,0,1,0),";
      else source+="float4(0,0,0,0),";
    }
    source+=R"(
};
XePsOut main(XePsIn xe_in) {
  float4 r[4];
  r[3] = scene.Load(int3(xe_in.pixel,0,0)).rbga;
  {
    float4 s0 = r[3].xzyy;
        float4 s1 = xe_c[4].xxxx;
  }
  XePsOut o;
  // An authored neutral LUT response with a fixed tint and a variable linear
  // fade. Bright scene input stays present even when the graded result is black.
  float3 nativeColor = pow(float3(0.5,1.0,0.25),2.2) *
      NativeGears3CurveLinear(max(r[3].x,0.0)) * asfloat(xe_sys2.y);
  float brightest = max(max(nativeColor.r, nativeColor.g), nativeColor.b);
  float highlightRange = max(asfloat(xe_sys2.x) - 1.0, 0.0001);
  float shoulder = 1.0 + highlightRange * (1.0 - exp(-max(brightest - 1.0, 0.0) / highlightRange));
  nativeColor *= shoulder / max(brightest, 1.0);
  o.color0=float4(nativeColor,1);
  return o;
}
)";
    source=hdr::Gears3SceneSource(source,256,256);
    if(source.empty())return 31;
    const auto main=source.find("XePsOut main(XePsIn xe_in)");
    source.replace(main,std::strlen("XePsOut main"),"XePsOut TestMaterial");
    source+=R"(
[numthreads(8,1,1)] void main(uint3 id : SV_DispatchThreadID) {
  if(id.x>=4)return;
  XePsIn input; input.pixel=id.x;
  output[id.xy]=TestMaterial(input).color0;
}
)";
    std::vector<uint8_t> bytes;std::string error;
    if(!compiler.Compile(source,gpu::ShaderStage::kCompute,backend.GetShaderIL(),bytes,error)) {
      std::printf("FAIL Gears 3 extension compile: %s\n",error.c_str());return 32;
    }
    const auto shader=backend.CreateShader(gpu::ShaderStage::kCompute,bytes.data(),bytes.size());
    const auto pipeline=shader?backend.CreateComputePipeline(shader):0;
    gpu::TextureDesc desc;desc.width=4;desc.height=1;desc.format=gpu::Format::kR16G16B16A16Float;
    desc.usage=gpu::kUsageSampled|gpu::kUsageStorage;
    const auto input=backend.CreateTexture(desc),output=backend.CreateTexture(desc);
    if(!pipeline||!input||!output)return 33;
    const float energies[]={-1.0f,pivot*0.5f,exposure*50,exposure*500};
    uint16_t data[16]{};
    for(unsigned i=0;i<4;++i)for(unsigned c=0;c<4;++c)data[4*i+c]=ToHalf(energies[i]);
    backend.UploadTexture(input,0,0,data,32,1,1);
    for(float peak : {4.0f,19.0f}) {
      float fullBrightness[16]{};
      for(float fade : {1.0f,0.5f,0.001f,0.0f}) {
      gpu::ComputeBindings bindings;bindings.constant_count=8;
      bindings.constants[0]=std::bit_cast<uint32_t>(peak);
      bindings.constants[1]=std::bit_cast<uint32_t>(fade);
      bindings.constants[2]=std::bit_cast<uint32_t>(pivot);bindings.constants[3]=std::bit_cast<uint32_t>(slope);
      bindings.constants[4]=std::bit_cast<uint32_t>(1.0f);bindings.srv[0]=input;bindings.uav_texture=output;
      backend.Dispatch(pipeline,bindings,1,1,1);
      const auto rb=backend.ReadbackTexture(output,{0,0,4,1});backend.WaitForFence(backend.Submit());
      uint32_t pitch=0;const auto* result=backend.ReadbackData(rb,pitch);uint16_t actual[16]{};
      if(result)std::memcpy(actual,result,sizeof(actual));backend.ReleaseReadback(rb);if(!result)return 34;
      for(unsigned i=0;i<4;++i) {
        const float red=Half(actual[4*i]),green=Half(actual[4*i+1]),blue=Half(actual[4*i+2]);
        if(!std::isfinite(red)||!std::isfinite(green)||!std::isfinite(blue)||red<0||blue<0||green>peak+0.02f)return 35;
        if(i<2 && std::abs(green-curve.Linear(std::max(Half(data[4*i]),0.0f))*fade)>0.001f)return 36;
        // A strongly over-range scene should use the selected peak's headroom
        // without raising paper white. The original paper-white-only extension
        // left this input below 25% of a 19x display range.
        if(i==2 && fade==1.0f && green<peak*0.8f) {
          std::printf("FAIL Gears 3 highlight headroom: %g of %g\n",green,peak);return 38;
        }
        if(i>=2 && fade==1.0f && (green<=1 || std::abs(red/green-std::pow(0.5f,2.2f))>0.002f ||
                    std::abs(blue/green-std::pow(0.25f,2.2f))>0.002f))return 37;
        for(unsigned c=0;c<3;++c) {
          const float value=Half(actual[4*i+c]);
          if(fade==1.0f)fullBrightness[4*i+c]=value;
          else if(std::abs(value-fullBrightness[4*i+c]*fade)>std::max(0.004f,peak*0.001f) ||
                  (fade==0.0f && value!=0.0f)) {
            std::printf("FAIL Gears 3 fade %.4f pixel %u: %g != %g\n",fade,i,value,fullBrightness[4*i+c]*fade);return 39;
          }
        }
      }
      }
    }
    backend.DestroyTexture(input);backend.DestroyTexture(output);
    std::printf("PASS Gears 3 extension exposure %.2f: SDR lows, highlight headroom, hue, peak bounds, linear fades, exact black\n",exposure);
  }
  return 0;
}
int main(int argc,char** argv) {
  const bool vulkan=argc>1&&std::string(argv[1])=="vulkan";
  const std::string mode=argc>2?argv[2]:"off";
  const bool quality=argc>3&&std::string(argv[3])=="quality";
  rex::cvar::SetFlagByName("gow2_fsr_vulkan","true");
  auto backend=vulkan?gpu::CreateVulkanBackend():gpu::CreateD3D12Backend();
  gpu::BackendOptions options;options.upload_ring_mb=(mode=="encoder_bench"||mode=="present_bench")?128:32;options.guest_memory_bytes=16ull<<20;
  std::string error;
  if(!backend || !backend->Init(options,error)) { std::printf("FAIL backend: %s\n",error.c_str());return 2; }
  auto compiler=xenos::renderer::CreateDxcCompiler({},error);
  if(!compiler) { std::printf("FAIL compiler: %s\n",error.c_str());return 3; }
  if(mode=="ui")return UiCompositeCheck(*backend,*compiler);
  if(mode=="gears3")return Gears3SceneCheck(*backend,*compiler);
  auto shader=[&](const char* source,gpu::ShaderStage stage) {
    std::vector<uint8_t> bytes;std::string messages;
    if(!compiler->Compile(source,stage,backend->GetShaderIL(),bytes,messages)) {
      std::printf("FAIL shader: %s\n",messages.c_str());return gpu::ShaderHandle(0);
    }
    return backend->CreateShader(stage,bytes.data(),bytes.size());
  };
  auto compute=[&](const char* source) {const auto s=shader(source,gpu::ShaderStage::kCompute);return s?backend->CreateComputePipeline(s):0;};
  if((mode=="encoder_bench" && argc==4) || (mode=="present_bench" && argc==3)) {
    const bool presentation=mode=="present_bench";
    std::string oldSource=hdr::MatchingSizeEncoder();
    if(!presentation){std::ifstream oldFile(argv[3],std::ios::binary);oldSource.assign(std::istreambuf_iterator<char>(oldFile),{});}
    const auto oldPipeline=compute(oldSource.c_str()),newPipeline=compute(hdr::MatchingSizeEncoder().c_str());
    constexpr uint32_t w=3840,h=2160;
    gpu::TextureDesc d;d.width=w;d.height=h;d.format=gpu::Format::kR16G16B16A16Float;d.usage=gpu::kUsageSampled;
    const auto input=backend->CreateTexture(d);d.format=gpu::Format::kR32Uint;d.usage|=gpu::kUsageStorage;
    const auto oldOutput=backend->CreateTexture(d),newOutput=backend->CreateTexture(d);
    if(!oldPipeline||!newPipeline||!input||!oldOutput||!newOutput)return 26;
    std::vector<uint16_t> data(size_t(w)*h*4);
    for(size_t i=0;i<data.size();i+=4){data[i]=ToHalf(float((i/4)%73)/16);data[i+1]=ToHalf(float((i/4)%31)/32);data[i+2]=ToHalf(float((i/4)%19)/8);data[i+3]=0x3c00;}
    backend->UploadTexture(input,0,0,data.data(),w*8,h,1);backend->WaitForFence(backend->Submit());
    gpu::PipelineHandle gamma=0;gpu::ComputeBindings gammaBindings;
    if(presentation) {
      gamma=compute(xenos::renderer::builtin::kPresentGammaCs);
      const auto reference=backend->CreateTexture(d);
      d.width=640;d.height=1;const auto ramp=backend->CreateTexture(d);
      if(!gamma||!reference||!ramp)return 41;
      std::vector<uint32_t> words(size_t(w)*h);
      for(size_t i=0;i<words.size();++i)words[i]=uint32_t(i)*1664525u+1013904223u;
      backend->UploadTexture(reference,0,0,words.data(),w*4,h,1);backend->WaitForFence(backend->Submit());
      uint32_t rampWords[640]{};
      for(unsigned i=0;i<256;++i){const auto v=unsigned(std::round(i*1023.0/255.0));rampWords[i]=v|(v<<10)|(v<<20);}
      backend->UploadTexture(ramp,0,0,rampWords,sizeof(rampWords),1,1);backend->WaitForFence(backend->Submit());
      gammaBindings.constants[0]=w;gammaBindings.constants[1]=h;gammaBindings.constants[4]=0x688;
      gammaBindings.constant_count=8;gammaBindings.srv[0]=reference;gammaBindings.srv[1]=ramp;
    }
    gpu::ComputeBindings b;b.constants[0]=b.constants[2]=w;b.constants[1]=b.constants[3]=h;
    b.constants[4]=std::bit_cast<uint32_t>(200.f);b.constants[5]=std::bit_cast<uint32_t>(3800.f);b.constants[6]=std::bit_cast<uint32_t>(1.f);b.constant_count=8;b.srv[0]=input;
    for(float sharp:{0.f,.35f}) {
      b.constants[7]=std::bit_cast<uint32_t>(sharp);
      auto run=[&](gpu::PipelineHandle p,gpu::TextureHandle out,int iterations){
        b.uav_texture=out;gammaBindings.uav_texture=out;
        for(int i=0;i<iterations;++i){
          if(presentation&&out==oldOutput)backend->Dispatch(gamma,gammaBindings,(w+7)/8,(h+7)/8,1);
          backend->Dispatch(p,b,(w+7)/8,(h+7)/8,1);
        }
        backend->WaitForFence(backend->Submit());backend->CompletedFence();
      };
      backend->SetGpuTag(0);run(oldPipeline,oldOutput,80);run(newPipeline,newOutput,80);
      std::vector<double> oldTimes,newTimes;
      std::vector<gpu::GpuTagTime> timings;backend->TakeGpuTagTimes(timings);
      constexpr unsigned iterations=80;
      for(int trial=0;trial<6;++trial)for(int kind=0;kind<2;++kind){
        const bool revised=(kind+(trial&1))%2;
        backend->SetGpuTag(unsigned(trial)*2+(revised?2:1));
        run(revised?newPipeline:oldPipeline,revised?newOutput:oldOutput,iterations);
      }
      // Vulkan retires timestamp queries at the following submission. Attribute
      // batches by their recorded tags, not a potentially delayed Stats delta.
      backend->SetGpuTag(0);run(newPipeline,newOutput,1);run(newPipeline,newOutput,1);
      backend->TakeGpuTagTimes(timings);double batches[13]{};
      for(const auto& timing:timings)if(timing.tag>0&&timing.tag<=12)batches[timing.tag]+=timing.ms;
      for(unsigned tag=1;tag<=12;++tag){if(batches[tag]<=0){std::puts("FAIL missing GPU timestamps");return 29;}(tag%2?oldTimes:newTimes).push_back(batches[tag]/iterations);}
      std::sort(oldTimes.begin(),oldTimes.end());std::sort(newTimes.begin(),newTimes.end());
      const auto a=backend->ReadbackTexture(oldOutput,{0,0,int(w),int(h)}),c=backend->ReadbackTexture(newOutput,{0,0,int(w),int(h)});
      backend->WaitForFence(backend->Submit());uint32_t ap=0,cp=0;const auto* av=backend->ReadbackData(a,ap);const auto* cv=backend->ReadbackData(c,cp);
      if(!av||!cv){std::string lost;backend->IsLost(&lost);std::printf("FAIL encoder readback: old %llu new %llu; %s\n",static_cast<unsigned long long>(a),static_cast<unsigned long long>(c),lost.c_str());return 27;}
      for(unsigned y=0;y<h;++y)if(std::memcmp(av+size_t(y)*ap,cv+size_t(y)*cp,w*4)){std::printf("FAIL encoder changed pixels at row %u\n",y);return 28;}
      backend->ReleaseReadback(a);backend->ReleaseReadback(c);
      std::printf("PASS 4K %s sharp %.2f: identical 8294400 pixels, median GPU %.4f -> %.4f ms (%.1f%% faster)\n",presentation?"native presentation":"encoder",sharp,oldTimes[3],newTimes[3],100*(1-newTimes[3]/oldTimes[3]));
    }
    return 0;
  }
  if((mode=="profile" || mode=="scene_profile" || mode=="binary_profile") && argc==5) {
    const auto load=[](const char* path) {std::ifstream file(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(file),{});};
    const auto vs=load(argv[3]),ps=load(argv[4]);
    gpu::GraphicsPipelineDesc p;
    p.vs=mode=="binary_profile"?backend->CreateShader(gpu::ShaderStage::kVertex,vs.data(),vs.size()):shader(vs.c_str(),gpu::ShaderStage::kVertex);
    p.ps=mode=="binary_profile"?backend->CreateShader(gpu::ShaderStage::kPixel,ps.data(),ps.size()):shader(ps.c_str(),gpu::ShaderStage::kPixel);
    p.color_count=mode=="profile"?2:1;p.depth_clip=false;
    for(unsigned i=0;i<p.color_count;++i) {
      p.color_formats[i]=gpu::Format::kR16G16B16A16Float;p.color_write_masks[i]=7;
      p.color_blend[i].enable=mode!="scene_profile";p.color_blend[i].src=gpu::BlendFactor::kSrcAlpha;p.color_blend[i].dst=gpu::BlendFactor::kInvSrcAlpha;
    }
    if(vs.empty()||ps.empty()||!p.vs||!p.ps||!backend->GetGraphicsPipeline(p))return 25;
    std::printf("PASS captured %s: real material, matching vertex shader, FP16 pipeline\n",mode.c_str());return 0;
  }
  const uint32_t rw=1280,rh=720,ow=quality?1920:rw,oh=quality?1080:rh;
  auto texture=[&](uint32_t w,uint32_t h,gpu::Format format,uint32_t usage) {
    gpu::TextureDesc d;d.width=w;d.height=h;d.format=format;d.usage=usage;d.debug_name="HDR range test";
    return backend->CreateTexture(d);
  };
  constexpr uint32_t sampled=gpu::kUsageSampled, rt=sampled|gpu::kUsageRenderTarget, work=rt|gpu::kUsageStorage;
  const auto input=texture(rw,rh,gpu::Format::kR16G16B16A16Float,work);
  const auto output=texture(ow,oh,gpu::Format::kR16G16B16A16Float,work);
  const auto depth=texture(rw,rh,gpu::Format::kR32Float,sampled);
  const auto motion=texture(rw,rh,gpu::Format::kR16G16Float,sampled);
  if(!input||!output||!depth||!motion) return 4;
  std::vector<uint16_t> color(size_t(rw)*rh*4);
  for(size_t i=0;i<color.size();i+=4) {color[i]=0x4400;color[i+1]=0x4000;color[i+2]=0x3C00;color[i+3]=0x3C00;}
  std::vector<float> depthData(size_t(rw)*rh,.5f);
  std::vector<uint32_t> motionData(size_t(rw)*rh,0);
  backend->UploadTexture(input,0,0,color.data(),rw*8,rh,1);
  backend->UploadTexture(depth,0,0,depthData.data(),rw*4,rh,1);
  backend->UploadTexture(motion,0,0,motionData.data(),rw*4,rh,1);
  gpu::TextureHandle result=input;
  const auto kind=up::ParseKind(mode);
  if(kind==up::Kind::kDlss || kind==up::Kind::kFsr || kind==up::Kind::kXess) {
    auto aa=up::CreateUpscaler(kind);
    if(!aa || !aa->Configure(*backend,rw,rh,ow,oh,true,true,false)) {
      std::printf("FAIL %s configure: %s\n",mode.c_str(),aa?aa->Status().c_str():"not built");return 5;
    }
    up::FrameInputs frame;frame.color=input;frame.depth=depth;frame.motion=motion;frame.output=output;
    frame.render_width=rw;frame.render_height=rh;frame.output_width=ow;frame.output_height=oh;
    frame.hdr=true;frame.camera_near=.1f;frame.camera_far=10000.0f;frame.vertical_fov=1.0f;
    for(unsigned i=0;i<8;++i) {
      frame.reset=i==0;
      if(!aa->Evaluate(*backend,frame)) {std::printf("FAIL %s: %s\n",mode.c_str(),aa->Status().c_str());return 6;}
      backend->WaitForFence(backend->Submit());
    }
    result=output;
    std::printf("%s\n",aa->Status().c_str());
    // Finish GPU use before destroying SDK contexts.
    backend->WaitForFence(backend->Submit());
  } else if(mode=="smaa") {
    const auto weights=texture(rw,rh,gpu::Format::kR8G8B8A8Unorm,rt);
    const float zero[4]={};backend->ClearColor(weights,zero);
    const auto vsSource=smaa::Source(smaa::kBlendVs), psSource=smaa::Source(smaa::kT2xBlendPs);
    gpu::GraphicsPipelineDesc p;p.vs=shader(vsSource.c_str(),gpu::ShaderStage::kVertex);p.ps=shader(psSource.c_str(),gpu::ShaderStage::kPixel);
    p.depth_clip=false;p.color_count=1;p.color_formats[0]=gpu::Format::kR16G16B16A16Float;p.color_write_masks[0]=15;
    const auto pipeline=p.vs&&p.ps?backend->GetGraphicsPipeline(p):0;
    if(!pipeline || quality) return 7;
    gpu::TextureView views[96];views[0].texture=input;views[1].texture=weights;
    gpu::SamplerDesc samplers[32];for(auto& s:samplers) for(auto& address:s.address) address=gpu::SamplerAddress::kClamp;
    const float metrics[4]={1.0f/rw,1.0f/rh,float(rw),float(rh)};
    backend->SetTextures(views,samplers,2,2);backend->SetConstantsBoth(gpu::kCbSystem,metrics,sizeof(metrics));
    backend->SetRenderTargets(1,&output,0);backend->SetViewport({0,0,float(rw),float(rh),0,1});
    backend->SetScissor({0,0,int(rw),int(rh)});backend->SetPipeline(pipeline);backend->Draw(3);
    result=output;
  } else if(mode=="smaa_t2x") {
    const auto edge=texture(rw,rh,gpu::Format::kR10G10B10A2Unorm,work);
    const auto mapped=texture(rw,rh,gpu::Format::kR16G16B16A16Float,work);
    const auto prep=compute(smaa::kT2xPrepCs), resolve=compute(smaa::kT2xResolveCs);
    if(!edge||!mapped||!prep||!resolve||quality) return 7;
    gpu::ComputeBindings b;b.constants[0]=std::bit_cast<uint32_t>(1.0f);b.constants[1]=std::bit_cast<uint32_t>(1.0f);b.constant_count=4;
    b.srv[0]=input;b.uav_texture=edge;b.uav2_texture=mapped;backend->Dispatch(prep,b,(rw+7)/8,(rh+7)/8,1);
    b.srv[0]=mapped;b.srv[1]=mapped;b.srv[2]=motion;b.uav_texture=output;b.uav2_texture=0;
    backend->Dispatch(resolve,b,(rw+7)/8,(rh+7)/8,1);result=output;
  } else if(mode!="off" || quality) return 8;
  const uint32_t width=quality?ow:rw,height=quality?oh:rh;
  auto readback=backend->ReadbackTexture(result,{int(width/2),int(height/2),int(width/2+1),int(height/2+1)});
  backend->WaitForFence(backend->Submit());uint32_t pitch=0;
  const auto* bytes=backend->ReadbackData(readback,pitch);
  if(!bytes) return 9;
  uint16_t half[4];std::memcpy(half,bytes,sizeof(half));
  const float red=Half(half[0]),green=Half(half[1]),blue=Half(half[2]);
  backend->ReleaseReadback(readback);
  if(!std::isfinite(red)||!std::isfinite(green)||!std::isfinite(blue)||red<1.5f||std::abs(red-4)>.3f) {
    std::printf("FAIL %s HDR RGB: %g %g %g\n",mode.c_str(),red,green,blue);return 11;
  }
  const auto packed=texture(width,height,gpu::Format::kR32Uint,sampled|gpu::kUsageStorage);
  const auto encoder=compute(hdr::kEncodeCs);if(!packed||!encoder) return 12;
  gpu::ComputeBindings b;b.constants[0]=width;b.constants[1]=height;b.constants[2]=width;b.constants[3]=height;
  b.constants[4]=std::bit_cast<uint32_t>(203.f);b.constants[5]=std::bit_cast<uint32_t>(3800.f);b.constants[6]=std::bit_cast<uint32_t>(1.f);
  b.constant_count=12;b.srv[0]=result;b.uav_texture=packed;backend->Dispatch(encoder,b,(width+7)/8,(height+7)/8,1);
  readback=backend->ReadbackTexture(packed,{int(width/2),int(height/2),int(width/2+1),int(height/2+1)});
  backend->WaitForFence(backend->Submit());bytes=backend->ReadbackData(readback,pitch);uint32_t word=0;
  if(bytes)std::memcpy(&word,bytes,4);backend->ReleaseReadback(readback);
  const double to2020red=.627404*red+.329282*green+.0433136*blue;
  const int expected=int(std::round(Pq(203*to2020red)*1023));
  if(!bytes || std::abs(int(word&1023)-expected)>2 || (word&1023)<=unsigned(std::round(Pq(203)*1023))) return 13;
  if(backend->IsLost(&error)) {std::printf("FAIL device lost: %s\n",error.c_str());return 14;}
  std::printf("PASS %s %s %s: FP16 RGB %.4f %.4f %.4f -> PQ R %u, 203-nit white %u\n",
      vulkan?"Vulkan":"D3D12",mode.c_str(),quality?"quality":"native",red,green,blue,word&1023,unsigned(std::round(Pq(203)*1023)));
}
