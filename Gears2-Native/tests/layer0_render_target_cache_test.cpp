// Layer 0 unit tests: RenderTargetCache frees the targets of a scale no longer in use (0.4.9 task 7). Game-free,
// GPU-free (mock gpu::Backend, as in layer0_texture_cache_test.cpp). Our code (MIT).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <xenos/renderer/gpu.h>
#include <xenos/renderer/render_target_cache.h>

namespace {

using namespace xenos::renderer;

class RtMockBackend final : public gpu::Backend {
 public:
  struct CreatedTex {
    gpu::TextureHandle handle = 0;
    gpu::TextureDesc desc;
  };
  struct UploadRecord {
    gpu::TextureHandle texture = 0;
    uint32_t mip = 0;
    uint32_t layer = 0;
    uint32_t row_bytes = 0;
    uint32_t rows = 0;
    uint32_t slices = 0;
    std::vector<uint8_t> bytes;
  };

  gpu::TextureHandle next_handle = 1;
  std::vector<CreatedTex> created;
  std::vector<gpu::TextureHandle> destroyed;
  std::vector<UploadRecord> uploads;
  std::unordered_map<gpu::TextureHandle, gpu::TextureDesc> live_textures;
  gpu::BackendStats stats{};

  bool Init(const gpu::BackendOptions&, std::string&) override { return true; }
  std::string Describe() const override { return "RtMockBackend"; }
  gpu::ShaderIL GetShaderIL() const override { return gpu::ShaderIL::kDxil; }
  bool IsLost(std::string*) override { return false; }

  gpu::TextureHandle CreateTexture(const gpu::TextureDesc& desc) override {
    const gpu::TextureHandle h = next_handle++;
    created.push_back({h, desc});
    live_textures[h] = desc;
    return h;
  }
  void DestroyTexture(gpu::TextureHandle texture) override {
    destroyed.push_back(texture);
    live_textures.erase(texture);
  }
  gpu::TextureHandle ImportTexture(void*, gpu::Format, uint32_t) override { return 0; }
  void ReleaseImport(gpu::TextureHandle, uint32_t) override {}
  gpu::ShaderHandle CreateShader(gpu::ShaderStage, const void*, size_t) override { return 1; }
  gpu::PipelineHandle GetGraphicsPipeline(const gpu::GraphicsPipelineDesc&, bool, bool*) override { return 1; }
  gpu::PipelineHandle CreateComputePipeline(gpu::ShaderHandle) override { return 1; }
  void UploadGuestMemory(uint32_t, const void*, uint32_t) override {}
  void SetRenderTargets(uint32_t, const gpu::TextureHandle*, gpu::TextureHandle, bool) override {}
  void ClearDepthStencil(gpu::TextureHandle, bool, float, bool, uint8_t, const gpu::Rect*, uint32_t) override {}
  void SetViewport(const gpu::Viewport&) override {}
  void SetScissor(const gpu::Rect&) override {}
  void SetStencilRef(uint8_t) override {}
  void SetBlendConstant(const float[4]) override {}
  void ClearColor(gpu::TextureHandle, const float[4], const gpu::Rect*, uint32_t) override {}
  void SetPipeline(gpu::PipelineHandle) override {}
  void SetConstants(gpu::ShaderStage, uint32_t, const void*, uint32_t) override {}
  void SetTextures(const gpu::TextureView*, const gpu::SamplerDesc*, uint32_t, uint32_t) override {}
  void UploadTexture(gpu::TextureHandle texture, uint32_t mip, uint32_t layer, const void* data, uint32_t row_bytes,
                     uint32_t rows, uint32_t slices) override {
    const size_t total = size_t(row_bytes) * rows * slices;
    const auto* p = static_cast<const uint8_t*>(data);
    uploads.push_back({texture, mip, layer, row_bytes, rows, slices, std::vector<uint8_t>(p, p + total)});
  }
  void Draw(uint32_t) override {}
  void DrawIndexed(const uint32_t*, uint32_t) override {}
  void NextFrame() override {}
  gpu::BufferHandle CreateIndexBuffer(const uint32_t*, uint32_t) override { return 0; }
  void ReleaseIndexBuffer(gpu::BufferHandle) override {}
  void DrawIndexedBuffer(gpu::BufferHandle, uint32_t) override {}
  gpu::OcclusionHandle BeginOcclusionQuery() override { return 0; }
  void EndOcclusionQuery(gpu::OcclusionHandle) override {}
  bool GetOcclusionResult(gpu::OcclusionHandle, uint64_t*) override { return false; }
  void ReleaseOcclusionQuery(gpu::OcclusionHandle) override {}
  void Dispatch(gpu::PipelineHandle, const gpu::ComputeBindings&, uint32_t, uint32_t, uint32_t) override {}
  gpu::ReadbackHandle ReadbackTexture(gpu::TextureHandle, const gpu::Rect&) override { return 0; }
  bool IsReadbackReady(gpu::ReadbackHandle) override { return false; }
  const uint8_t* ReadbackData(gpu::ReadbackHandle, uint32_t&) override { return nullptr; }
  void ReleaseReadback(gpu::ReadbackHandle) override {}
  uint64_t Submit() override { return 1; }
  uint64_t CompletedFence() override { return 1; }
  void WaitForFence(uint64_t) override {}
  void BeginEvent(const char*) override {}
  void EndEvent() override {}
  const gpu::BackendStats& Stats() const override { return stats; }
};

}  // namespace

