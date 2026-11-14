// Execute the captured modulation PS, including its production epilogue and
// destination-color blend. A neutral factor must preserve HDR scene energy.
#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3dcompiler.h>
#endif

static int ModulationValuesCheck(gpu::Backend& backend,
                                 xenos::renderer::ShaderCompiler& compiler,
                                 const char* pixelPath) {
  const char* vertexSource = R"(
// Match the captured DXBC register order as well as the semantic names.
struct Output { float4 interp[16] : TEXCOORD0; float4 position : SV_Position; };
Output main(uint vertex : SV_VertexID) {
  Output o = (Output)0;
  o.position = float4(float2((vertex << 1) & 2, vertex & 2) * 2.0 - 1.0, 0.0, 1.0);
  o.interp[0] = float4(0.5, 0.5, 0.0, 0.0);
  // Alpha zero isolates the captured program's constant color term. This
  // drives its real output conversion without depending on a game texture.
  o.interp[1] = 0.0;
  return o;
})";
  std::vector<uint8_t> vertexBytes;
  std::string error;
  bool vertexOk = false;
#ifdef _WIN32
  if (backend.GetShaderIL() == gpu::ShaderIL::kDxil) {
    // The captured PS is DXBC. Its numeric fixture must use a DXBC VS too;
    // D3D12 does not accept the mixed DXIL/DXBC graphics shader pair.
    const auto module = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto compile = module ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(module, "D3DCompile")) : nullptr;
    ID3DBlob* code = nullptr;
    ID3DBlob* messages = nullptr;
    if (compile) {
      vertexOk = SUCCEEDED(compile(vertexSource, std::strlen(vertexSource), "modulation-fixture", nullptr, nullptr,
                                    "main", "vs_5_1", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                    0, &code, &messages));
      if (vertexOk) {
        const auto* data = static_cast<const uint8_t*>(code->GetBufferPointer());
        vertexBytes.assign(data, data + code->GetBufferSize());
      }
      if (messages) error.assign(static_cast<const char*>(messages->GetBufferPointer()), messages->GetBufferSize());
    }
    if (messages) messages->Release();
    if (code) code->Release();
    if (module) FreeLibrary(module);
  } else
#endif
  {
    vertexOk = compiler.Compile(vertexSource, gpu::ShaderStage::kVertex,
                                backend.GetShaderIL(), vertexBytes, error);
  }
  if (!vertexOk) {
    std::printf("FAIL modulation vertex: %s\n", error.c_str());
    return 40;
  }
  std::ifstream file(pixelPath, std::ios::binary);
  const std::string pixelBytes(std::istreambuf_iterator<char>(file), {});
  gpu::GraphicsPipelineDesc desc;
  desc.vs = backend.CreateShader(gpu::ShaderStage::kVertex, vertexBytes.data(), vertexBytes.size());
  desc.ps = backend.CreateShader(gpu::ShaderStage::kPixel, pixelBytes.data(), pixelBytes.size());
  desc.color_count = 1;
  desc.color_formats[0] = gpu::Format::kR16G16B16A16Float;
  desc.color_write_masks[0] = 15;
  desc.depth_clip = false;
  desc.color_blend[0].enable = true;
  desc.color_blend[0].src = gpu::BlendFactor::kDestColor;
  desc.color_blend[0].dst = gpu::BlendFactor::kZero;
  desc.color_blend[0].src_alpha = gpu::BlendFactor::kZero;
  desc.color_blend[0].dst_alpha = gpu::BlendFactor::kOne;
  const auto pipeline = desc.vs && desc.ps ? backend.GetGraphicsPipeline(desc) : 0;
  gpu::TextureDesc texture;
  texture.width = texture.height = 1;
  texture.format = gpu::Format::kR16G16B16A16Float;
  texture.usage = gpu::kUsageRenderTarget | gpu::kUsageSampled;
  const auto destination = backend.CreateTexture(texture), sample = backend.CreateTexture(texture);
  if (!pipeline || !destination || !sample) {
    std::printf("FAIL modulation setup: pipeline %u, VS %u, PS %u, target %u, sample %u\n",
                pipeline, desc.vs, desc.ps, destination, sample);
    return 41;
  }
  const uint16_t white[4] = {0x3c00, 0x3c00, 0x3c00, 0x3c00};
  backend.UploadTexture(sample, 0, 0, white, sizeof(white), 1, 1);
  gpu::TextureView view; view.texture = sample;
  gpu::SamplerDesc sampler;
  uint32_t system[6][4]{};
  system[0][2] = 3; // initialize the two shader input interpolators
  for (auto& component : system[3]) component = std::bit_cast<uint32_t>(1.0f);
  // This PS uses c7 and c255; the compact layout is low[0..7], high[255].
  float constants[9][4]{};
  constants[8][1] = -1.0f;
  uint32_t zero[256]{};
  constexpr float background[4] = {64.0f, 16.0f, 8.0f, 1.0f};
  unsigned checked = 0;
  for (float peak : {2.0f, 20.0f}) for (float uiWhite : {1.0f, 100.0f}) {
    system[5][0] = std::bit_cast<uint32_t>(peak);
    system[5][1] = std::bit_cast<uint32_t>(uiWhite);
    for (float factor : {-4.0f, 0.0f, 0.5f, 1.0f, 2.0f, 65000.0f}) {
      constants[8][0] = factor;
      backend.ClearColor(destination, background);
      backend.SetConstants(gpu::ShaderStage::kPixel, 0, system, sizeof(system));
      backend.SetConstants(gpu::ShaderStage::kPixel, 1, constants, sizeof(constants));
      backend.SetConstants(gpu::ShaderStage::kPixel, 2, zero, sizeof(zero));
      backend.SetConstants(gpu::ShaderStage::kPixel, 3, zero, sizeof(zero));
      backend.SetTextures(&view, &sampler, 1, 1);
      backend.SetRenderTargets(1, &destination, 0);
      backend.SetViewport({0, 0, 1, 1, 0, 1});
      backend.SetScissor({0, 0, 1, 1});
      backend.SetPipeline(pipeline);
      backend.Draw(3);
      const auto rb = backend.ReadbackTexture(destination, {0, 0, 1, 1});
      backend.WaitForFence(backend.Submit());
      uint32_t pitch = 0;
      const auto* bytes = backend.ReadbackData(rb, pitch);
      uint16_t actual[4]{};
      if (bytes) std::memcpy(actual, bytes, sizeof(actual));
      backend.ReleaseReadback(rb);
      if (!bytes) return 42;
      const float linearFactor = std::pow(std::clamp(factor, 0.0f, 1.0f), 2.2f);
      for (unsigned channel = 0; channel < 4; ++channel) {
        const float expected = background[channel] * (channel == 3 ? 1.0f : linearFactor);
        const float value = Half(actual[channel]);
        if (!std::isfinite(value) || std::abs(value - expected) > std::max(0.001f, expected * 0.005f)) {
          std::printf("FAIL modulation factor %g, peak %g, UI %g, channel %u: %g != %g\n",
                      factor, peak, uiWhite, channel, value, expected);
          return 43;
        }
      }
      ++checked;
    }
  }
  std::printf("PASS captured modulation: %u draws, neutral HDR retention, bounded factors, black, alpha, no UI-white or peak gain\n", checked);
  return 0;
}
