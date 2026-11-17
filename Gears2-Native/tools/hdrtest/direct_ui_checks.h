// Authored GPU checks for direct HDR UI. No game shader data.
#pragma once
static int DirectUiAttachmentCheck(gpu::Backend& backend,xenos::renderer::ShaderCompiler& compiler) {
  auto shader=[&](const std::string& src,gpu::ShaderStage stage) {
    std::vector<uint8_t> b;std::string e;
    if(!compiler.Compile(src,stage,backend.GetShaderIL(),b,e)){std::printf("FAIL direct UI compile: %s\n",e.c_str());return gpu::ShaderHandle(0);}
    return backend.CreateShader(stage,b.data(),b.size());
  };
  gpu::TextureDesc td;td.width=2;td.height=4;td.format=gpu::Format::kR16G16B16A16Float;
  td.usage=gpu::kUsageRenderTarget|gpu::kUsageSampled|gpu::kUsageStorage;const auto hdrTarget=backend.CreateTexture(td);
  td.format=gpu::Format::kD24UnormS8;td.usage=gpu::kUsageDepthStencil;const auto depth=backend.CreateTexture(td);
  gpu::GraphicsPipelineDesc original;original.vs=OverlayFixtureVertex(backend,compiler,false);
  original.ps=shader("float4 main():SV_Target0{return 1.0.xxxx;}",gpu::ShaderStage::kPixel);
  original.depth_format=td.format;original.depth_clip=false;original.color_count=1;
  original.color_formats[0]=gpu::Format::kR16G16B16A16Float;original.color_write_masks[0]=0;
  original.stencil_enable=true;original.stencil_read_mask=original.stencil_write_mask=255;
  original.stencil_front.func=original.stencil_back.func=gpu::CompareFunc::kAlways;
  original.stencil_front.pass=original.stencil_back.pass=gpu::StencilOp::kReplace;
  const auto mark=backend.GetGraphicsPipeline(original);
  if(!mark||!depth||!hdrTarget)return 70;
  backend.ClearDepthStencil(depth,true,.5f,true,0,nullptr,0);
  backend.SetRenderTargets(1,&hdrTarget,depth);backend.SetViewport({0,0,2,1,0,1});backend.SetScissor({0,0,1,1});
  backend.SetStencilRef(1);backend.SetPipeline(mark);backend.Draw(3);
  original.color_write_masks[0]=15;original.depth_enable=original.depth_write=true;original.depth_func=gpu::CompareFunc::kLess;
  original.stencil_front.func=original.stencil_back.func=gpu::CompareFunc::kEqual;
  original.stencil_front.pass=original.stencil_back.pass=gpu::StencilOp::kZero;
  const auto replay=backend.GetGraphicsPipeline(hdr::OverlayPipeline(original,hdr::OverlayMode::kUi));if(!replay)return 71;
  const float background[]={8,4,2,1};
  // A no-FXAA scene arrives in a visible-only FP16 staging image. Publish
  // exactly that rectangle before stencil UI, leaving the padded rows alone.
  gpu::TextureDesc stageDesc;stageDesc.width=2;stageDesc.height=1;
  stageDesc.format=gpu::Format::kR16G16B16A16Float;stageDesc.usage=gpu::kUsageSampled;
  const auto stage=backend.CreateTexture(stageDesc);
  const auto copyShader=shader(hdr::kCopyForUiCs,gpu::ShaderStage::kCompute);
  const auto copy=copyShader?backend.CreateComputePipeline(copyShader):0;
  if(!stage||!copy)return 77;
  uint16_t scene[8];for(unsigned i=0;i<8;++i)scene[i]=ToHalf(background[i%4]);
  backend.UploadTexture(stage,0,0,scene,16,1,1);

  for(unsigned pass=0;pass<2;++pass) {
    const float padding[]={65000,65000,65000,1};backend.ClearColor(hdrTarget,padding);
    gpu::ComputeBindings publish;publish.constants[0]=2;publish.constants[1]=1;publish.constant_count=4;
    publish.srv[0]=stage;publish.uav_texture=hdrTarget;backend.Dispatch(copy,publish,1,1,1);
    const auto pad=backend.ReadbackTexture(hdrTarget,{0,1,2,2});backend.WaitForFence(backend.Submit());
    uint32_t padPitch=0;const auto* padData=backend.ReadbackData(pad,padPitch);
    uint16_t padValue=0;if(padData)std::memcpy(&padValue,padData,2);backend.ReleaseReadback(pad);
    if(!padData||padValue!=ToHalf(65000))return 78;
    backend.SetRenderTargets(1,&hdrTarget,depth);
    backend.SetViewport({0,0,2,1,0,1});backend.SetScissor({0,0,2,1});backend.SetStencilRef(1);backend.SetPipeline(replay);backend.Draw(3);
    const auto r=backend.ReadbackTexture(hdrTarget,{0,0,2,1});backend.WaitForFence(backend.Submit());
    uint32_t pitch=0;const auto* data=backend.ReadbackData(r,pitch);uint16_t out[8]{};if(data)std::memcpy(out,data,sizeof(out));backend.ReleaseReadback(r);
    if(!data||Half(out[0])!=1||Half(out[4])!=8){std::printf("FAIL direct UI coverage/write isolation pass %u\n",pass);return 72;}
  }
  // Deliberately poisonous padding catches UVs that normalize by physical extent.
  std::vector<uint16_t> padded(2*4*4,ToHalf(65000));
  for(unsigned x=0;x<2;++x)for(unsigned c=0;c<4;++c)padded[x*4+c]=ToHalf(background[c]);
  backend.UploadTexture(hdrTarget,0,0,padded.data(),16,4,1);
  td.height=1;td.format=gpu::Format::kR16G16B16A16Float;td.usage=gpu::kUsageRenderTarget|gpu::kUsageSampled;
  const auto output=backend.CreateTexture(td);td.format=gpu::Format::kR8G8B8A8Unorm;const auto weights=backend.CreateTexture(td);
  const float zero[4]{};backend.ClearColor(weights,zero);
  gpu::GraphicsPipelineDesc aa;aa.vs=shader(smaa::Source(smaa::kBlendVs),gpu::ShaderStage::kVertex);
  aa.ps=shader(smaa::NativeHdrBlendSource(),gpu::ShaderStage::kPixel);aa.depth_clip=false;aa.color_count=1;
  aa.color_formats[0]=gpu::Format::kR16G16B16A16Float;aa.color_write_masks[0]=15;
  const auto blend=backend.GetGraphicsPipeline(aa);if(!blend||!output||!weights)return 73;
  gpu::TextureView views[2];views[0].texture=hdrTarget;views[1].texture=weights;
  gpu::SamplerDesc samplers[2];for(auto& sampler:samplers)for(auto& address:sampler.address)address=gpu::SamplerAddress::kClamp;
  const float metrics[]={.5f,1.f,2.f,1.f};backend.SetConstantsBoth(gpu::kCbSystem,metrics,sizeof(metrics));
  backend.SetTextures(views,samplers,2,2);backend.SetRenderTargets(1,&output,0);backend.SetPipeline(blend);backend.Draw(3);
  const auto r=backend.ReadbackTexture(output,{0,0,2,1});backend.WaitForFence(backend.Submit());uint32_t pitch=0;
  const auto* data=backend.ReadbackData(r,pitch);uint16_t out[8]{};if(data)std::memcpy(out,data,sizeof(out));backend.ReleaseReadback(r);
  if(!data)return 74;
  for(unsigned x=0;x<2;++x)for(unsigned c=0;c<3;++c)if(std::abs(Half(out[x*4+c])-background[c])>.01f)return 75;
  if(backend.IsLost(nullptr))return 76;
  std::puts(
      "PASS direct padded UI: stencil/scissor coverage, retained HDR, read-only depth/stencil, one "
      "visible no-FXAA publish, untouched padding, SMAA rejects poisoned padding");return 0;
}

