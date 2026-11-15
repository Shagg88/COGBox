// Game-free checks of the production resource transport shaders.
#pragma once
static int HdrTransportCheck(gpu::Backend& backend,xenos::renderer::ShaderCompiler& compiler) {
  auto shader=[&](const char* source,gpu::ShaderStage stage) {
    std::vector<uint8_t> bytes;std::string error;
    if(!compiler.Compile(source,stage,backend.GetShaderIL(),bytes,error)) {
      std::printf("FAIL HDR transport compile: %s\n",error.c_str());return gpu::ShaderHandle(0);
    }
    return backend.CreateShader(stage,bytes.data(),bytes.size());
  };
  auto compute=[&](const char* source) {
    const auto s=shader(source,gpu::ShaderStage::kCompute);return s?backend.CreateComputePipeline(s):0;
  };
  const auto resolve=compute(hdr::kResolveCompanionCs),view=compute(hdr::kResolvedViewCs),seed=compute(hdr::kSeedReferenceCs);
  if(!resolve||!view||!seed)return 80;
  auto texture=[&](unsigned w,unsigned h,gpu::Format format) {
    gpu::TextureDesc d;d.width=w;d.height=h;d.format=format;
    d.usage=gpu::kUsageSampled|gpu::kUsageStorage|gpu::kUsageRenderTarget;
    return backend.CreateTexture(d);
  };
  const auto source=texture(4,2,gpu::Format::kR16G16B16A16Float);
  const auto snapshot=texture(8,4,gpu::Format::kR16G16B16A16Float);
  const auto reference=texture(8,4,gpu::Format::kR8G8B8A8Unorm);
  const auto output=texture(8,4,gpu::Format::kR16G16B16A16Float);
  if(!source||!snapshot||!reference||!output)return 81;
  uint16_t pixels[8*4]{};uint8_t original[32*4]{};
  for(unsigned i=0;i<8;++i)for(unsigned c=0;c<4;++c)pixels[i*4+c]=ToHalf(c==3?.3f:float(4+i+c));
  for(unsigned i=0;i<32;++i)for(unsigned c=0;c<4;++c)original[i*4+c]=c==3?128:uint8_t(64+i+c*16);
  backend.UploadTexture(source,0,0,pixels,4*8,2,1);
  backend.UploadTexture(reference,0,0,original,8*4,4,1);
  const float zero[4]{};backend.ClearColor(snapshot,zero);
  gpu::ComputeBindings b;
  b.constants[0]=4;b.constants[1]=2;b.constants[2]=2;b.constants[3]=0;
  b.constants[4]=1;b.constants[5]=0;b.constants[6]=1;b.constants[7]=2;
  b.constants[8]=4;b.constants[9]=2;b.constants[10]=1;b.constants[12]=std::bit_cast<uint32_t>(1.f);
  b.constant_count=16;b.srv[0]=source;b.uav_texture=snapshot;
  backend.Dispatch(resolve,b,1,1,1);
  // Partial overwrite from a non-HDR source invalidates just the changed pixels.
  b.constants[0]=2;b.constants[2]=4;b.constants[10]=0;b.srv[0]=0;
  backend.Dispatch(resolve,b,1,1,1);
  // A later write to the original target cannot mutate the resolved snapshot.
  const float changed[4]={64,32,16,1};backend.ClearColor(source,changed);
  b={};b.constants[0]=8;b.constants[1]=4;b.constants[2]=b.constants[3]=2;b.constants[5]=0x688;
  b.constants[8]=std::bit_cast<uint32_t>(1.f);b.constants[12]=8;b.constants[13]=4;b.constant_count=16;
  b.srv[0]=snapshot;b.srv[1]=reference;b.uav_texture=output;
  backend.Dispatch(view,b,1,1,1);
  auto read=[&](gpu::TextureHandle t,unsigned w,unsigned h) {
    const auto rb=backend.ReadbackTexture(t,{0,0,int32_t(w),int32_t(h)});
    backend.WaitForFence(backend.Submit());uint32_t pitch=0;const auto* bytes=backend.ReadbackData(rb,pitch);
    std::vector<float> values;
    if(bytes) {values.resize(w*h*4);for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w*4;++x) {
      uint16_t half;std::memcpy(&half,bytes+y*pitch+x*2,2);values[(y*w*4)+x]=Half(half);
    }}
    backend.ReleaseReadback(rb);return values;
  };
  auto actual=read(output,8,4);if(actual.empty())return 82;
  for(unsigned y=0;y<4;++y)for(unsigned x=0;x<8;++x)for(unsigned c=0;c<4;++c) {
    const unsigned i=y*8+x;
    const float expected=c==3?128.f/255.f:(y<2 && x>=2 && x<4)?float(5+c):std::pow(original[i*4+c]/255.f,2.2f);
    if(std::abs(actual[i*4+c]-expected)>.008f) {
      std::printf("FAIL HDR partial resolve %u,%u channel %u: %g != %g\n",x,y,c,actual[i*4+c],expected);return 83;
    }
  }
  // Synchronising an SDR clear updates only that rectangle of an existing HDR target.
  backend.ClearColor(output,changed);
  b={};b.constants[0]=2;b.constants[1]=1;b.constants[2]=3;b.constants[3]=2;b.constant_count=4;
  b.srv[0]=reference;b.uav_texture=output;backend.Dispatch(seed,b,1,1,1);
  actual=read(output,8,4);if(actual.empty())return 82;
  for(unsigned y=0;y<4;++y)for(unsigned x=0;x<8;++x) {
    const unsigned i=y*8+x;
    const float expected=y==2 && x>=3 && x<5?std::pow(original[i*4]/255.f,2.2f):64.f;
    if(std::abs(actual[i*4]-expected)>.01f)return 84;
  }
  // Linear gain/channel swap is applied before publishing a new version.
  b={};b.constants[0]=2;b.constants[1]=2;b.constants[6]=1;b.constants[7]=2;
  b.constants[8]=4;b.constants[9]=2;b.constants[10]=1;
  b.constants[12]=std::bit_cast<uint32_t>(2.f);b.constants[13]=std::bit_cast<uint32_t>(1.f);
  b.constant_count=16;b.srv[0]=source;b.uav_texture=snapshot;backend.Dispatch(resolve,b,1,1,1);
  actual=read(snapshot,8,4);if(actual.empty())return 82;
  if(actual[0]!=32 || actual[1]!=64 || actual[2]!=128 || actual[3]!=1 || actual[8]!=5 || actual[11]!=1)return 85;
  const auto packed=texture(4,2,gpu::Format::kR32Uint),presentView=texture(4,2,gpu::Format::kR16G16B16A16Float);
  uint32_t packedWords[8];std::fill_n(packedWords,8,512u|(256u<<10)|(768u<<20));
  backend.UploadTexture(packed,0,0,packedWords,16,2,1);
  b={};b.constants[0]=4;b.constants[1]=2;b.constants[2]=2;b.constants[3]=1;b.constants[4]=1;b.constants[5]=0x688;
  b.constants[8]=std::bit_cast<uint32_t>(1.f);b.constants[12]=8;b.constants[13]=4;b.constant_count=16;
  b.srv[0]=snapshot;b.srv[2]=packed;b.uav_texture=presentView;backend.Dispatch(view,b,1,1,1);
  actual=read(presentView,4,2);if(actual.empty())return 82;
  for(unsigned y=0;y<2;++y)for(unsigned x=0;x<4;++x) {
    const float expected=y==0&&x<2?(x==0?32.f:5.f):std::pow(512.f/1023.f,2.2f);
    if(std::abs(actual[(y*4+x)*4]-expected)>.01f || actual[(y*4+x)*4+3]!=1)return 92;
  }
  // Gears 3's actual BGR fetch cancels the resolve's red/blue storage swap.
  // HDR pixels must follow it; already-decoded reference gaps must not.
  b.constants[5]=hdr::PresentCompanionSwizzle(0xA0A,0,0);
  backend.Dispatch(view,b,1,1,1);actual=read(presentView,4,2);
  if(actual.empty() || actual[0]!=128 || actual[1]!=64 || actual[2]!=32 ||
      std::abs(actual[8]-std::pow(512.f/1023.f,2.2f))>.01f ||
      std::abs(actual[9]-std::pow(256.f/1023.f,2.2f))>.01f)return 93;
  const auto fused=compute(hdr::kSceneProxyCs);if(!fused)return 102;
  const auto stored=read(snapshot,8,4);
  b={};b.constants[0]=8;b.constants[1]=4;b.constants[2]=b.constants[3]=2;
  b.constants[5]=1;b.constants[6]=8;b.constants[7]=4;b.constants[8]=std::bit_cast<uint32_t>(.5f);b.constant_count=12;
  b.srv[0]=snapshot;b.srv[1]=reference;b.uav_texture=output;backend.Dispatch(fused,b,1,1,1);
  actual=read(output,8,4);if(actual.empty()||stored.empty())return 82;
  for(unsigned i=0;i<32;++i)for(unsigned c=0;c<4;++c) {
    const float expected=c==3?128.f/255.f:stored[i*4+3]>.5f?
        std::pow(stored[i*4+2-c]*.5f,1.f/2.2f):original[i*4+c]/255.f;
    if(std::abs(actual[i*4+c]-expected)>.01f)return 103;
  }
  // Offset background/composition work must not touch other menu pixels.
  const auto highRegion=texture(8,4,gpu::Format::kR16G16B16A16Float),lowRegion=texture(8,4,gpu::Format::kR16G16B16A16Float);
  const auto backgroundPipe=compute(hdr::kPrepareSceneOverlayCs),mixPipe=compute(hdr::kSceneUiBlendCs);
  backend.ClearColor(highRegion,zero);backend.ClearColor(lowRegion,zero);backend.ClearColor(output,changed);
  b={};b.constants[0]=2;b.constants[1]=1;b.constants[2]=3;b.constants[3]=2;b.constant_count=4;
  b.srv[0]=output;b.srv[1]=reference;b.uav_texture=highRegion;b.uav2_texture=lowRegion;
  backend.Dispatch(backgroundPipe,b,1,1,1);actual=read(highRegion,8,4);
  if(actual.empty()||actual[0]!=0 || std::abs(actual[(2*8+3)*4]-std::pow(64.f,1.f/2.2f))>.02f)return 104;
  b.constants[4]=std::bit_cast<uint32_t>(20.f);b.constant_count=8;
  b.srv[0]=b.srv[1]=highRegion;b.srv[2]=reference;b.uav_texture=output;b.uav2_texture=0;
  backend.Dispatch(mixPipe,b,1,1,1);actual=read(output,8,4);if(actual.empty())return 82;
  for(unsigned y=0;y<4;++y)for(unsigned x=0;x<8;++x) {
    const unsigned i=y*8+x;const float expected=y==2&&x>=3&&x<5?std::pow(original[i*4]/255.f,2.2f):64.f;
    if(std::abs(actual[i*4]-expected)>.01f)return 105;
  }
  // Actual copy-on-write pool: a full resolve pins without a dispatch. A later
  // target mutation copies once and cannot alter the old resolved version.
  hdr::CompanionPool pool;
  auto targetImage=pool.Acquire(backend,4,4,1);targetImage.height=1;
  const float asymmetric[]={8,4,2,.1f},poison[]={64,0,64,1};backend.ClearColor(targetImage.texture,poison);
  uint16_t visiblePixels[16];for(unsigned i=0;i<16;++i)visiblePixels[i]=ToHalf(asymmetric[i%4]);
  backend.UploadTexture(targetImage.texture,0,0,visiblePixels,32,1,1);
  auto pinned=targetImage;
  const auto copy=compute(hdr::kCopyForUiCs);
  if(!hdr::WritableCompanion(backend,pool,copy,targetImage,1,true) || targetImage.texture==pinned.texture ||
      targetImage.allocated_height!=4 || targetImage.height!=1)return 94;
  backend.ClearColor(targetImage.texture,changed);
  auto pinnedValues=read(pinned.texture,4,1);
  if(pinnedValues.empty() || pinnedValues[0]!=8 || pinnedValues[2]!=2)return 95;
  // Whole snapshots use explicit coverage, never the game's (possibly zero)
  // alpha as a validity mask. Allocated padding is outside the known extent.
  b={};b.constants[0]=8;b.constants[1]=4;b.constants[2]=b.constants[3]=1;
  b.constants[5]=0x688;b.constants[6]=1;b.constants[8]=std::bit_cast<uint32_t>(1.f);
  b.constants[12]=4;b.constants[13]=1;b.constant_count=16;
  b.srv[0]=pinned.texture;b.srv[1]=reference;b.uav_texture=output;backend.Dispatch(view,b,1,1,1);
  actual=read(output,8,4);
  if(actual.empty() || actual[0]!=8 || std::abs(actual[8*4]-std::pow(original[8*4]/255.f,2.2f))>.01f)return 96;
  const auto weights=texture(4,1,gpu::Format::kR8G8B8A8Unorm),aaOutput=texture(4,1,gpu::Format::kR16G16B16A16Float);
  backend.ClearColor(weights,zero);
  gpu::GraphicsPipelineDesc aa;
  aa.vs=shader(smaa::Source(smaa::kBlendVs).c_str(),gpu::ShaderStage::kVertex);
  aa.ps=shader(smaa::NativeHdrBlendSource().c_str(),gpu::ShaderStage::kPixel);
  aa.depth_clip=false;aa.color_count=1;aa.color_formats[0]=gpu::Format::kR16G16B16A16Float;aa.color_write_masks[0]=15;
  const auto aaPipeline=aa.vs&&aa.ps?backend.GetGraphicsPipeline(aa):0;if(!aaPipeline)return 100;
  gpu::TextureView aaViews[2];aaViews[0].texture=pinned.texture;aaViews[1].texture=weights;
  gpu::SamplerDesc aaSamplers[2];aaSamplers[0].min_linear=aaSamplers[0].mag_linear=1;
  backend.SetTextures(aaViews,aaSamplers,2,2);const float metrics[]={.25f,1.f,4.f,1.f};
  backend.SetConstants(gpu::ShaderStage::kVertex,0,metrics,sizeof(metrics));
  backend.SetConstants(gpu::ShaderStage::kPixel,0,metrics,sizeof(metrics));
  backend.SetRenderTargets(1,&aaOutput,0);backend.SetViewport({0,0,4,1,0,1});backend.SetScissor({0,0,4,1});
  backend.SetPipeline(aaPipeline);backend.Draw(3);actual=read(aaOutput,4,1);
  if(actual.empty() || actual[0]!=8 || actual[1]!=4 || actual[2]!=2)return 101;
  const auto liveBytes=pool.Bytes();pool.Collect(backend,10,0);
  if(pool.Bytes()!=liveBytes)return 97; // pinned/live allocations cannot be collected
  pinned={};targetImage={};pool.Collect(backend,10,0);if(pool.Bytes()!=0)return 97;
  auto warmA=pool.Acquire(backend,4,4,11),warmB=pool.Acquire(backend,4,4,11);
  const auto allocations=pool.Created();warmA={};warmB={};pool.Collect(backend,11,64);
  if(pool.Bytes()!=128)return 106; // keep one spare despite the soft budget
  warmA=pool.Acquire(backend,4,4,12);
  if(pool.Created()!=allocations || !pool.Reused())return 106;
  warmA={};pool.Collect(backend,200,64);if(pool.Bytes()!=0)return 106;
  // Matching colour aliases keep HDR; uncovered source rows/columns use the
  // original source rather than zero or another allocation's old HDR image.
  const auto aliasSource=texture(40,16,gpu::Format::kR16G16B16A16Float);
  const auto aliasReference=texture(80,16,gpu::Format::kR8G8B8A8Unorm);
  const auto aliasOutput=texture(80,16,gpu::Format::kR16G16B16A16Float);
  const float hdrColor[]={8,4,2,.5f},sdrColor[]={.5f,.25f,.125f,.75f};
  backend.ClearColor(aliasSource,hdrColor);backend.ClearColor(aliasReference,sdrColor);
  gpu::GraphicsPipelineDesc desc;desc.vs=shader(xenos::renderer::builtin::kTransferVs,gpu::ShaderStage::kVertex);
  desc.ps=shader(hdr::kTransferCompanionPs,gpu::ShaderStage::kPixel);desc.depth_clip=false;
  desc.color_count=1;desc.color_formats[0]=gpu::Format::kR16G16B16A16Float;desc.color_write_masks[0]=15;
  const auto pipeline=desc.vs&&desc.ps?backend.GetGraphicsPipeline(desc):0;
  if(!aliasSource||!aliasReference||!aliasOutput||!pipeline) {
    std::printf("FAIL alias resources source %u reference %u output %u VS %u PS %u pipeline %u\n",
        aliasSource,aliasReference,aliasOutput,desc.vs,desc.ps,pipeline);
#ifdef _WIN32
    void* device=nullptr;void* list=nullptr;
    if(backend.BeginNative(&device,&list)) {
      ID3D12InfoQueue* queue=nullptr;
      if(SUCCEEDED(static_cast<ID3D12Device*>(device)->QueryInterface(IID_PPV_ARGS(&queue)))) {
        for(UINT64 i=0;i<queue->GetNumStoredMessages();++i) {
          SIZE_T size=0;queue->GetMessage(i,nullptr,&size);std::vector<uint8_t> data(size);
          auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());
          if(SUCCEEDED(queue->GetMessage(i,message,&size)))std::printf("D3D12: %s\n",message->pDescription);
        }
        queue->Release();
      }
      backend.EndNative();
    }
