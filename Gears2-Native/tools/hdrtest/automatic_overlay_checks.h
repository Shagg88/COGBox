// Authored, game-free GPU checks of the production overlay translators.
#pragma once
#include <xenos/dxbc_gen.h>
#include <xenos/dxbc_spirv.h>
#include <xenos/renderer/hdr_overlay.h>


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
  uc::Instruction kill;kill.alu.vector_opcode=uc::AluVectorOpcode::kKillGt;
  kill.alu.vector_result.write_mask=0;kill.alu.vector_operand_count=2;
  kill.alu.vector_operands[0].source=kill.alu.vector_operands[1].source=uc::Source::kConstantFloat;
  kill.alu.vector_operands[1].index=1;
  for(auto& c:kill.alu.vector_operands[0].components)c=uc::Swizzle::kW;
  cf.exec_count = 2; cf.instructions={kill,ins}; program.control_flow.push_back(cf);
  gpu::TextureDesc td; td.width = 2; td.height = 1;
  td.format = gpu::Format::kR16G16B16A16Float;
  td.usage = gpu::kUsageRenderTarget | gpu::kUsageSampled;
  const auto target = backend.CreateTexture(td);
  if (!target) return 50;
  constexpr float background[] = {16, 4, 2, 1};
  constexpr float colors[][4] = {
      {0,0,0,0}, {.5f,.25f,.125f,0}, {.25f,.125f,.0625f,.5f},
      {.75f,.375f,.1875f,.5f}, {1,1,1,1}, {-4,2,65000,2}, {-4,2,65000,.5f}, {.5f,.5f,.5f,1}};
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
      for(bool alphaReject:{false,true}) for (float peak : {2.f, 20.f}) for (float white : {1.f, 3.f}) for (const auto& color : colors) {
        uint32_t system[6][4]{};system[0][0]=alphaReject?32u:0u;
        for (auto& c : system[3]) c = std::bit_cast<uint32_t>(1.f);
        system[5][0] = std::bit_cast<uint32_t>(peak);
        system[5][1] = std::bit_cast<uint32_t>(white);
        backend.ClearColor(target, background);
        backend.SetConstants(gpu::ShaderStage::kPixel, 0, system, sizeof(system));
        float constants[2][4];std::memcpy(constants[0],color,sizeof(color));std::fill_n(constants[1],4,.6f);
        backend.SetConstants(gpu::ShaderStage::kPixel, 1, constants, sizeof(constants));
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
          if(alphaReject || color[3]>.6f)expected=background[c];
          const float actual = Half(pixels[c]);
          if (!std::isfinite(actual) || std::abs(actual-expected) > std::max(.004f, expected*.006f) ||
              Half(pixels[4+c]) != background[c]) {
            std::printf(
                      "FAIL automatic overlay %s mode %u alpha %g channel %u: %g != %g, untouched "
                      "%g\n",
                dxbc?"DXBC":"HLSL", unsigned(mode), alpha, c, actual, expected, Half(pixels[4+c])); return 57;
          }
        }
        ++draws;
      }
      if (mode == Mode::kUi)
        for (float white : {1.f, 3.f})
          for (float level : {0.f, 1.f / 255.f, 2.f / 255.f, 4.f / 255.f, 8.f / 255.f, 16.f / 255.f,
                              32.f / 255.f, .18f}) {
            uint32_t system[6][4]{};
            for (auto& c : system[3])
              c = std::bit_cast<uint32_t>(1.f);
            system[5][0] = std::bit_cast<uint32_t>(20.f);
            system[5][1] = std::bit_cast<uint32_t>(white);
            const float constants[2][4] = {{level, level, level, 1.f}, {2.f, 2.f, 2.f, 2.f}};
            backend.ClearColor(target, background);
            backend.SetConstants(gpu::ShaderStage::kPixel, 0, system, sizeof(system));
            backend.SetConstants(gpu::ShaderStage::kPixel, 1, constants, sizeof(constants));
            backend.SetRenderTargets(1, &target, 0);
            backend.SetViewport({0, 0, 2, 1, 0, 1});
            backend.SetScissor({0, 0, 1, 1});
            backend.SetPipeline(pipeline);
            backend.Draw(3);
            const auto rb = backend.ReadbackTexture(target, {0, 0, 2, 1});
            backend.WaitForFence(backend.Submit());
            uint32_t pitch = 0;
            const auto* data = backend.ReadbackData(rb, pitch);
            uint16_t pixels[8]{};
            if (data)
              std::memcpy(pixels, data, sizeof(pixels));
            backend.ReleaseReadback(rb);
            if (!data)
              return 59;
            const float expected = std::pow(level, 2.2f) * white;
            for (unsigned c = 0; c < 3; ++c)
              if (!std::isfinite(Half(pixels[c])) || (level > 0 && Half(pixels[c]) == 0) ||
                  std::abs(Half(pixels[c]) - expected) > std::max(4e-8f, expected * .006f) ||
                  Half(pixels[4 + c]) != background[c]) {
                std::printf("FAIL UI near-black %s code %g: %g != %g\n", dxbc ? "DXBC" : "HLSL",
                            level * 255.f, Half(pixels[c]), expected);
                return 59;
              }
            ++draws;
          }
    }
  }
  if (backend.IsLost(nullptr)) return 58;
  std::printf(
      "PASS automatic overlays: %u draws; DXBC/HLSL, premultiplied alpha, additive emission, "
      "modulation/2x, untouched HDR, UI white isolation, near-black code separation, original kill "
      "and alpha-test rejection\n", draws);
  return 0;
}
