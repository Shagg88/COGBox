// Authored, game-free GPU checks of the production overlay translators.
#pragma once
#include <xenos/dxbc_gen.h>
#include <xenos/dxbc_spirv.h>
#include <xenos/renderer/hdr_overlay.h>
#include <xenos/renderer/hdr_transport.h>

static gpu::ShaderHandle OverlayFixtureVertex(gpu::Backend& backend,
    xenos::renderer::ShaderCompiler& compiler, bool dxbc) {
  const char* source = R"(
struct Output { float4 interp[16] : TEXCOORD0; float4 position : SV_Position; };
Output main(uint vertex : SV_VertexID) {
  Output o = (Output)0;
  o.position = float4(float2((vertex << 1) & 2, vertex & 2) * 2.0 - 1.0, 0.0, 1.0);
  return o;
})";
  std::vector<uint8_t> bytes;
  std::string error;
  bool ok = false;
#ifdef _WIN32
  if (dxbc && backend.GetShaderIL() == gpu::ShaderIL::kDxil) {
    const auto module = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto compile = module ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(module, "D3DCompile")) : nullptr;
    ID3DBlob* code = nullptr; ID3DBlob* messages = nullptr;
    if (compile) {
      ok = SUCCEEDED(compile(source, std::strlen(source), "overlay-fixture", nullptr, nullptr,
          "main", "vs_5_1", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages));
      if (ok) {
        const auto* data = static_cast<const uint8_t*>(code->GetBufferPointer());
        bytes.assign(data, data + code->GetBufferSize());
      }
      if (messages) error.assign(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize());
    }
    if (code) code->Release();
    if (messages) messages->Release();
    if (module) FreeLibrary(module);
  } else
#endif
  {
    ok = compiler.Compile(source, gpu::ShaderStage::kVertex, backend.GetShaderIL(), bytes, error);
  }
  if (!ok) { std::printf("FAIL automatic overlay vertex: %s\n", error.c_str()); return 0; }
  return backend.CreateShader(gpu::ShaderStage::kVertex, bytes.data(), bytes.size());
}

