// Game-free regression fixtures for HDR review fixes. MIT.
#pragma once
#include <xenos/renderer/texture_cache.h>
#include <xenos/renderer/texture_pack.h>
#include <rex/hash.h>
#include <filesystem>
static int ScenePackInputCheck(gpu::Backend& backend, const char* directory) {
  namespace rr = xenos::renderer;
  const auto root = std::filesystem::absolute(directory);
  std::filesystem::create_directories(root);
  constexpr uint32_t base = 0x3000, w = 64, h = 64, length = 32 * 15 * 16 + 16 * 16;
  std::vector<uint8_t> memory(1 << 20, 0x5a);
  const auto hash = rr::ComputeRexTextureHash(XXH3_64bits(memory.data() + base, length), w, h, 1, 2,
                                              20, 0, 1, false, false, 0, length, 0);
  char filename[32];
  std::snprintf(filename, sizeof(filename), "%016llX.dds", static_cast<unsigned long long>(hash));
  std::vector<uint8_t> pixels(w * h * 4);
  for (unsigned i = 0; i < w * h; ++i) {
    pixels[i * 4] = pixels[i * 4 + 3] = 255;
  }
  const auto dds =
      rr::texpack::WriteDds(gpu::Format::kR8G8B8A8Unorm, w, h, pixels.data(), pixels.size());
  if (dds.empty())
    return 170;
  {
    std::ofstream file(root / filename, std::ios::binary);
    file.write(reinterpret_cast<const char*>(dds.data()), dds.size());
    if (!file)
      return 170;
  }
  const auto path = root.u8string();
  rex::cvar::SetFlagByName("gow2_texture_pack_dirs",
                           std::string(reinterpret_cast<const char*>(path.data()), path.size()));
  rr::GuestWatch watch(nullptr);
  rr::TextureCache cache(backend, memory.data(), watch);
  uint32_t fetch[192]{};
  fetch[0] = 2 | (2u << 22);
  fetch[1] = base | 20;
  fetch[2] = 63 | (63 << 13);
  fetch[3] = (0 | (1 << 3) | (2 << 6) | (3 << 9)) << 1;
  fetch[5] = 1 << 9;
  gpu::TextureView views[96]{};
  gpu::SamplerDesc samplers[32]{};
  if (cache.Bind(fetch, 1, 0, 0, 1, views, samplers) != 0 || cache.Stats().host_bytes != w * h * 4)
    return 171;
  const auto rb = backend.ReadbackTexture(views[0].texture, {0, 0, 1, 1});
  backend.WaitForFence(backend.Submit());
  uint32_t pitch = 0;
  const auto* data = backend.ReadbackData(rb, pitch);
  const bool replaced = data && data[0] == 255 && data[1] == 0 && data[2] == 0 && data[3] == 255;
  backend.ReleaseReadback(rb);
  if (!replaced)
    return 172;
  rr::TextureCache::SceneInput scene;
  if (cache.BoundSceneInput(0, fetch, scene))
    return 173;
  fetch[1] = 0x8000 | 6;
  if (cache.Bind(fetch, 1, 0, 0, 2, views, samplers) != 0 ||
      !cache.BoundSceneInput(0, fetch, scene))
    return 174;
  std::puts(
      "PASS HDR scene input: actual RGBA8 texture-pack replacement rejected; ordinary guest RGBA8 "
      "accepted");
  return 0;
}
static int PairedDepthCheck(gpu::Backend& backend, xenos::renderer::ShaderCompiler& compiler) {
  const bool mixed = backend.SupportsMixedAttachmentSizes();
  auto shader = [&](const char* source, gpu::ShaderStage stage) {
    std::vector<uint8_t> bytes;
    std::string error;
    if (!compiler.Compile(source, stage, backend.GetShaderIL(), bytes, error)) {
      std::printf("FAIL paired depth shader %s\n", error.c_str());
      return gpu::ShaderHandle(0);
    }
    return backend.CreateShader(stage, bytes.data(), bytes.size());
  };
  gpu::TextureDesc td;
  td.width = 2;
  td.height = mixed ? 1 : 4;
  td.format = gpu::Format::kR16G16B16A16Float;
  td.usage = gpu::kUsageRenderTarget | gpu::kUsageSampled;
  const auto hdrTarget = backend.CreateTexture(td);
  td.height = 4;
  td.format = gpu::Format::kR8G8B8A8Unorm;
  const auto sdrTarget = backend.CreateTexture(td);
  td.format = gpu::Format::kD24UnormS8;
  td.usage = gpu::kUsageDepthStencil;
  const auto depth = backend.CreateTexture(td);
  gpu::GraphicsPipelineDesc desc;
  desc.vs = shader(
      "float4 main(uint id:SV_VertexID):SV_Position{return "
      "float4(float2((id<<1)&2,id&2)*2-1,0.25,1);}",
      gpu::ShaderStage::kVertex);
  desc.ps = shader(
      "struct O{float4 hdr:SV_Target0;float4 sdr:SV_Target1;};O main(){O "
      "o;o.hdr=float4(8,4,2,1);o.sdr=float4(.5,.25,.125,1);return o;}",
      gpu::ShaderStage::kPixel);
  desc.color_count = 2;
  desc.color_formats[0] = gpu::Format::kR16G16B16A16Float;
  desc.color_formats[1] = gpu::Format::kR8G8B8A8Unorm;
  desc.color_write_masks[0] = desc.color_write_masks[1] = 15;
  desc.depth_format = td.format;
  desc.depth_clip = false;
  desc.depth_enable = desc.depth_write = desc.stencil_enable = true;
  desc.depth_func = gpu::CompareFunc::kLess;
  desc.stencil_front.func = desc.stencil_back.func = gpu::CompareFunc::kEqual;
  desc.stencil_front.pass = desc.stencil_back.pass = gpu::StencilOp::kIncrSat;
  const auto pipeline = backend.GetGraphicsPipeline(desc);
  if (!hdrTarget || !sdrTarget || !depth || !pipeline)
    return 175;
  const float background[] = {16, 16, 16, 1};
  backend.ClearColor(hdrTarget, background);
  const float zero[4]{};
  backend.ClearColor(sdrTarget, zero);
  backend.ClearDepthStencil(depth, true, .5f, true, 1, nullptr, 0);
  gpu::TextureHandle targets[] = {hdrTarget, sdrTarget};
  backend.SetRenderTargets(2, targets, depth);
  backend.SetViewport({0, 0, 2, 1, 0, 1});
  backend.SetScissor({0, 0, 1, 1});
  backend.SetStencilRef(1);
  backend.SetPipeline(pipeline);
  backend.Draw(3);
  // A repeat must be rejected by both the updated depth and stencil values.
  backend.Draw(3);
  const auto hr = backend.ReadbackTexture(hdrTarget, {0, 0, 2, 1}),
             sr = backend.ReadbackTexture(sdrTarget, {0, 0, 2, 1});
  backend.WaitForFence(backend.Submit());
  uint32_t hp = 0, sp = 0;
  const auto* hv = backend.ReadbackData(hr, hp);
  const auto* sv = backend.ReadbackData(sr, sp);
  uint16_t actual[8]{};
  uint8_t sdr[8]{};
  if (hv)
    std::memcpy(actual, hv, sizeof(actual));
  if (sv)
    std::memcpy(sdr, sv, sizeof(sdr));
  backend.ReleaseReadback(hr);
  backend.ReleaseReadback(sr);
  if (!hv || !sv || Half(actual[0]) != 8 || Half(actual[4]) != 16 ||
      std::abs(int(sdr[0]) - 128) > 1 || sdr[4] != 0)
    return 176;
  // Stencil must have advanced exactly once (1 -> 2), not been dropped or replayed twice.
  desc.depth_enable = desc.depth_write = false;
  desc.stencil_front.pass = desc.stencil_back.pass = gpu::StencilOp::kKeep;
  const auto probe = backend.GetGraphicsPipeline(desc);
  if (!probe)
    return 177;
  backend.ClearColor(hdrTarget, background);
  backend.SetRenderTargets(2, targets, depth);
  backend.SetStencilRef(2);
  backend.SetPipeline(probe);
  backend.Draw(3);
  const auto pr = backend.ReadbackTexture(hdrTarget, {0, 0, 1, 1});
  backend.WaitForFence(backend.Submit());
  uint32_t pp = 0;
  const auto* pv = backend.ReadbackData(pr, pp);
  uint16_t first = 0;
  if (pv)
    std::memcpy(&first, pv, 2);
  backend.ReleaseReadback(pr);
  if (!pv || Half(first) != 8 || backend.IsLost(nullptr))
    return 178;
  std::printf(
      "PASS paired HDR/SDR scene: original depth/stencil tests and one write, clipped scissor, "
      "mixed attachment sizes %s\n",
      mixed ? "supported" : "not supported (matched-size fallback)");
  return 0;
}