static int UiPaddingBench(gpu::Backend& backend,xenos::renderer::ShaderCompiler& compiler) {
  auto shader=[&](const char* src,gpu::ShaderStage stage) {
    std::vector<uint8_t> b;std::string e;if(!compiler.Compile(src,stage,backend.GetShaderIL(),b,e))return gpu::ShaderHandle(0);
    return backend.CreateShader(stage,b.data(),b.size());
  };
  constexpr unsigned w=3840,h=2160,padded=6144,draws=35,frames=3;
  gpu::TextureDesc td;td.width=w;td.height=h;td.format=gpu::Format::kR16G16B16A16Float;td.usage=gpu::kUsageSampled|gpu::kUsageRenderTarget;
  const auto upstream=backend.CreateTexture(td);td.height=padded;const auto direct=backend.CreateTexture(td);
  gpu::GraphicsPipelineDesc d;d.vs=OverlayFixtureVertex(backend,compiler,false);
  d.ps=shader("float4 main():SV_Target0{return float4(.5,.25,.125,.02);}",gpu::ShaderStage::kPixel);
  d.color_count=1;d.color_formats[0]=td.format;d.color_write_masks[0]=15;d.depth_clip=false;
  d.color_blend[0].enable=true;d.color_blend[0].src=gpu::BlendFactor::kSrcAlpha;d.color_blend[0].dst=gpu::BlendFactor::kInvSrcAlpha;
  const auto overlay=backend.GetGraphicsPipeline(d);d.color_blend[0].enable=false;
  d.ps=shader("float4 main():SV_Target0{return float4(8,4,2,1);}",gpu::ShaderStage::kPixel);
  const auto background=backend.GetGraphicsPipeline(d);if(!upstream||!direct||!overlay||!background)return 80;
  backend.SetViewport({0,0,float(w),float(h),0,1});backend.SetScissor({0,0,int(w),int(h)});
  auto run=[&](bool revised,unsigned iterations) {
    const auto target=revised?direct:upstream;
    for(unsigned f=0;f<iterations;++f) {
      backend.SetRenderTargets(1,&target,0);backend.SetPipeline(background);backend.Draw(3);
      backend.SetPipeline(overlay);for(unsigned i=0;i<draws;++i)backend.Draw(3);
    }
    backend.WaitForFence(backend.Submit());backend.CompletedFence();
  };
  backend.SetGpuTag(0);run(false,1);run(true,1);
  const auto a=backend.ReadbackTexture(upstream,{0,0,1,1}),b=backend.ReadbackTexture(direct,{0,0,1,1});
  backend.WaitForFence(backend.Submit());uint32_t pa=0,pb=0;const auto* da=backend.ReadbackData(a,pa);const auto* db=backend.ReadbackData(b,pb);
  const bool same=da&&db&&!std::memcmp(da,db,8);backend.ReleaseReadback(a);backend.ReleaseReadback(b);if(!same)return 81;
  std::vector<gpu::GpuTagTime> times;backend.TakeGpuTagTimes(times);
  for(unsigned trial=0;trial<6;++trial)for(unsigned kind=0;kind<2;++kind){const bool revised=((trial+kind)&1)!=0;backend.SetGpuTag(trial*2+(revised?2:1));run(revised,frames);}
  backend.SetGpuTag(0);run(true,1);run(true,1);backend.TakeGpuTagTimes(times);
  double batches[13]{};for(const auto& t:times)if(t.tag>0&&t.tag<=12)batches[t.tag]+=t.ms/frames;
  std::vector<double> before,after;for(unsigned t=1;t<=12;++t){if(batches[t]<=0)return 82;(t%2?before:after).push_back(batches[t]);}
  std::sort(before.begin(),before.end());std::sort(after.begin(),after.end());
  std::printf(
      "PASS 4K UI target-shape benchmark: 35 overlays, identical pixels, zero preservation copies "
      "in both paths; median GPU visible %.3f ms, padded %.3f ms. Additional UI coverage and "
      "gameplay FPS are not measured by this test.\n",before[3],after[3]);return 0;
}
static int AsyncUiCacheCheck(gpu::Backend& backend,xenos::renderer::ShaderCompiler& compiler,const char* path,bool sceneFilter=false) {
  std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)return 85;
  const size_t bytes=size_t(file.tellg());if(!bytes||bytes%4)return 85;
  std::vector<uint32_t> code(bytes/4);file.seekg(0);file.read(reinterpret_cast<char*>(code.data()),bytes);if(!file)return 85;
  struct NoHlslCompiler final:xenos::renderer::ShaderCompiler {
    std::atomic<unsigned>& calls;explicit NoHlslCompiler(std::atomic<unsigned>& c):calls(c){}
    std::string Describe() const override{return "native variant queue test";}
    std::string CacheTag(gpu::ShaderIL) const override{return "native-ui-test";}
    bool Compile(const std::string&,gpu::ShaderStage,gpu::ShaderIL,std::vector<uint8_t>&,std::string&)override{++calls;return false;}
  };
  std::atomic<unsigned> hlslCalls=0;
  xenos::renderer::ShaderCache cache(backend,[&]{return std::make_unique<NoHlslCompiler>(hlslCalls);},{},2);
  cache.SetDxbc(true);using Mode=hdr::OverlayMode;
  const uint64_t hash=sceneFilter?(code.size()==96?0x8356D7C3D65FE328ull:0x212F89E82422D29Dull):0x1234;
  const auto mode=sceneFilter?Mode::kFilterLinear:Mode::kUi;
  const auto& ui=cache.GetNativeOverlay(hash,code.data(),code.size(),mode);
  if(ui.state!=xenos::renderer::ShaderState::kPending||cache.PendingCount()!=1)return 86;
  const auto& repeated=cache.GetNativeOverlay(hash,code.data(),code.size(),mode);
  if(&ui!=&repeated||cache.PendingCount()!=1)return 86;
  const auto& premult=cache.GetNativeOverlay(sceneFilter?hash:0x1234,code.data(),code.size(),
      sceneFilter?Mode::kFilterPremultiplied:Mode::kPremultiplied);
  if(&premult==&ui||cache.PendingCount()!=2)return 86;
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while((!ui.ready()||!premult.ready()) && std::chrono::steady_clock::now()<deadline) {
    cache.WaitReady(ui,50);cache.WaitReady(premult,50);
  }
  if(!ui.ready()||!premult.ready()||ui.handle==premult.handle||hlslCalls)return 87;
  gpu::GraphicsPipelineDesc d;d.vs=OverlayFixtureVertex(backend,compiler,true);d.color_count=1;
  d.color_formats[0]=gpu::Format::kR16G16B16A16Float;d.color_write_masks[0]=15;d.depth_clip=false;
  d.ps=ui.handle;if(!backend.GetGraphicsPipeline(d))return 88;
  d.ps=premult.handle;if(!backend.GetGraphicsPipeline(d))return 88;
  if(sceneFilter) {
    std::vector<uint8_t> geometry;std::string error;
    if(backend.GetShaderIL()==gpu::ShaderIL::kDxil)geometry=xenos::dxbc_gen::RectListGeometryShader();
    else if(!compiler.Compile(xenos::renderer::builtin::kRectListGs,gpu::ShaderStage::kGeometry,backend.GetShaderIL(),geometry,error))return 88;
    d.gs=backend.CreateShader(gpu::ShaderStage::kGeometry,geometry.data(),geometry.size());
    if(!d.gs || !backend.GetGraphicsPipeline(d))return 88;
    d.ps=ui.handle;if(!backend.GetGraphicsPipeline(d))return 88;
  }
  if(cache.PendingCount()||!cache.GetNativeOverlay(hash,code.data(),code.size(),mode).ready())return 89;
  if(sceneFilter && cache.GetNativeOverlay(hash^1,code.data(),code.size(),mode).state!=xenos::renderer::ShaderState::kFailed)return 89;
  std::puts(
      "PASS asynchronous UI variants: caller returns pending, duplicate requests cached, modes "
      "independent, worker translation without DXC, real pipelines ready");return 0;
}