static int AutomaticOverlayCheck(gpu::Backend& backend, xenos::renderer::ShaderCompiler& compiler) {
  namespace uc = xenos::ucode;
  using Mode = hdr::OverlayMode;
  using F = gpu::BlendFactor;
  // oC0 = max(c0,c0): exercise the actual translator epilogue, not a copy of it.
  uc::Program program; program.is_vertex_shader = false;
  uc::Instruction ins;
  ins.alu.vector_opcode = uc::AluVectorOpcode::kMax;
  ins.alu.vector_result.target = uc::Target::kColor;
  ins.alu.vector_result.write_mask = 15;
  ins.alu.vector_operand_count = 2;
  for (unsigned i = 0; i < 2; ++i) ins.alu.vector_operands[i].source = uc::Source::kConstantFloat;
  uc::ControlFlow cf; cf.opcode = uc::CfOpcode::kExecEnd; cf.exec_end = true;
  cf.exec_count = 1; cf.instructions.push_back(ins); program.control_flow.push_back(cf);
  gpu::TextureDesc td; td.width = 2; td.height = 1;
  td.format = gpu::Format::kR16G16B16A16Float;
  td.usage = gpu::kUsageRenderTarget | gpu::kUsageSampled;
  const auto target = backend.CreateTexture(td);
  if (!target) return 50;
  constexpr float background[] = {16, 4, 2, 1};
  constexpr float colors[][4] = {
      {0,0,0,0}, {.5f,.25f,.125f,0}, {.25f,.125f,.0625f,.5f},
      {.75f,.375f,.1875f,.5f}, {1,1,1,1}, {-4,2,65000,2}, {.5f,.5f,.5f,1}};
  unsigned draws = 0;
  for (bool dxbc : {true, false}) {
    const auto vertex = OverlayFixtureVertex(backend, compiler, dxbc);
    if (!vertex) return 51;
    for (Mode mode : {Mode::kUi, Mode::kPremultiplied, Mode::kModulation, Mode::kModulation2x}) {
      xenos::hlsl::HlslOptions options;
      options.clamp_ui_output = options.native_hdr_linear = true;
      options.native_hdr_modulation = mode == Mode::kModulation || mode == Mode::kModulation2x;
      options.native_hdr_ui = !options.native_hdr_modulation;
      options.native_hdr_premultiplied = mode == Mode::kPremultiplied;
      options.native_hdr_modulation_x2 = mode == Mode::kModulation2x;
      std::vector<uint8_t> bytes;
      std::string error;
      if (dxbc) {
        const auto translated = xenos::dxbc_gen::TranslateToDxbc(program, options);
        if (!translated.ok) { std::printf("FAIL overlay DXBC: %s\n", translated.unsupported.c_str()); return 52; }
        bytes = translated.bytecode;
        if (backend.GetShaderIL() == gpu::ShaderIL::kSpirv) {
          const auto converted = xenos::dxbc_spirv::ConvertDxbcToSpirv(bytes.data(), bytes.size());
          if (!converted.ok) { std::printf("FAIL overlay SPIR-V: %s\n", converted.error.c_str()); return 53; }
          const auto* data = reinterpret_cast<const uint8_t*>(converted.words.data());
          bytes.assign(data, data + converted.words.size() * 4);
        }
      } else if (!compiler.Compile(xenos::hlsl::TranslateToHlsl(program, options).hlsl,
          gpu::ShaderStage::kPixel, backend.GetShaderIL(), bytes, error)) {
        std::printf("FAIL overlay HLSL: %s\n", error.c_str()); return 54;
      }
      gpu::GraphicsPipelineDesc desc;
      desc.vs = vertex; desc.ps = backend.CreateShader(gpu::ShaderStage::kPixel, bytes.data(), bytes.size());
      desc.color_count = 1; desc.color_formats[0] = td.format; desc.color_write_masks[0] = 7;
      desc.depth_clip = false;
      auto& blend = desc.color_blend[0]; blend.enable = true;
      blend.src = mode == Mode::kUi ? F::kSrcAlpha : mode == Mode::kPremultiplied ? F::kOne : F::kDestColor;
      blend.dst = mode == Mode::kUi || mode == Mode::kPremultiplied ? F::kInvSrcAlpha :
          mode == Mode::kModulation2x ? F::kSrcColor : F::kZero;
      desc = hdr::OverlayPipeline(desc, mode);
      const auto pipeline = backend.GetGraphicsPipeline(desc);
      if (!pipeline) return 55;
      for (float peak : {2.f, 20.f}) for (float white : {1.f, 3.f}) for (const auto& color : colors) {
        uint32_t system[6][4]{};
        for (auto& c : system[3]) c = std::bit_cast<uint32_t>(1.f);
        system[5][0] = std::bit_cast<uint32_t>(peak);
        system[5][1] = std::bit_cast<uint32_t>(white);
        backend.ClearColor(target, background);
        backend.SetConstants(gpu::ShaderStage::kPixel, 0, system, sizeof(system));
        backend.SetConstants(gpu::ShaderStage::kPixel, 1, color, sizeof(color));
        backend.SetRenderTargets(1, &target, 0);
        backend.SetViewport({0,0,2,1,0,1});
        backend.SetScissor({0,0,1,1}); // untouched second pixel must retain HDR
        backend.SetPipeline(pipeline); backend.Draw(3);
        const auto rb = backend.ReadbackTexture(target, {0,0,2,1});
        backend.WaitForFence(backend.Submit());
        uint32_t pitch = 0; const auto* data = backend.ReadbackData(rb, pitch);
        uint16_t pixels[8]{};
        if (data) std::memcpy(pixels, data, sizeof(pixels));
        backend.ReleaseReadback(rb);
        if (!data) return 56;
        const float alpha = std::clamp(color[3], 0.f, 1.f);
        for (unsigned c = 0; c < 3; ++c) {
          const float value = std::clamp(color[c], 0.f, 1.f);
          float expected;
          if (mode == Mode::kModulation || mode == Mode::kModulation2x) {
            expected = background[c] * std::pow(value * (mode == Mode::kModulation2x ? 2.f : 1.f), 2.2f);
          } else if (mode == Mode::kPremultiplied) {
            // Straight-alpha equivalence for covered color, with independent
            // additive emission retained when alpha is zero or below RGB.
            const float covered = alpha ? std::min(value, alpha) / alpha : 0;
            expected = white * (alpha * std::pow(covered, 2.2f) + std::pow(std::max(value-alpha, 0.f), 2.2f)) + background[c]*(1-alpha);
          } else expected = white * std::pow(value, 2.2f)*alpha + background[c]*(1-alpha);
          const float actual = Half(pixels[c]);
          if (!std::isfinite(actual) || std::abs(actual-expected) > std::max(.004f, expected*.006f) ||
              Half(pixels[4+c]) != background[c]) {
            std::printf("FAIL automatic overlay %s mode %u alpha %g channel %u: %g != %g, untouched %g\n",
                dxbc?"DXBC":"HLSL", unsigned(mode), alpha, c, actual, expected, Half(pixels[4+c])); return 57;
          }
        }
        ++draws;
      }
    }
  }
  if (backend.IsLost(nullptr)) return 58;
  std::printf("PASS automatic overlays: %u draws; DXBC/HLSL, premultiplied alpha, additive emission, modulation/2x, untouched HDR, UI white isolation\n", draws);
  return 0;
}

