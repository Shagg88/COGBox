// GPU comparisons of supplied transient programs. No game bytecode is stored here.
#pragma once
static int JudgmentFxaaPairCheck(gpu::Backend& backend,xenos::renderer::ShaderCompiler& compiler,
    const char* originalPath,const char* pairedPath,bool bench=false) {
  const unsigned w=bench?3840:64,h=bench?2160:32;
  auto load=[](const char* path){std::ifstream f(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(f),{});};
  const auto original=load(originalPath),paired=load(pairedPath);if(original.empty()||paired.empty())return 140;
  auto shader=[&](const std::string& source,gpu::ShaderStage stage){
    std::vector<uint8_t> b;std::string e;if(!compiler.Compile(source,stage,backend.GetShaderIL(),b,e)) {
      std::printf("FAIL paired shader: %s\n",e.c_str());return gpu::ShaderHandle(0);
    }
    return backend.CreateShader(stage,b.data(),b.size());
  };
  const auto vertex=shader(std::string("#define TEST_W ")+std::to_string(w)+".0\n#define TEST_H "+std::to_string(h)+".0\n"+R"(
struct O{float4 interp[16]:TEXCOORD0;float4 position:SV_Position;};
O main(uint v:SV_VertexID){O o=(O)0;float2 uv=float2((v<<1)&2,v&2);o.position=float4(uv*float2(2,-2)+float2(-1,1),0,1);
 o.interp[0]=float4(uv,uv);o.interp[1]=uv.xyxy+float4(-1.0/TEST_W,-1.0/TEST_H,1.0/TEST_W,1.0/TEST_H);o.interp[2]=float4(uv,uv);return o;}
)",gpu::ShaderStage::kVertex);
  auto legacy=original;const auto legacyAt=legacy.rfind("  return o;");if(legacyAt==std::string::npos)return 140;
  legacy.insert(legacyAt,"  o.color0.rgb=pow(max(o.color0.rgb,0.0),2.2);\n");
  auto replay=original;const auto at=replay.rfind("  return o;");if(at==std::string::npos)return 140;
  replay.insert(at,"  o.color0.rgb=pow(max(o.color0.rgb,0.0),2.2);\n");
  // A direct path must share the *original* SDR luma decisions. The previous
  // proxy's half-rounded alpha can select different edges at exact ties, so
  // use the original alpha in this independent encoded-RGB reference too.
  const auto mainAt=replay.find("XePsOut main(");
  replay.insert(mainAt,"XE_VK(31,1) Texture2D<float4> referenceAlpha:register(t31,space0);\n");
  const std::string sample="float4 tv = xe_tex_post(xe_tex_2d_0.SampleLevel(xe_sampler_0, tc, 0), 0u);";
  size_t sampleAt=0;while((sampleAt=replay.find(sample,sampleAt))!=std::string::npos) {
    const std::string replacement=
        "float4 "
        "refSample=xe_tex_2d_0.SampleLevel(xe_sampler_0,tc,0);refSample.a=referenceAlpha."
        "SampleLevel(xe_sampler_0,tc,0).a;float4 tv=xe_tex_post(refSample,0u);";
    replay.replace(sampleAt,sample.size(),replacement);sampleAt+=replacement.size();
  }
  auto pipeline=[&](const std::string& source,unsigned count,bool hdrFirst){
    gpu::GraphicsPipelineDesc d;d.vs=vertex;d.ps=shader(source,gpu::ShaderStage::kPixel);d.depth_clip=false;d.color_count=count;
    for(unsigned i=0;i<count;++i){d.color_formats[i]=(i==0?hdrFirst:!hdrFirst)?gpu::Format::kR16G16B16A16Float:gpu::Format::kR8G8B8A8Unorm;d.color_write_masks[i]=15;}
    return d.ps?backend.GetGraphicsPipeline(d):0;
  };
  const auto sdrPipe=pipeline(original,1,false),hdrPipe=pipeline(replay,1,true),pairPipe=pipeline(paired,2,false);
  const auto legacyPipe=bench?pipeline(legacy,1,true):0;
  const auto cs=shader(hdr::kEncodeSceneForFxaa,gpu::ShaderStage::kCompute);const auto encode=cs?backend.CreateComputePipeline(cs):0;
  if(!vertex||!sdrPipe||!hdrPipe||!pairPipe||!encode)return 141;
  gpu::TextureDesc td;td.width=w;td.height=h;td.format=gpu::Format::kR16G16B16A16Float;td.usage=gpu::kUsageSampled|gpu::kUsageRenderTarget|gpu::kUsageStorage;
  const auto scene=backend.CreateTexture(td),proxy=backend.CreateTexture(td),hdrOld=backend.CreateTexture(td),hdrNew=backend.CreateTexture(td);
  td.format=gpu::Format::kR8G8B8A8Unorm;const auto sdr=backend.CreateTexture(td),sdrOld=backend.CreateTexture(td),sdrNew=backend.CreateTexture(td);
  if(!scene||!proxy||!hdrOld||!hdrNew||!sdr||!sdrOld||!sdrNew)return 142;
  const bool high=std::string(originalPath).find("16CBC2E3D5BEC0A5")!=std::string::npos;
  float cf[256][4];for(auto& c:cf)std::fill_n(c,4,1.f);
  const float low3[]={1.f/w,1.f/h,-1.f/w,-1.f/h},low7[]={-.5f,-.5f,.5f,.5f},low9[]={1,-1,8,8};
  std::copy_n(low3,4,cf[3]);std::copy_n(low7,4,cf[7]);std::copy_n(low9,4,cf[9]);
  cf[4][0]=8;cf[5][0]=.166f;cf[8][0]=.03125f;
  cf[255][0]=.5;cf[255][1]=.25;cf[255][2]=cf[255][3]=0;
  if(high){cf[3][0]=.75;cf[4][0]=.166;cf[5][0]=.03125;cf[7][0]=1.f/w;cf[7][1]=1.f/h;
    cf[254][0]=0;cf[254][1]=2;cf[255][0]=1;cf[255][1]=.5;cf[255][2]=0;cf[255][3]=-1;}
  uint32_t sys[6][4]{};sys[0][2]=0xffff;sys[1][3]=std::bit_cast<uint32_t>(1.f);
  for(auto& c:sys[3])c=std::bit_cast<uint32_t>(1.f);sys[5][0]=w;sys[5][1]=h;
  uint32_t fetch[48][4]{};fetch[0][0]=0x80000002;fetch[0][1]=6;fetch[0][3]=0x00280D10;
  uint32_t boolean[10][4]{};
  float worst=0;
  for(unsigned pattern=0;pattern<(bench?1u:3u);++pattern)for(bool swap:{false,true})for(bool wrap:{false,true}) {
    if(bench && (swap||wrap))continue;
    std::vector<uint8_t> bounded(w*h*4);std::vector<uint16_t> extended(w*h*4);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){const unsigned p=x+w*y;
      float a=pattern==0?float((x/5+y/3)&1):pattern==1?float((p*1664525u+1013904223u)&255)/255.f:.18f;
      unsigned luma=unsigned(std::round(a*255));
      for(unsigned c=0;c<3;++c){float value=a*float(c+1)*2.f+.001f*c;
        extended[4*p+c]=ToHalf(value);bounded[4*p+c]=uint8_t(std::round(std::min(std::pow(value,1.f/2.2f),1.f)*255));}
      extended[4*p+3]=ToHalf(float(luma)/255.f);bounded[4*p+3]=uint8_t(luma);
    }
    backend.UploadTexture(scene,0,0,extended.data(),w*8,h,1);backend.UploadTexture(sdr,0,0,bounded.data(),w*4,h,1);
    gpu::ComputeBindings cb;cb.srv[0]=scene;cb.uav_texture=proxy;backend.Dispatch(encode,cb,(w+7)/8,(h+7)/8,1);
    sys[5][2]=wrap?0:0x202;
    backend.SetConstantsBoth(0,sys,sizeof(sys));backend.SetConstants(gpu::ShaderStage::kPixel,1,cf,sizeof(cf));
    backend.SetConstants(gpu::ShaderStage::kPixel,2,boolean,sizeof(boolean));backend.SetConstants(gpu::ShaderStage::kPixel,3,fetch,sizeof(fetch));
    gpu::SamplerDesc sampler;sampler.mip_linear=0;for(auto& a:sampler.address)a=wrap?gpu::SamplerAddress::kWrap:gpu::SamplerAddress::kClamp;
    gpu::TextureView views[32];views[0].texture=sdr;views[31].texture=scene;
    if(swap){views[0].components[0]=views[31].components[0]=2;views[0].components[2]=views[31].components[2]=0;}
    backend.SetViewport({0,0,float(w),float(h),0,1});backend.SetScissor({0,0,int(w),int(h)});
    backend.SetTextures(views,&sampler,1,1);backend.SetRenderTargets(1,&sdrOld,0);backend.SetPipeline(sdrPipe);backend.Draw(3);
    views[0].texture=proxy;views[31].texture=sdr;backend.SetTextures(views,&sampler,32,1);backend.SetRenderTargets(1,&hdrOld,0);backend.SetPipeline(hdrPipe);backend.Draw(3);
    views[0].texture=sdr;views[31].texture=scene;const gpu::TextureHandle targets[]={sdrNew,hdrNew};
    backend.SetTextures(views,&sampler,32,1);backend.SetRenderTargets(2,targets,0);backend.SetPipeline(pairPipe);backend.Draw(3);
    if(bench) {
      if(!legacyPipe)return 146;
      auto run=[&](bool fused,unsigned iterations){for(unsigned n=0;n<iterations;++n) {
        if(fused) {
          views[0].texture=sdr;views[31].texture=scene;backend.SetTextures(views,&sampler,32,1);
          backend.SetRenderTargets(2,targets,0);backend.SetPipeline(pairPipe);backend.Draw(3);
        } else {
          backend.Dispatch(encode,cb,(w+7)/8,(h+7)/8,1);
          views[0].texture=proxy;backend.SetTextures(views,&sampler,1,1);backend.SetRenderTargets(1,&hdrOld,0);backend.SetPipeline(legacyPipe);backend.Draw(3);
          views[0].texture=sdr;backend.SetTextures(views,&sampler,1,1);backend.SetRenderTargets(1,&sdrOld,0);backend.SetPipeline(sdrPipe);backend.Draw(3);
        }
      }backend.WaitForFence(backend.Submit());};
      run(false,8);run(true,8);std::vector<gpu::GpuTagTime> times;backend.TakeGpuTagTimes(times);times.clear();
      for(unsigned trial=0;trial<6;++trial)for(unsigned kind=0;kind<2;++kind) {
        const bool fused=(kind+(trial&1))&1;backend.SetGpuTag(trial*2+(fused?2:1));run(fused,16);
      }
      backend.SetGpuTag(0);run(true,1);run(true,1);backend.TakeGpuTagTimes(times);
      double sums[13]{};for(const auto& t:times)if(t.tag>0&&t.tag<=12)sums[t.tag]+=t.ms/16;
      std::vector<double> before,after;for(unsigned tag=1;tag<=12;++tag) {
        if(sums[tag]<=0)return 147;(tag&1?before:after).push_back(sums[tag]);
      }
      std::sort(before.begin(),before.end());std::sort(after.begin(),after.end());
      std::printf(
              "BENCH paired 4K FXAA %s: old encode + HDR/SDR replays %.4f ms, single draw %.4f ms "
              "(GPU, fixture only)\n",high?"high":"low",before[3],after[3]);
      return 0;
    }
    gpu::ReadbackHandle readbacks[4];const gpu::TextureHandle outputs[]={sdrOld,sdrNew,hdrOld,hdrNew};
    for(unsigned i=0;i<4;++i)readbacks[i]=backend.ReadbackTexture(outputs[i],{0,0,int(w),int(h)});
    backend.WaitForFence(backend.Submit());std::vector<uint8_t> values[4];
    for(unsigned i=0;i<4;++i){uint32_t pitch=0;const auto* raw=backend.ReadbackData(readbacks[i],pitch);const unsigned row=w*(i<2?4:8);
      if(!raw)return 143;values[i].resize(row*h);for(unsigned y=0;y<h;++y)std::memcpy(values[i].data()+y*row,raw+y*pitch,row);backend.ReleaseReadback(readbacks[i]);}
    for(unsigned i=0;i<w*h*4;++i){
      if(std::abs(int(values[0][i])-int(values[1][i]))>1)return 144;
      const float a=Half(reinterpret_cast<const uint16_t*>(values[2].data())[i]),b=Half(reinterpret_cast<const uint16_t*>(values[3].data())[i]);
      worst=std::max(worst,std::abs(a-b));
      if(!std::isfinite(b)||std::abs(a-b)>std::max(.006f,std::abs(a)*.012f)){
        std::printf("FAIL paired FXAA %s pattern %u swap %u wrap %u pixel %u: %g != %g\n",high?"high":"low",pattern,swap,wrap,i,a,b);return 145;}
    }
  }
  std::printf(
      "PASS paired FXAA %s: original SDR output, shared edge decisions, HDR proxy equivalence (max "
      "absolute %g), wrap/clamp and R/B mapping\n",high?"high":"low",worst);return 0;
}