using namespace xenos::renderer;
namespace edram = xenos::edram;

TEST_CASE("render_target_cache: a scale change frees the old scale's targets", "[renderer_core][rt_cache]") {
  RtMockBackend backend;
  RenderTargetCache rts(backend, false, 2, 0);
  const RenderTargetKey colour{0, uint32_t(edram::ColorFormat::k_8_8_8_8), 1280, 0};
  const RenderTargetKey depth{1000, kStorageClassDepthFlag, 1280, 0};
  for (int cycle = 0; cycle < 4; ++cycle) {  // 2x, 1x, 2x, 1x: memory must stay at one set
    const uint32_t s = (cycle & 1) ? 1u : 2u;
    rts.SetPhaseScale(s);
    RenderTarget* c = rts.Get(colour);
    RenderTarget* d = rts.Get(depth);
    REQUIRE(c);
    REQUIRE(d);
    CHECK(c->scale == s);
    rts.MarkWritten(*c, {0, 0, c->width, c->height}, nullptr);
    rts.MarkWritten(*d, {0, 0, d->width, d->height}, nullptr);
    const uint64_t one_set = c->host_bytes + d->host_bytes;
    const RenderTargetCache::FreeResult r = rts.FreeUnusedScales({s});
    CHECK(rts.size() == 2);
    CHECK(rts.HostBytes() == one_set);
    CHECK(backend.live_textures.size() == 2);
    if (cycle) CHECK(r.count == 2);
    // No tile names a freed target: a write by a new target sees only live previous owners.
    std::vector<AliasEvent> aliases;
    rts.MarkWritten(*c, {0, 0, c->width, c->height}, &aliases);
    for (const AliasEvent& e : aliases) CHECK(rts.FindById(e.previous->id) == e.previous);
  }
  CHECK(backend.destroyed.size() == 6);
}

TEST_CASE("render_target_cache: twins of both phases in use are kept", "[renderer_core][rt_cache]") {
  RtMockBackend backend;
  RenderTargetCache rts(backend, false, 2, 640);  // pitch 640 and below stays at 1x
  const RenderTargetKey big{0, uint32_t(edram::ColorFormat::k_8_8_8_8), 1280, 0};
  const RenderTargetKey small{600, uint32_t(edram::ColorFormat::k_8_8_8_8), 320, 0};
  rts.SetPhaseScale(1);
  REQUIRE(rts.Get(big));
  rts.SetPhaseScale(2);
  REQUIRE(rts.Get(big));
  REQUIRE(rts.Get(small));
  CHECK(rts.FreeUnusedScales({2, 1}).count == 0);
  CHECK(rts.size() == 3);
  const RenderTargetCache::FreeResult r = rts.FreeUnusedScales({2});  // upscaling off: the 1x twin goes, the small one stays
  CHECK(r.count == 1);
  CHECK(rts.size() == 2);
  CHECK(rts.Find(small) != nullptr);
}
