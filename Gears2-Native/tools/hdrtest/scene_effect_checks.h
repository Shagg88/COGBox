// Authored GPU tests, no game shader data. MIT.
#pragma once
#include <xenos/renderer/hdr_scene_effect.h>
static xenos::ucode::Program SceneFilterFixture() {
  namespace u=xenos::ucode;
  u::Program p;p.is_vertex_shader=false;
  u::ControlFlow cf;cf.opcode=u::CfOpcode::kExecEnd;cf.exec_end=true;
  for(unsigned i=0;i<2;++i) {
    u::Instruction in;in.kind=u::Instruction::Kind::kTextureFetch;
    auto& f=in.tfetch;f.dimension=u::FetchDimension::k2D;f.has_result=f.has_fetch_constant=true;
    f.result.target=u::Target::kRegister;f.result.index=i+1;f.result.write_mask=15;
    f.coordinates.source=u::Source::kConstantFloat;f.coordinates.index=3+i;f.coordinates.component_count=2;
    f.use_computed_lod=false;cf.instructions.push_back(in);
  }
  u::Instruction mul;auto& a=mul.alu;a.vector_opcode=u::AluVectorOpcode::kMul;
  a.vector_result.target=u::Target::kRegister;a.vector_result.index=2;a.vector_result.write_mask=15;
  a.vector_operand_count=2;a.vector_operands[0].index=2;
  a.vector_operands[1].source=u::Source::kConstantFloat;a.vector_operands[1].index=1;
  cf.instructions.push_back(mul);
  u::Instruction mad;auto& m=mad.alu;m.vector_opcode=u::AluVectorOpcode::kMad;
  m.vector_result.target=u::Target::kRegister;m.vector_result.index=0;m.vector_result.write_mask=7;m.vector_result.clamp=true;
  m.vector_operand_count=3;m.vector_operands[0].index=1;
  m.vector_operands[1].source=u::Source::kConstantFloat;m.vector_operands[1].index=1;m.vector_operands[2].index=2;
  cf.instructions.push_back(mad);
  u::Instruction out;auto& o=out.alu;o.vector_opcode=u::AluVectorOpcode::kMax;
  o.vector_result.target=u::Target::kColor;o.vector_result.write_mask=7;o.vector_operand_count=2;
  cf.instructions.push_back(out);
  o.vector_result.write_mask=8;for(auto& op:o.vector_operands)op.source=u::Source::kConstantFloat;
  cf.instructions.push_back(out);
  cf.exec_count=uint32_t(cf.instructions.size());p.control_flow.push_back(cf);return p;
}
static int SceneEffectCheck(gpu::Backend& backend,xenos::renderer::ShaderCompiler& compiler) {
  std::vector<uint8_t> bytes;std::string error;
  if(!compiler.Compile(hdr::kEncodeSceneForEffects,gpu::ShaderStage::kCompute,backend.GetShaderIL(),bytes,error))return 90;
  const auto cs=backend.CreateShader(gpu::ShaderStage::kCompute,bytes.data(),bytes.size());
  const auto encode=backend.CreateComputePipeline(cs);if(!encode)return 90;
  gpu::TextureDesc td;td.width=2;td.height=4;td.format=gpu::Format::kR16G16B16A16Float;
  td.usage=gpu::kUsageStorage|gpu::kUsageSampled|gpu::kUsageRenderTarget;
  const auto scene=backend.CreateTexture(td);td.height=1;const auto encoded=backend.CreateTexture(td);
  const auto output=backend.CreateTexture(td);if(!scene||!encoded||!output)return 91;
  uint16_t source[32];std::fill_n(source,32,ToHalf(60000));
  const float colors[8]={16,4,2,.25f,4,1,.5f,.75f};
  for(unsigned i=0;i<8;++i)source[i]=ToHalf(colors[i]);
  backend.UploadTexture(scene,0,0,source,16,4,1);
  gpu::ComputeBindings cb;cb.srv[0]=scene;cb.uav_texture=encoded;backend.Dispatch(encode,cb,1,1,1);
  auto rb=backend.ReadbackTexture(encoded,{0,0,2,1});backend.WaitForFence(backend.Submit());
  uint32_t pitch=0;const auto* raw=backend.ReadbackData(rb,pitch);
  uint16_t packed[8]{};if(raw)std::memcpy(packed,raw,16);backend.ReleaseReadback(rb);if(!raw)return 92;
  for(unsigned i=0;i<8;++i) {
    const float expected=i%4==3?colors[i]:std::pow(colors[i],1.f/2.2f);
    if(std::abs(Half(packed[i])-expected)>.004f)return 93;
  }
  for(bool dxbc:{true,false})for(bool hdrOutput:{false,true})for(bool swap:{false,true})for(bool premult:{false,true})for(float sourceAlpha:{0.f,.375f,1.f}) {
    xenos::hlsl::HlslOptions options;options.native_hdr_filter=hdrOutput;options.native_hdr_linear=hdrOutput;
    options.native_hdr_premultiplied=premult;
    const auto program=SceneFilterFixture();bytes.clear();
    if(dxbc) {
      auto translated=xenos::dxbc_gen::TranslateToDxbc(program,options);if(!translated.ok)return 94;
      if(backend.GetShaderIL()==gpu::ShaderIL::kSpirv) {
        auto converted=xenos::dxbc_spirv::ConvertDxbcToSpirv(translated.bytecode.data(),translated.bytecode.size());
        if(!converted.ok)return 94;
        const auto* data=reinterpret_cast<const uint8_t*>(converted.words.data());
        bytes.assign(data,data+converted.words.size()*4);
      } else bytes=std::move(translated.bytecode);
    } else if(!compiler.Compile(xenos::hlsl::TranslateToHlsl(program,options).hlsl,gpu::ShaderStage::kPixel,backend.GetShaderIL(),bytes,error))return 94;
    gpu::GraphicsPipelineDesc d;d.vs=OverlayFixtureVertex(backend,compiler,dxbc);
    d.ps=backend.CreateShader(gpu::ShaderStage::kPixel,bytes.data(),bytes.size());d.depth_clip=false;
    d.color_count=1;d.color_formats[0]=td.format;d.color_write_masks[0]=15;
    if(premult) {
      d.color_blend[0].enable=true;d.color_blend[0].src=gpu::BlendFactor::kOne;
      d.color_blend[0].dst=gpu::BlendFactor::kInvSrcAlpha;
      d.color_blend[0].src_alpha=gpu::BlendFactor::kOne;d.color_blend[0].dst_alpha=gpu::BlendFactor::kZero;
    }
    const auto pipeline=backend.GetGraphicsPipeline(d);if(!pipeline)return 95;
    uint32_t system[6][4]{};for(auto& c:system[3])c=std::bit_cast<uint32_t>(1.f);
    float constants[5][4]={{0,0,0,.375f},{.5f,.5f,.5f,.5f},{0,0,0,0},{.25f,.5f,0,0},{.75f,.5f,0,0}};
    constants[0][3]=sourceAlpha;
    backend.SetConstants(gpu::ShaderStage::kPixel,0,system,sizeof(system));
    backend.SetConstants(gpu::ShaderStage::kPixel,1,constants,sizeof(constants));
    gpu::TextureView view;view.texture=encoded;
    for(unsigned c=0;c<4;++c)view.components[c]=hdr::SceneComponent(c,swap);
    gpu::SamplerDesc sampler;
    for(auto& address:sampler.address)address=gpu::SamplerAddress::kClamp;
    backend.SetTextures(&view,&sampler,1,1);constexpr float bg[4]={32,16,8,.5f};backend.ClearColor(output,bg);
    backend.SetRenderTargets(1,&output,0);backend.SetViewport({0,0,2,1,0,1});backend.SetScissor({0,0,1,1});
    backend.SetPipeline(pipeline);backend.Draw(3);
    rb=backend.ReadbackTexture(output,{0,0,2,1});backend.WaitForFence(backend.Submit());
    raw=backend.ReadbackData(rb,pitch);if(raw)std::memcpy(packed,raw,16);backend.ReleaseReadback(rb);if(!raw)return 96;
    for(unsigned c=0;c<3;++c) {
      const unsigned component=hdr::SceneComponent(c,swap);
      const float mean=(Half(ToHalf(std::pow(colors[component],1.f/2.2f)))+Half(ToHalf(std::pow(colors[4+component],1.f/2.2f))))*.5f;
      const float alpha=premult && sourceAlpha>1e-6f?sourceAlpha:1.f;
      const float converted=hdrOutput?std::pow(mean/alpha,2.2f)*alpha:std::min(mean,1.f);
      const float expected=converted+(premult?bg[c]*(1-sourceAlpha):0.f);
      if(!std::isfinite(Half(packed[c])) || std::abs(Half(packed[c])-expected)>expected*.015f+.003f || Half(packed[4+c])!=bg[c]) {
        std::printf("FAIL scene filter %s HDR %u c%u: %g expected %g\n",dxbc?"DXBC":"HLSL",hdrOutput,c,Half(packed[c]),expected);return 97;
      }
    }
    if(Half(packed[3])!=sourceAlpha || Half(packed[7])!=.5f)return 98;
  }
  std::puts(
      "PASS selective scene effects: lazy encoded input rejects padding, production DXBC/HLSL blur "
      "preserves HDR and original alpha, original bounded shader unchanged, scissor retains "
      "untouched HDR");return 0;
}