static int PaddedStencilOverlayCheck(gpu::Backend& backend, xenos::renderer::ShaderCompiler& compiler) {
  auto compile = [&](const char* source, gpu::ShaderStage stage) {
    std::vector<uint8_t> bytes; std::string error;
    if (!compiler.Compile(source, stage, backend.GetShaderIL(), bytes, error)) {
      std::printf("FAIL stencil fixture compile: %s\n", error.c_str()); return gpu::ShaderHandle(0);
    }
    return backend.CreateShader(stage, bytes.data(), bytes.size());
  };
  const auto cs = compile(hdr::kCopyForUiCs, gpu::ShaderStage::kCompute);
  const auto copy = cs ? backend.CreateComputePipeline(cs) : 0;
  gpu::TextureDesc td; td.width = 2; td.height = 1;
  td.format = gpu::Format::kR16G16B16A16Float;
  td.usage = gpu::kUsageSampled | gpu::kUsageStorage | gpu::kUsageRenderTarget;
  const auto scene = backend.CreateTexture(td);
  td.height = 4; td.format = gpu::Format::kD24UnormS8; td.usage = gpu::kUsageDepthStencil;
  const auto depth = backend.CreateTexture(td);
  hdr::StencilReplayTarget replay;
  const float background[] = {8,4,2,1};
  backend.ClearColor(scene, background);
  if (!depth || !hdr::CompatibleOverlayDepth(2,1,1,2,4,2,4) ||
      !replay.Prepare(backend, copy, scene, 2,1,2,4)) return 70;
  const auto allocated = replay.texture;
  if (!replay.Prepare(backend, copy, scene, 2,1,2,4) || replay.texture != allocated) return 71;
  gpu::GraphicsPipelineDesc original;
  original.vs = OverlayFixtureVertex(backend, compiler, false);
  original.ps = compile("float4 main() : SV_Target { return float4(1,1,1,1); }", gpu::ShaderStage::kPixel);
  original.color_count = 1; original.color_formats[0] = gpu::Format::kR16G16B16A16Float;
  original.depth_format = td.format; original.depth_clip = false;
  original.stencil_enable = true; original.stencil_read_mask = original.stencil_write_mask = 255;
  original.stencil_front.func = original.stencil_back.func = gpu::CompareFunc::kAlways;
  original.stencil_front.pass = original.stencil_back.pass = gpu::StencilOp::kReplace;
  original.color_write_masks[0] = 0;
  const auto stamp = backend.GetGraphicsPipeline(original);
  if (!stamp) return 72;
  backend.ClearDepthStencil(depth, true, .5f, true, 0, nullptr, 0);
  backend.SetRenderTargets(1, &replay.texture, depth); backend.SetViewport({0,0,2,1,0,1});
  backend.SetScissor({0,0,1,1}); backend.SetStencilRef(1); backend.SetPipeline(stamp); backend.Draw(3);
  original.color_write_masks[0] = 15;
  original.stencil_front.func = original.stencil_back.func = gpu::CompareFunc::kEqual;
  original.stencil_front.pass = original.stencil_back.pass = gpu::StencilOp::kZero;
  original.depth_enable = original.depth_write = true; original.depth_func = gpu::CompareFunc::kLess;
  const auto overlay = backend.GetGraphicsPipeline(hdr::OverlayPipeline(original, hdr::OverlayMode::kUi));
  if (!overlay) return 73;
  // Repeat against the same stencil/depth. If replay wrote either, the
  // second pass would fail. Reset HDR color between passes to expose that.
  for (unsigned pass = 0; pass < 2; ++pass) {
    if (!replay.Prepare(backend, copy, scene, 2,1,2,4)) return 74;
    backend.SetRenderTargets(1, &replay.texture, depth);
    backend.SetViewport({0,0,2,1,0,1}); backend.SetScissor({0,0,2,1});
    backend.SetStencilRef(1); backend.SetPipeline(overlay); backend.Draw(3);
    hdr::StencilReplayTarget::Copy(backend, copy, replay.texture, scene, 2,1);
    const auto rb = backend.ReadbackTexture(scene, {0,0,2,1});
    backend.WaitForFence(backend.Submit()); uint32_t pitch = 0;
    const auto* bytes = backend.ReadbackData(rb, pitch); uint16_t pixels[8]{};
    if (bytes) std::memcpy(pixels, bytes, sizeof(pixels)); backend.ReleaseReadback(rb);
    if (!bytes) return 75;
    for (unsigned c = 0; c < 3; ++c) {
      if (Half(pixels[c]) != 1 || Half(pixels[4+c]) != background[c]) {
        std::printf("FAIL padded stencil pass %u channel %u: covered %g, untouched %g\n",
            pass, c, Half(pixels[c]), Half(pixels[4+c])); return 76;
      }
    }
    backend.ClearColor(scene, background);
  }
  // Scene-effect pairs use two equally padded attachments against the same
  // unchanged tests. Verify coverage on both MRT outputs.
  hdr::StencilReplayTarget lowReplay;
  if(!replay.Prepare(backend,copy,scene,2,1,2,4) || !lowReplay.Prepare(backend,copy,scene,2,1,2,4))return 79;
  original.ps=compile(R"(
struct Pair { float4 high:SV_Target0; float4 low:SV_Target1; };
Pair main() { Pair o;o.high=float4(1,1,1,1);o.low=float4(2,2,2,1);return o; }
)",gpu::ShaderStage::kPixel);
  original.color_count=2;original.color_formats[1]=original.color_formats[0];original.color_write_masks[1]=15;
  const auto pairPipeline=backend.GetGraphicsPipeline(hdr::OverlayPipeline(original,hdr::OverlayMode::kNone));
  if(!pairPipeline)return 79;
  const gpu::TextureHandle pairTargets[]={replay.texture,lowReplay.texture};
  backend.SetRenderTargets(2,pairTargets,depth);backend.SetViewport({0,0,2,1,0,1});backend.SetScissor({0,0,2,1});
  backend.SetStencilRef(1);backend.SetPipeline(pairPipeline);backend.Draw(3);
  const auto pairRb=backend.ReadbackTexture(lowReplay.texture,{0,0,2,1});
  backend.WaitForFence(backend.Submit());uint32_t pairPitch=0;const auto* pairBytes=backend.ReadbackData(pairRb,pairPitch);
  uint16_t pairPixels[8]{};if(pairBytes)std::memcpy(pairPixels,pairBytes,sizeof(pairPixels));backend.ReleaseReadback(pairRb);
  if(!pairBytes || Half(pairPixels[0])!=2 || Half(pairPixels[4])!=8)return 79;
  // Nonzero scissor origin copies only the affected UI rectangle.
  const float zero[] = {0,0,0,0}; backend.ClearColor(replay.texture, zero);
  hdr::StencilReplayTarget::Copy(backend, copy, scene, replay.texture, 1,1,1,0);
  const auto offset_rb = backend.ReadbackTexture(replay.texture, {0,0,2,1});
  backend.WaitForFence(backend.Submit()); uint32_t offset_pitch = 0;
  const auto* offset_bytes = backend.ReadbackData(offset_rb, offset_pitch); uint16_t offset_pixels[8]{};
  if (offset_bytes) std::memcpy(offset_pixels, offset_bytes, sizeof(offset_pixels));
  backend.ReleaseReadback(offset_rb);
  if (!offset_bytes || Half(offset_pixels[0]) != 0 || Half(offset_pixels[4]) != background[0]) return 78;
  backend.WaitForFence(backend.Submit());
  if (backend.IsLost(nullptr)) return 77;
  std::puts("PASS padded stencil replay: matching allocated extent, cropped FP16 copy, stencil clipping, retained HDR, no repeated depth/stencil writes, cached allocation");
  return 0;
}