#endif
    return 86;
  }
  gpu::TextureView views[2];views[0].texture=aliasSource;views[1].texture=aliasReference;
  gpu::SamplerDesc samplers[1];backend.SetTextures(views,samplers,2,1);
  backend.SetRenderTargets(1,&aliasOutput,0);backend.SetViewport({0,0,80,16,0,1});backend.SetScissor({0,0,80,16});
  uint32_t constants[16]={0,1,0,1,80,0,0,0,0,0,40,16,1,1};
  backend.SetConstants(gpu::ShaderStage::kPixel,0,constants,sizeof(constants));backend.SetPipeline(pipeline);backend.Draw(3);
  actual=read(aliasOutput,80,16);if(actual.empty())return 82;
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<80;++x) {
    const float expected=x<40?8.f:std::pow(128.f/255.f,2.2f);
    if(std::abs(actual[(y*80+x)*4]-expected)>.01f)return 87;
  }
  // Two unrelated slots, both with HDR, travel through the real translator.
  // The authored upper RGB clamp extends only in the HDR evaluation. Kill and
  // alpha decisions must still use the bounded original input.
  namespace uc=xenos::ucode;
  uc::Program program;program.is_vertex_shader=false;
  uc::Instruction f0;f0.kind=uc::Instruction::Kind::kTextureFetch;
  f0.tfetch.dimension=uc::FetchDimension::k2D;
  f0.tfetch.has_result=f0.tfetch.has_fetch_constant=f0.tfetch.has_attributes=true;
  f0.tfetch.result.target=uc::Target::kRegister;f0.tfetch.result.write_mask=15;
  auto f5=f0;f5.tfetch.fetch_constant=5;f5.tfetch.result.index=1;
  uc::Instruction kill;kill.alu.vector_opcode=uc::AluVectorOpcode::kKillGt;kill.alu.vector_operand_count=2;
  kill.alu.vector_operands[1].source=uc::Source::kConstantFloat;
  uc::Instruction sum;sum.alu.vector_opcode=uc::AluVectorOpcode::kAdd;sum.alu.vector_operand_count=2;
  sum.alu.vector_operands[1].index=1;
  sum.alu.vector_result.target=uc::Target::kColor;sum.alu.vector_result.write_mask=15;sum.alu.vector_result.clamp=true;
  uc::ControlFlow cf;cf.opcode=uc::CfOpcode::kExecEnd;cf.exec_end=true;
  cf.instructions={f0,f5,kill,sum};cf.exec_count=4;program.control_flow.push_back(cf);
  hdr::ExtendSceneClamps(program,33);
  xenos::hlsl::HlslOptions options;options.entry="xe_scene_ui_material";
  const auto pair=hdr::TransportScenePairSource(xenos::hlsl::TranslateToHlsl(program,options).hlsl,33,33);
  if(pair.empty())return 88;
  desc={};desc.vs=OverlayFixtureVertex(backend,compiler,false);desc.ps=shader(pair.c_str(),gpu::ShaderStage::kPixel);
  desc.color_count=2;desc.depth_clip=false;
  for(unsigned i=0;i<2;++i){desc.color_formats[i]=gpu::Format::kR16G16B16A16Float;desc.color_write_masks[i]=15;}
  const auto pairPipeline=desc.vs&&desc.ps?backend.GetGraphicsPipeline(desc):0;
  const auto bounded=texture(1,1,gpu::Format::kR8G8B8A8Unorm);
  const auto proxy0=texture(1,1,gpu::Format::kR16G16B16A16Float),proxy5=texture(1,1,gpu::Format::kR16G16B16A16Float);
  const auto high=texture(1,1,gpu::Format::kR16G16B16A16Float),low=texture(1,1,gpu::Format::kR16G16B16A16Float);
  if(!pairPipeline||!bounded||!proxy0||!proxy5||!high||!low)return 89;
  const float boundedColor[]={.75f,.75f,.75f,.25f},proxyColor0[]={4,4,4,.25f},proxyColor5[]={2,2,2,.25f};
  backend.ClearColor(bounded,boundedColor);backend.ClearColor(proxy0,proxyColor0);backend.ClearColor(proxy5,proxyColor5);
  gpu::TextureView inputs[32];inputs[0].texture=inputs[5].texture=bounded;
  inputs[31].texture=proxy0;inputs[30].texture=proxy5;gpu::SamplerDesc sample[32];
  backend.SetTextures(inputs,sample,32,32);
  uint32_t system[6][4]{};system[0][2]=1;for(auto& c:system[3])c=std::bit_cast<uint32_t>(1.f);
  uint32_t fetches[192]{};float threshold[]={.8f,.8f,.8f,.8f};
  backend.SetConstants(gpu::ShaderStage::kPixel,0,system,sizeof(system));
  backend.SetConstants(gpu::ShaderStage::kPixel,1,threshold,sizeof(threshold));
  backend.SetConstants(gpu::ShaderStage::kPixel,3,fetches,sizeof(fetches));
  const gpu::TextureHandle targets[]={high,low};backend.SetRenderTargets(2,targets,0);
  backend.SetViewport({0,0,1,1,0,1});backend.SetScissor({0,0,1,1});backend.SetPipeline(pairPipeline);backend.Draw(3);
  const auto highValues=read(high,1,1),lowValues=read(low,1,1);
  if(highValues.empty()||lowValues.empty())return 82;
  if(std::abs(highValues[0]-6)>.01f || lowValues[0]!=1 || std::abs(highValues[3]-128.f/255.f)>.001f ||
      highValues[3]!=lowValues[3]) {
    std::printf("FAIL HDR multiple inputs/clamp/coverage: high %g alpha %g low %g alpha %g\n",
        highValues[0],highValues[3],lowValues[0],lowValues[3]);return 90;
  }
  backend.ClearColor(high,changed);backend.ClearColor(low,changed);
  std::fill_n(threshold,4,.5f);backend.SetConstants(gpu::ShaderStage::kPixel,1,threshold,sizeof(threshold));
  backend.SetRenderTargets(2,targets,0);backend.Draw(3);
  const auto discarded=read(high,1,1);if(discarded.empty()||discarded[0]!=64) {
    std::printf("FAIL bounded discard: %g\n",discarded.empty()?-1.f:discarded[0]);return 91;
  }
  std::puts("PASS HDR transport: frozen offset/scaled snapshots, partial SDR overwrite, authoritative gaps/alpha, regional clears, linear gain/channel swap, EDRAM aliases, multiple HDR inputs, RGB clamp extension, bounded kill/alpha");
  return 0;
}

