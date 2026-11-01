// Layer 0 unit tests: TextureCache overlay scale recreation, runtime overlay generation switching, and dual-hash
// (swap_hash + rex_hash) lookup. Game-free, GPU-free (mock gpu::Backend).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <rex/hash.h>
#include <xenos/dev_tools.h>
#include <xenos/renderer/gpu.h>
#include <xenos/renderer/guest_watch.h>
#include <xenos/renderer/texture_cache.h>

// Stub for guest_watch.cpp's diagnostic log when observer_hub.cpp is not linked into gn_tests.
uint64_t XenosSwapCount() { return 0; }

namespace {

using namespace xenos::renderer;

class MockBackend final : public gpu::Backend {
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
  std::string Describe() const override { return "MockBackend"; }
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

// Build a 6-dword Xenos 2D texture fetch constant at slot 0 for a linear DXT5 (format 20) texture.
void MakeLinearDxt5Fetch(uint32_t tf[6], uint32_t base_addr, uint32_t width, uint32_t height) {
  const uint32_t pitch_div_32 = width / 32;
  const uint32_t format = 20;  // k_DXT4_5 -> host BC3
  tf[0] = 2u | (pitch_div_32 << 22);  // type = kTexture (2), tiled = 0
  tf[1] = format | (base_addr & 0xFFFFF000u);  // endian = 0, base_page << 12
  tf[2] = (width - 1) | ((height - 1) << 13);
  const uint32_t swz = 0u | (1u << 3) | (2u << 6) | (3u << 9);
  tf[3] = swz << 1;
  tf[4] = 0;  // max_level = 0 (single mip)
  tf[5] = 1u << 9;  // dim = k2DOrStacked (1), packed = 0, mip_base = 0
}

// Create an in-memory overlay blob [width:u32, height:u32, RGBA...] filled with a solid RGBA color.
std::vector<uint8_t> MakeSolidOverlay(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  std::vector<uint8_t> ov(8 + size_t(w) * h * 4);
  std::memcpy(ov.data(), &w, 4);
  std::memcpy(ov.data() + 4, &h, 4);
  for (size_t i = 0; i < size_t(w) * h; ++i) {
    ov[8 + i * 4 + 0] = r;
    ov[8 + i * 4 + 1] = g;
    ov[8 + i * 4 + 2] = b;
    ov[8 + i * 4 + 3] = a;
  }
  return ov;
}

}  // namespace

TEST_CASE("texture_cache: recreates host texture when reused Key{a, b} changes ov_scale (1x <-> 4x)",
          "[texture_cache][overlay]") {
  ClearDynamicOverlays();

  constexpr uint32_t kBase = 0x1000;
  constexpr uint32_t kWidth = 256, kHeight = 256;
  constexpr uint32_t kBaseBytes = (kWidth / 4) * (kHeight / 4) * 16;  // 65536 bytes
  std::vector<uint8_t> guest_mem(kBase + kBaseBytes + 0x1000, 0);

  // Fill guest memory with initial non-prompt texture data (pattern 0x11).
  std::memset(guest_mem.data() + kBase, 0x11, kBaseBytes);

  // Prepare prompt texture guest bytes (pattern 0x77) and register a 4x (1024x1024) overlay for its swap_hash.
  std::vector<uint8_t> prompt_guest_bytes(kBaseBytes, 0x77);
  const uint64_t prompt_swap_hash = XXH3_64bits(prompt_guest_bytes.data(), kBaseBytes);
  SetDynamicOverlay(prompt_swap_hash, MakeSolidOverlay(1024, 1024, 255, 0, 0, 255));

  MockBackend backend;
  GuestWatch watch(nullptr);
  TextureCache cache(backend, guest_mem.data(), watch);

  uint32_t fetch[192] = {};
  MakeLinearDxt5Fetch(fetch, kBase, kWidth, kHeight);
  gpu::TextureView views[96] = {};
  gpu::SamplerDesc samplers[32] = {};

  // Step 1: Bind with initial 1x non-overlay texture at Key{a, b}.
  REQUIRE(cache.Bind(fetch, 1u, 0u, 0u, 1, views, samplers) == 0);
  REQUIRE(backend.created.size() == 1);
  CHECK(backend.created[0].desc.width == 256);
  CHECK(backend.created[0].desc.height == 256);
  CHECK(backend.destroyed.empty());
  REQUIRE(backend.uploads.size() == 1);
  CHECK(backend.uploads[0].row_bytes == (256 / 4) * 16);
  CHECK(backend.uploads[0].rows == 256 / 4);
  const gpu::TextureHandle handle_1x = views[0].texture;
  CHECK(handle_1x == backend.created[0].handle);
  CHECK(cache.Stats().textures == 1);
  CHECK(cache.Stats().host_bytes == 65536);

  // Step 2: Overwrite guest memory at the SAME Key{a, b} with prompt atlas bytes (matching 4x overlay), bump watch.
  std::memcpy(guest_mem.data() + kBase, prompt_guest_bytes.data(), kBaseBytes);
  watch.Bump(kBase, kBaseBytes);

  REQUIRE(cache.Bind(fetch, 1u, 0u, 0u, 2, views, samplers) == 0);
  // Old 256x256 host texture must be destroyed, and a new 1024x1024 host texture created and uploaded!
  REQUIRE(backend.destroyed.size() == 1);
  CHECK(backend.destroyed[0] == handle_1x);
  REQUIRE(backend.created.size() == 2);
  CHECK(backend.created[1].desc.width == 1024);
  CHECK(backend.created[1].desc.height == 1024);
  const gpu::TextureHandle handle_4x = views[0].texture;
  CHECK(handle_4x == backend.created[1].handle);
  CHECK(handle_4x != handle_1x);
  REQUIRE(backend.uploads.size() == 2);
  CHECK(backend.uploads[1].texture == handle_4x);
  CHECK(backend.uploads[1].row_bytes == (1024 / 4) * 16);
  CHECK(backend.uploads[1].rows == 1024 / 4);
  CHECK(backend.uploads[1].bytes.size() == 1024 * 1024);
  CHECK(cache.Stats().textures == 1);
  CHECK(cache.Stats().host_bytes == 1024 * 1024);

  // Step 3: Overwrite guest memory back with non-overlay bytes (4x -> 1x) and verify recreation back to 256x256.
  std::memset(guest_mem.data() + kBase, 0x22, kBaseBytes);
  watch.Bump(kBase, kBaseBytes);

  REQUIRE(cache.Bind(fetch, 1u, 0u, 0u, 3, views, samplers) == 0);
  REQUIRE(backend.destroyed.size() == 2);
  CHECK(backend.destroyed[1] == handle_4x);
  REQUIRE(backend.created.size() == 3);
  CHECK(backend.created[2].desc.width == 256);
  CHECK(backend.created[2].desc.height == 256);
  CHECK(views[0].texture == backend.created[2].handle);
  CHECK(cache.Stats().textures == 1);
  CHECK(cache.Stats().host_bytes == 65536);

  ClearDynamicOverlays();
}

TEST_CASE("texture_cache: bumping overlay generation invalidates slot_cache_ and reloads without guest writes",
          "[texture_cache][overlay]") {
  ClearDynamicOverlays();

  constexpr uint32_t kBase = 0x2000;
  constexpr uint32_t kWidth = 64, kHeight = 64;
  // Linear 64x64 DXT5: row_pitch_blocks = AlignUp(16, 32) = 32 blocks (512 bytes/row), y_extent_blocks = 16 rows.
  constexpr uint32_t kBaseBytes = 32 * 15 * 16 + 16 * 16;
  std::vector<uint8_t> guest_mem(kBase + 0x4000, 0x33);

  const uint64_t swap_hash = XXH3_64bits(guest_mem.data() + kBase, kBaseBytes);
  // Register initial 1x overlay (solid red).
  SetDynamicOverlay(swap_hash, MakeSolidOverlay(kWidth, kHeight, 255, 0, 0, 255));

  MockBackend backend;
  GuestWatch watch(nullptr);
  TextureCache cache(backend, guest_mem.data(), watch);

  uint32_t fetch[192] = {};
  MakeLinearDxt5Fetch(fetch, kBase, kWidth, kHeight);
  gpu::TextureView views[96] = {};
  gpu::SamplerDesc samplers[32] = {};

  // Initial bind loads the red overlay.
  REQUIRE(cache.Bind(fetch, 1u, 0u, 0u, 1, views, samplers) == 0);
  REQUIRE(backend.uploads.size() == 1);
  const std::vector<uint8_t> upload_red = backend.uploads.back().bytes;

  // Second bind with no changes hits slot_cache_ (no new upload).
  REQUIRE(cache.Bind(fetch, 1u, 0u, 0u, 2, views, samplers) == 0);
  CHECK(backend.uploads.size() == 1);
  CHECK(cache.Stats().slot_hits == 1);

  // Switch overlay to solid green (simulating controller <-> keyboard switch, NO guest memory write or watch.Bump!).
  SetDynamicOverlay(swap_hash, MakeSolidOverlay(kWidth, kHeight, 0, 255, 0, 255));

  REQUIRE(cache.Bind(fetch, 1u, 0u, 0u, 3, views, samplers) == 0);
  // Must have invalidated slot_cache_ and reloaded with the green overlay!
  REQUIRE(backend.uploads.size() == 2);
  const std::vector<uint8_t> upload_green = backend.uploads.back().bytes;
  CHECK((upload_green != upload_red));

  ClearDynamicOverlays();
}

TEST_CASE("texture_cache: both swap_hash and rex_hash match registered overlays", "[texture_cache][overlay]") {
  ClearDynamicOverlays();

  constexpr uint32_t kBase = 0x3000;
  constexpr uint32_t kWidth = 64, kHeight = 64;
  constexpr uint32_t kPitchDiv32 = kWidth / 32;
  constexpr uint32_t kFormat = 20;
  constexpr uint32_t kBaseBytes = 32 * 15 * 16 + 16 * 16;
  std::vector<uint8_t> guest_mem(kBase + 0x4000, 0x5A);

  const uint64_t swap_hash = XXH3_64bits(guest_mem.data() + kBase, kBaseBytes);
  const uint64_t rex_hash =
      ComputeRexTextureHash(swap_hash, kWidth, kHeight, 1, kPitchDiv32, kFormat, 0, 1, false, false, 0, kBaseBytes, 0);
  REQUIRE(swap_hash != rex_hash);

  MockBackend backend;
  GuestWatch watch(nullptr);
  TextureCache cache(backend, guest_mem.data(), watch);

  uint32_t fetch[192] = {};
  MakeLinearDxt5Fetch(fetch, kBase, kWidth, kHeight);
  gpu::TextureView views[96] = {};
  gpu::SamplerDesc samplers[32] = {};

  // 1. Load with no overlay to capture baseline guest bytes.
  REQUIRE(cache.Bind(fetch, 1u, 0u, 0u, 1, views, samplers) == 0);
  REQUIRE(backend.uploads.size() == 1);
  const std::vector<uint8_t> baseline = backend.uploads.back().bytes;

  // 2. Register a 2x overlay keyed by rex_hash ONLY (as in Gears 3 / ReXGlue dumps).
  SetDynamicOverlay(rex_hash, MakeSolidOverlay(kWidth * 2, kHeight * 2, 0, 128, 255, 255));
  REQUIRE(cache.Bind(fetch, 1u, 0u, 0u, 2, views, samplers) == 0);
  REQUIRE(backend.created.size() == 2);
  CHECK(backend.created.back().desc.width == kWidth * 2);
  CHECK(backend.created.back().desc.height == kHeight * 2);
  REQUIRE(backend.uploads.size() == 2);
  CHECK((backend.uploads.back().bytes != baseline));

  // 3. Clear and register a 1x overlay with "COGBOXOV" header keyed by swap_hash.
  ClearDynamicOverlays();
  std::vector<uint8_t> cogboxov = {'C', 'O', 'G', 'B', 'O', 'X', 'O', 'V'};
  const std::vector<uint8_t> body = MakeSolidOverlay(kWidth, kHeight, 255, 255, 0, 255);
  cogboxov.insert(cogboxov.end(), body.begin(), body.end());
  SetDynamicOverlay(swap_hash, std::move(cogboxov));

  REQUIRE(cache.Bind(fetch, 1u, 0u, 0u, 3, views, samplers) == 0);
  REQUIRE(backend.created.size() == 3);
  CHECK(backend.created.back().desc.width == kWidth);
  CHECK(backend.created.back().desc.height == kHeight);
  REQUIRE(backend.uploads.size() == 3);
  CHECK((backend.uploads.back().bytes != baseline));

  ClearDynamicOverlays();
}