static int SceneEncodeBench(gpu::Backend& backend,xenos::renderer::ShaderCompiler& compiler) {
  std::vector<uint8_t> bytes;std::string error;
  if(!compiler.Compile(hdr::kEncodeSceneForEffects,gpu::ShaderStage::kCompute,backend.GetShaderIL(),bytes,error))return 99;
  const auto shader=backend.CreateShader(gpu::ShaderStage::kCompute,bytes.data(),bytes.size());
  const auto pipeline=backend.CreateComputePipeline(shader);if(!pipeline)return 99;
  gpu::TextureDesc d;d.width=3840;d.height=6144;d.format=gpu::Format::kR16G16B16A16Float;
  d.usage=gpu::kUsageSampled|gpu::kUsageStorage|gpu::kUsageRenderTarget;
  const auto source=backend.CreateTexture(d);d.height=2160;const auto output=backend.CreateTexture(d);
  if(!source||!output)return 99;
  const char* fillSource=R"(
RWTexture2D<float4> output:register(u0);
[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){
 uint w,h;output.GetDimensions(w,h);if(p.x>=w||p.y>=h)return;
 uint n=(p.x*1664525u+p.y*1013904223u);n^=n>>13;n*=1274126177u;
 float v=float(n&65535u)/65535.0;output[p.xy]=float4(v*16.0,v*v*4.0,2.0-v,.5);
})";
  bytes.clear();if(!compiler.Compile(fillSource,gpu::ShaderStage::kCompute,backend.GetShaderIL(),bytes,error))return 99;
  const auto fillShader=backend.CreateShader(gpu::ShaderStage::kCompute,bytes.data(),bytes.size());
  const auto fill=backend.CreateComputePipeline(fillShader);if(!fill)return 99;
  gpu::ComputeBindings seed;seed.uav_texture=source;backend.Dispatch(fill,seed,480,768,1);
  gpu::ComputeBindings cb;cb.srv[0]=source;cb.uav_texture=output;
  auto run=[&]{backend.Dispatch(pipeline,cb,480,270,1);backend.WaitForFence(backend.Submit());};
  for(unsigned i=0;i<4;++i)run();
  std::vector<gpu::GpuTagTime> times;backend.TakeGpuTagTimes(times);times.clear();
  for(unsigned t=1;t<=8;++t){backend.SetGpuTag(t);for(unsigned frame=0;frame<4;++frame)run();}
  backend.SetGpuTag(0);run();run();backend.TakeGpuTagTimes(times);
  double sum[9]{};for(const auto& time:times)if(time.tag>0&&time.tag<=8)sum[time.tag]+=time.ms/4;
  std::vector<double> samples;for(unsigned t=1;t<=8;++t){if(sum[t]<=0)return 99;samples.push_back(sum[t]);}
  std::sort(samples.begin(),samples.end());
  std::printf(
      "PASS visible 4K scene-input conversion: %.3f ms median, %.3f ms max; one dispatch per "
      "consumed version, none without consumers. Excludes blur replay and gameplay FPS.\n",(samples[3]+samples[4])*.5,samples.back());return 0;
}