static int HdrTransportBench(gpu::Backend& backend,xenos::renderer::ShaderCompiler& compiler) {
  auto shader=[&](const char* source,gpu::ShaderStage stage) {
    std::vector<uint8_t> bytes;std::string error;
    if(!compiler.Compile(source,stage,backend.GetShaderIL(),bytes,error))return gpu::ShaderHandle(0);
    return backend.CreateShader(stage,bytes.data(),bytes.size());
  };
  constexpr unsigned w=3840,h=2160,padded=6144,draws=35,iterations=3;
  const auto cs=shader(hdr::kCopyForUiCs,gpu::ShaderStage::kCompute);
  const auto copy=cs?backend.CreateComputePipeline(cs):0;
  hdr::CompanionPool pool;
  auto oldScene=pool.Acquire(backend,w,h,0),oldReplay=pool.Acquire(backend,w,padded,0),oldResolve=pool.Acquire(backend,w,h,0);
  auto direct=pool.Acquire(backend,w,padded,0);direct.height=h;
  gpu::GraphicsPipelineDesc desc;desc.vs=shader(xenos::renderer::builtin::kTransferVs,gpu::ShaderStage::kVertex);
  desc.ps=shader("float4 main():SV_Target0{return float4(.5,.25,.125,.02);}",gpu::ShaderStage::kPixel);
  desc.color_count=1;desc.color_formats[0]=gpu::Format::kR16G16B16A16Float;desc.color_write_masks[0]=15;desc.depth_clip=false;
  desc.color_blend[0].enable=true;desc.color_blend[0].src=gpu::BlendFactor::kSrcAlpha;desc.color_blend[0].dst=gpu::BlendFactor::kInvSrcAlpha;
  const auto pipeline=desc.vs&&desc.ps?backend.GetGraphicsPipeline(desc):0;
  if(!copy||!pipeline||!oldScene.texture||!oldReplay.texture||!oldResolve.texture||!direct.texture)return 98;
  const float background[]={8,4,2,1};backend.ClearColor(oldScene.texture,background);backend.ClearColor(direct.texture,background);
  auto run=[&](bool revised) {
    for(unsigned frame=0;frame<iterations;++frame) {
      auto pinned=direct;
      if(revised && !hdr::WritableCompanion(backend,pool,copy,direct,frame,true))return false;
      for(unsigned draw=0;draw<draws;++draw) {
        if(!revised)hdr::StencilReplayTarget::Copy(backend,copy,oldScene.texture,oldReplay.texture,w,h);
        const auto target=revised?direct.texture:oldReplay.texture;
        backend.SetRenderTargets(1,&target,0);backend.SetViewport({0,0,float(w),float(h),0,1});
        backend.SetScissor({0,0,int32_t(w),int32_t(h)});backend.SetPipeline(pipeline);backend.Draw(3);
        if(!revised)hdr::StencilReplayTarget::Copy(backend,copy,oldReplay.texture,oldScene.texture,w,h);
      }
      if(!revised)for(unsigned resolve=0;resolve<3;++resolve)
        hdr::StencilReplayTarget::Copy(backend,copy,oldScene.texture,oldResolve.texture,w,h);
    }
    backend.WaitForFence(backend.Submit());return true;
  };
  backend.SetGpuTag(0);run(false);run(true);
  std::vector<gpu::GpuTagTime> timings;backend.TakeGpuTagTimes(timings);
  for(unsigned trial=0;trial<4;++trial)for(unsigned kind=0;kind<2;++kind) {
    const bool revised=((trial+kind)&1)!=0;backend.SetGpuTag(trial*2+(revised?2:1));
    if(!run(revised))return 98;
  }
  backend.SetGpuTag(0);run(true);run(true);backend.TakeGpuTagTimes(timings);
  double batches[9]{};for(const auto& time:timings)if(time.tag>0&&time.tag<=8)batches[time.tag]+=time.ms/iterations;
  std::vector<double> before,after;
  for(unsigned tag=1;tag<=8;++tag){if(batches[tag]<=0)return 99;(tag%2?before:after).push_back(batches[tag]);}
  std::sort(before.begin(),before.end());std::sort(after.begin(),after.end());
  std::printf("PASS synthetic 4K transport, 35 overlays: median GPU %.3f -> %.3f ms; per frame 73 preservation copies -> 1 copy-on-write. This is not a gameplay FPS measurement.\n",before[2],after[2]);
  return 0;
}