static int AutomaticSceneOverlayCheck(gpu::Backend& backend, xenos::renderer::ShaderCompiler& compiler) {
  namespace uc = xenos::ucode;
  auto shader = [&](const std::string& source, gpu::ShaderStage stage) {
    std::vector<uint8_t> bytes; std::string error;
    if (!compiler.Compile(source, stage, backend.GetShaderIL(), bytes, error)) {
      std::printf("FAIL automatic scene effect compile: %s\n", error.c_str()); return gpu::ShaderHandle(0);
    }
    return backend.CreateShader(stage, bytes.data(), bytes.size());
  };
  auto compute = [&](const char* source) {
    const auto s = shader(source, gpu::ShaderStage::kCompute);
    return s ? backend.CreateComputePipeline(s) : 0;
  };
  // A new, unprofiled scene-sampling material, authored as translator IR.
  uc::Program program; program.is_vertex_shader = false;
  uc::Instruction fetch; fetch.kind = uc::Instruction::Kind::kTextureFetch;
  fetch.tfetch.dimension = uc::FetchDimension::k2D;
  fetch.tfetch.has_result = fetch.tfetch.has_fetch_constant = fetch.tfetch.has_attributes = true;
  fetch.tfetch.result.target = uc::Target::kRegister; fetch.tfetch.result.write_mask = 15;
  uc::Instruction output;
  output.alu.vector_opcode = uc::AluVectorOpcode::kMax;
  output.alu.vector_result.target = uc::Target::kColor; output.alu.vector_result.write_mask = 15;
  output.alu.vector_result.clamp = true;
  output.alu.vector_operand_count = 2;
  uc::ControlFlow cf; cf.opcode = uc::CfOpcode::kExecEnd; cf.exec_end = true;
  cf.exec_count = 2; cf.instructions = {fetch, output}; program.control_flow.push_back(cf);
  xenos::hlsl::HlslOptions options; options.entry = "xe_scene_ui_material";
  hdr::ExtendSceneClamps(program,1);
  const auto pairSource = hdr::TransportScenePairSource(xenos::hlsl::TranslateToHlsl(program, options).hlsl,1,1);
  if (pairSource.empty()) { std::puts("FAIL automatic scene material redirection"); return 60; }
  gpu::GraphicsPipelineDesc desc;
  desc.vs = OverlayFixtureVertex(backend, compiler, false);
  desc.ps = shader(pairSource, gpu::ShaderStage::kPixel);
  desc.color_count = 2; desc.depth_clip = false;
  for (unsigned i = 0; i < 2; ++i) {
    desc.color_formats[i] = gpu::Format::kR16G16B16A16Float; desc.color_write_masks[i] = 15;
    desc.color_blend[i].enable = true;
    desc.color_blend[i].src = gpu::BlendFactor::kSrcAlpha;
    desc.color_blend[i].dst = gpu::BlendFactor::kInvSrcAlpha;
  }
  const auto pipeline = desc.vs && desc.ps ? backend.GetGraphicsPipeline(desc) : 0;
  const auto copy = compute(hdr::kCopyForUiCs), prepare = compute(hdr::kPrepareSceneUiCs);
  const auto background = compute(hdr::kPrepareSceneOverlayCs), mix = compute(hdr::kSceneUiBlendCs);
  gpu::TextureDesc td; td.width = td.height = 1;
  td.format = gpu::Format::kR16G16B16A16Float;
  td.usage = gpu::kUsageRenderTarget | gpu::kUsageStorage | gpu::kUsageSampled;
  const auto current = backend.CreateTexture(td), snapshot = backend.CreateTexture(td);
  const auto proxy = backend.CreateTexture(td), high = backend.CreateTexture(td), low = backend.CreateTexture(td);
  td.format = gpu::Format::kR8G8B8A8Unorm; td.usage = gpu::kUsageSampled | gpu::kUsageRenderTarget;
  const auto reference = backend.CreateTexture(td), bounded = backend.CreateTexture(td);
  if (!pipeline || !copy || !prepare || !background || !mix || !current || !snapshot || !proxy || !high || !low || !reference || !bounded) return 61;
  const uint16_t source[] = {ToHalf(4),ToHalf(4),ToHalf(4),ToHalf(1)};
  backend.UploadTexture(current, 0, 0, source, 8, 1, 1);
  gpu::ComputeBindings b; b.constants[0] = b.constants[1] = 1; b.constant_count = 4;
  b.srv[0] = current; b.uav_texture = snapshot; backend.Dispatch(copy, b, 1, 1, 1);
  // A later overlay changes the current image. The material must still sample
  // the snapshot tied to its original resolve, without feeding that overlay back.
  const float changed[] = {16,16,16,1}; backend.ClearColor(current, changed);
  const uint8_t sdr[] = {255,255,255,128};
  backend.UploadTexture(reference, 0, 0, sdr, 4, 1, 1);
  backend.UploadTexture(bounded, 0, 0, sdr, 4, 1, 1);
  b.srv[0] = snapshot; b.srv[1] = reference; b.uav_texture = proxy; b.uav2_texture = low;
  backend.Dispatch(prepare, b, 1, 1, 1);
  b.srv[0] = current; b.srv[1] = bounded; b.uav_texture = high;
  backend.Dispatch(background, b, 1, 1, 1);
  gpu::TextureView views[32]; views[0].texture = reference; views[31].texture = proxy;
  gpu::SamplerDesc samplers[1];
  backend.SetTextures(views, samplers, 32, 1);
  uint32_t system[6][4]{}; system[0][2] = 1;
  for (auto& c : system[3]) c = std::bit_cast<uint32_t>(1.f);
  uint32_t fetchConstants[192]{}; // no gamma/sign/format conversion
  backend.SetConstants(gpu::ShaderStage::kPixel, 0, system, sizeof(system));
  backend.SetConstants(gpu::ShaderStage::kPixel, 3, fetchConstants, sizeof(fetchConstants));
  const gpu::TextureHandle targets[] = {high, low};
  backend.SetRenderTargets(2, targets, 0); backend.SetViewport({0,0,1,1,0,1});
  backend.SetScissor({0,0,1,1}); backend.SetPipeline(pipeline); backend.Draw(3);
  b = {}; b.constants[0] = b.constants[1] = 1; b.constants[4] = std::bit_cast<uint32_t>(20.f); b.constant_count = 8;
  b.srv[0] = high; b.srv[1] = low; b.srv[2] = bounded; b.uav_texture = current;
  backend.Dispatch(mix, b, 1, 1, 1);
  const auto rb = backend.ReadbackTexture(current, {0,0,1,1});
  backend.WaitForFence(backend.Submit()); uint32_t pitch = 0;
  const auto* bytes = backend.ReadbackData(rb, pitch); uint16_t pixels[4]{};
  if (bytes) std::memcpy(pixels, bytes, sizeof(pixels)); backend.ReleaseReadback(rb);
  const float alpha = 128.f/255;
  const float expected = std::pow(std::pow(4.f,1.f/2.2f)*alpha + std::pow(16.f,1.f/2.2f)*(1-alpha), 2.2f);
  if (!bytes) return 62;
  for (unsigned c = 0; c < 3; ++c) if (!std::isfinite(Half(pixels[c])) || std::abs(Half(pixels[c])-expected)>.05f) {
    std::printf("FAIL automatic scene snapshot channel %u: %g != %g\n", c, Half(pixels[c]), expected); return 63;
  }
  std::puts("PASS automatic scene effect: new material, frozen resolve, independent current background, partial alpha, paired HDR range");
  return 0;
}
