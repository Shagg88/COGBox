// Layer 0 unit tests: TextureCache overlay scale recreation, runtime overlay generation switching, and dual-hash
// (swap_hash + rex_hash) lookup. Game-free, GPU-free (mock gpu::Backend).
// Contributed by Flippage (COGBox PR #117); portions from gow3-recomp: BSD-3-Clause, Copyright (c) Flippage and the
// gow3-recomp contributors (THIRD_PARTY_NOTICES.md). The rest is our code (MIT).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/hash.h>
#include <xenos/dev_tools.h>
#include <xenos/renderer/gpu.h>
#include <xenos/renderer/guest_watch.h>
#include <xenos/renderer/texture_cache.h>
#include <xenos/renderer/hdr_overlay.h>
#include <xenos/renderer/hdr_scene_effect.h>
#include <xenos/renderer/texture_pack.h>

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
  bool drop_upload=false;
  uint64_t load_drops=0;
  uint64_t TextureLoadDrops() const override { return load_drops; }

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
    if(drop_upload){++load_drops;return;}
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

TEST_CASE("HDR UI: normalized masks are independent but failed loads are not", "[texture_cache][hdr]") {
  ClearDynamicOverlays();
  std::vector<uint8_t> memory(1u<<20,0x55);MockBackend backend;GuestWatch watch(nullptr);
  TextureCache cache(backend,memory.data(),watch);
  uint32_t fetch[192]{};MakeLinearDxt5Fetch(fetch,0x1000,32,32);
  gpu::TextureView views[96]{};gpu::SamplerDesc samplers[32]{};
  REQUIRE(cache.Bind(fetch,1,0,0,1,views,samplers)==0);
  CHECK(cache.IndependentUiMask(1)==1);
  watch.MarkGpuWritten(0x1000,4096,false);
  REQUIRE(cache.Bind(fetch,1,0,0,2,views,samplers)==0);
  CHECK(cache.IndependentUiMask(1)==1);
  // R8 mask: bounded coverage remains valid even on previously rendered pages.
  fetch[1]=(fetch[1]&~63u)|2u;
  REQUIRE(cache.Bind(fetch,1,0,0,3,views,samplers)==0);
  CHECK(cache.IndependentUiMask(1)==1);
  backend.drop_upload=true;fetch[1]=0x8000|2u;
  REQUIRE(cache.Bind(fetch,1,0,0,4,views,samplers)==0);
  CHECK(cache.IndependentUiMask(1)==0);
  CHECK(cache.Stats().load_failures>0);
  backend.drop_upload=false;
  REQUIRE(cache.Bind(fetch,1,0,0,5,views,samplers)==0);
  CHECK(cache.IndependentUiMask(1)==1);
  ClearDynamicOverlays();
}
TEST_CASE("HDR UI: CPU RGBA8 proof is revoked by render provenance", "[texture_cache][hdr]") {
  ClearDynamicOverlays();std::vector<uint8_t> memory(1u<<20,0x55);
  MockBackend backend;GuestWatch watch(nullptr);TextureCache cache(backend,memory.data(),watch);
  uint32_t fetch[192]{};MakeLinearDxt5Fetch(fetch,0x1000,32,32);fetch[1]=(fetch[1]&~63u)|6u;
  gpu::TextureView views[96]{};gpu::SamplerDesc samplers[32]{};
  REQUIRE(cache.Bind(fetch,1,0,0,1,views,samplers)==0);CHECK(cache.IndependentUiMask(1)==1);
  watch.MarkGpuWritten(0x1000,4096,false);
  REQUIRE(cache.Bind(fetch,1,0,0,2,views,samplers)==0);CHECK(cache.IndependentUiMask(1)==0);
  // A page-level CPU invalidation alone is not proof of a complete replacement.
  watch.Bump(0x1000,4096);
  REQUIRE(cache.Bind(fetch,1,0,0,3,views,samplers)==0);CHECK(cache.IndependentUiMask(1)==0);
  CHECK(cache.IndependentUiMask(0)==0);ClearDynamicOverlays();
}
TEST_CASE("HDR UI: geometry and state bound eligibility; replay preserves original tests", "[hdr]") {
  namespace hdr=xenos::renderer::native_hdr;using M=hdr::OverlayMode;using F=gpu::BlendFactor;
  hdr::OverlayInputs input{true,true,true,false,false,false,1};gpu::GraphicsPipelineDesc original;
  original.depth_enable=original.depth_write=original.stencil_enable=true;original.stencil_write_mask=255;
  original.stencil_front.pass=original.stencil_back.pass=gpu::StencilOp::kIncrSat;
  original.color_blend[0].enable=true;original.color_blend[0].src=F::kOne;original.color_blend[0].dst=F::kInvSrcAlpha;
  REQUIRE(hdr::ClassifyOverlay(input,original.color_blend[0])==M::kPremultiplied);
  const auto replay=hdr::OverlayPipeline(original,M::kPremultiplied);
  CHECK(replay.depth_enable);CHECK_FALSE(replay.depth_write);CHECK(replay.stencil_enable);CHECK(replay.stencil_write_mask==0);
  CHECK(replay.stencil_front.pass==gpu::StencilOp::kKeep);CHECK(original.depth_write);CHECK(original.stencil_write_mask==255);
  input.scene_texture=true;CHECK(hdr::ClassifyOverlay(input,original.color_blend[0])==M::kNone);
  input.scene_texture=false;input.shader_side_effects=true;CHECK(hdr::ClassifyOverlay(input,original.color_blend[0])==M::kNone);
  input.shader_side_effects=false;input.screen_vertex=false;CHECK(hdr::ClassifyOverlay(input,original.color_blend[0])==M::kNone);
}

TEST_CASE("HDR scene effects: leases reject stale frames, owners, writes and guest memory", "[hdr]") {
  using xenos::renderer::native_hdr::SceneLease;
  SceneLease lease{10,17,25,3};
  CHECK(lease.Current(10,3,17,25));
  CHECK_FALSE(lease.Current(11,3,17,25));
  CHECK_FALSE(lease.Current(10,4,17,25));
  CHECK_FALSE(lease.Current(10,3,18,25));
  CHECK_FALSE(lease.Current(10,3,17,26));
  SceneLease empty;CHECK_FALSE(empty.Current(0,0,0,0));
}
TEST_CASE("HDR scene effects: only complete canonical RGBA8 resolves and fetches qualify", "[hdr]") {
  namespace h=xenos::renderer::native_hdr;
  CHECK(h::DirectSceneResolve(6,0,false,2,0,0,0,0,1280,720,1280));
  CHECK_FALSE(h::DirectSceneResolve(6,1,false,2,0,0,0,0,1280,720,1280));
  CHECK(h::DirectSceneResolve(6,0,true,2,0,0,0,0,1280,720,1280));
  CHECK(h::SceneComponent(0,true)==2);CHECK(h::SceneComponent(2,true)==0);
  CHECK(h::SceneComponent(1,true)==1);CHECK(h::SceneComponent(3,true)==3);
  CHECK(h::SceneComponent(0,false)==0);CHECK(h::SceneComponent(7,false)==5);
  CHECK_FALSE(h::DirectSceneResolve(6,0,false,2,8,0,0,0,1280,720,1280));
  CHECK_FALSE(h::DirectSceneResolve(6,0,false,2,0,0,8,0,1280,720,1280));
  CHECK_FALSE(h::DirectSceneResolve(6,0,false,2,0,0,0,0,1200,720,1280));
  CHECK_FALSE(h::DirectSceneResolve(7,0,false,2,0,0,0,0,1280,720,1280));
  uint32_t f[6]{};f[0]=(1u<<31)|(40u<<22);f[1]=6|(2<<6);f[3]=3<<10;f[5]=1<<9;
  CHECK(h::SceneFetchCompatible(f,2,1280));
  f[3]|=1;CHECK_FALSE(h::SceneFetchCompatible(f,2,1280));f[3]&=~1u;
  CHECK_FALSE(h::SceneFetchCompatible(f,0,1280));f[0]|=1<<2;CHECK_FALSE(h::SceneFetchCompatible(f,2,1280));
  f[0]&=~(1u<<2);f[3]=(3<<10)|(1<<13);CHECK_FALSE(h::SceneFetchCompatible(f,2,1280));
  f[3]=3<<10;f[4]=1<<6;CHECK_FALSE(h::SceneFetchCompatible(f,2,1280));f[4]=0;
  CHECK_FALSE(h::SceneFetchCompatible(f,2,1344));
  f[0]&=~(1u<<31);CHECK_FALSE(h::SceneFetchCompatible(f,2,1280));f[0]|=1u<<31;
  f[5]=0;CHECK_FALSE(h::SceneFetchCompatible(f,2,1280));f[5]=1<<9;
  f[1]|=1<<10;CHECK_FALSE(h::SceneFetchCompatible(f,2,1280));
  CHECK_FALSE(h::SceneFetchCompatible(nullptr,2,1280));
}
TEST_CASE("HDR scene effects: allocation arithmetic enforces the fixed budget", "[hdr]") {
  namespace h=xenos::renderer::native_hdr;
  CHECK(h::SceneEffectAllocationFits(0,0,3840,2160));
  CHECK(h::SceneEffectAllocationFits(3840ull*2160*8,3840ull*2160*8,5120,2160));
  CHECK_FALSE(h::SceneEffectAllocationFits(0,0,7680,4320));
  CHECK_FALSE(h::SceneEffectAllocationFits(0,1,1,1));
  CHECK_FALSE(h::SceneEffectAllocationFits(0,0,0,2160));
  CHECK_FALSE(h::SceneEffectAllocationFits(h::kSceneEffectBudget,0,1,1));
}
// 0.6.4 (GitHub #341): texture packs - one host texture per pack file, shared by every guest address that holds the same
// content; kept (counted) when nothing shows it, reused when the content comes back, freed only when the cap needs it.
TEST_CASE("texture_cache: a pack texture is read once and shared across guest addresses", "[texture_cache][texture_pack]") {
  namespace fs = std::filesystem;
  ClearDynamicOverlays();
  const fs::path tmp = fs::temp_directory_path() / "gn_tests_pack341";
  std::error_code ec;
  fs::remove_all(tmp, ec);
  const fs::path dump = tmp / "dump", pack = tmp / "pack";
  auto utf8 = [](const fs::path& p) {  // (the hook takes UTF-8, as the cvars do; %TEMP% may hold non-ASCII names)
    const auto u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
  };
  fs::create_directories(pack, ec);

  constexpr uint32_t kW = 256, kH = 256, kBytes = (kW / 4) * (kH / 4) * 16;  // 65536
  constexpr uint32_t kA = 0x10000, kB = 0x40000;
  std::vector<uint8_t> guest(0x80000, 0);
  std::vector<uint8_t> content(kBytes);
  for (size_t i = 0; i < content.size(); ++i) content[i] = uint8_t(i * 13 + 5);
  std::memcpy(guest.data() + kA, content.data(), kBytes);
  std::memcpy(guest.data() + kB, content.data(), kBytes);
  uint32_t fa[192] = {}, fb[192] = {};
  MakeLinearDxt5Fetch(fa, kA, kW, kH);
  MakeLinearDxt5Fetch(fb, kB, kW, kH);
  gpu::TextureView views[96] = {};
  gpu::SamplerDesc samplers[32] = {};

  // 1. The pack's name for this content: the renderer's own pack dump (as a pack maker gets it).
  SetTexturePacksForTests("", utf8(dump));
  {
    MockBackend backend;
    GuestWatch watch(nullptr);
    TextureCache cache(backend, guest.data(), watch);
    REQUIRE(cache.Bind(fa, 1u, 0u, 0u, 1, views, samplers) == 0);
  }
  std::string name;
  for (const auto& f : fs::directory_iterator(dump, ec)) name = f.path().filename().string();
  REQUIRE(name.size() > 17);
  REQUIRE(name[16] == '_');

  // 2. A 4x replacement (1024x1024 BC3) under that name.
  std::vector<uint8_t> big(size_t(1024 / 4) * (1024 / 4) * 16, 0x5A);
  const std::vector<uint8_t> dds = texpack::WriteDds(gpu::Format::kBC3, 1024, 1024, big.data(), big.size());
  REQUIRE(!dds.empty());
  { std::ofstream out(pack / (name.substr(0, 16) + "_4x.dds"), std::ios::binary); out.write(reinterpret_cast<const char*>(dds.data()), std::streamsize(dds.size())); }
  SetTexturePacksForTests(utf8(pack), "");

  MockBackend backend;
  GuestWatch watch(nullptr);
  TextureCache cache(backend, guest.data(), watch);
  cache.SetHostMemoryCap(64ull << 20);

  // 3. Address A: the file is read and uploaded once - a 1024x1024 host texture.
  REQUIRE(cache.Bind(fa, 1u, 0u, 0u, 1, views, samplers) == 0);
  REQUIRE(backend.created.size() == 1);
  CHECK(backend.created[0].desc.width == 1024);
  const gpu::TextureHandle shared = views[0].texture;
  CHECK(shared == backend.created[0].handle);
  CHECK(backend.uploads.size() == 1);
  CHECK(cache.Stats().host_bytes == big.size());
  CHECK(cache.Stats().textures == 1);

  // 4. Address B, same content (the game's pool moved it): the same host texture, no new texture, no upload.
  REQUIRE(cache.Bind(fb, 1u, 0u, 0u, 2, views, samplers) == 0);
  CHECK(views[0].texture == shared);
  CHECK(backend.created.size() == 1);
  CHECK(backend.uploads.size() == 1);
  CHECK(cache.Stats().host_bytes == big.size());  // counted once
  CHECK(cache.Stats().textures == 2);             // two entries show it

  // 5. Both addresses get other content: guest textures load there; the pack texture is NOT destroyed (kept for later).
  std::memset(guest.data() + kA, 0x21, kBytes);
  watch.Bump(kA, kBytes);
  std::memset(guest.data() + kB, 0x43, kBytes);
  watch.Bump(kB, kBytes);
  REQUIRE(cache.Bind(fa, 1u, 0u, 0u, 3, views, samplers) == 0);
  CHECK(views[0].texture != shared);
  REQUIRE(cache.Bind(fb, 1u, 0u, 0u, 3, views, samplers) == 0);
  CHECK(views[0].texture != shared);
  CHECK(std::find(backend.destroyed.begin(), backend.destroyed.end(), shared) == backend.destroyed.end());
  CHECK(backend.live_textures.count(shared) == 1);
  CHECK(cache.Stats().host_bytes == big.size() + 2 * uint64_t(kBytes));  // the kept pack texture + two guest textures
  const size_t uploads_before = backend.uploads.size(), created_before = backend.created.size();

  // 6. The content comes back at A: the kept texture again - no file read, no new texture, no upload.
  std::memcpy(guest.data() + kA, content.data(), kBytes);
  watch.Bump(kA, kBytes);
  REQUIRE(cache.Bind(fa, 1u, 0u, 0u, 4, views, samplers) == 0);
  CHECK(views[0].texture == shared);
  CHECK(backend.created.size() == created_before);
  CHECK(backend.uploads.size() == uploads_before);

  // 7. Unused again, and the cap needs the memory: the pack texture is freed (once), guest textures in use are kept.
  std::memset(guest.data() + kA, 0x65, kBytes);
  watch.Bump(kA, kBytes);
  REQUIRE(cache.Bind(fa, 1u, 0u, 0u, 5, views, samplers) == 0);
  CHECK(views[0].texture != shared);
  cache.SetHostMemoryCap(512ull << 10);  // 512 KB: under the 1 MB pack texture
  cache.EndFrame(6);
  CHECK(std::count(backend.destroyed.begin(), backend.destroyed.end(), shared) == 1);
  CHECK(backend.live_textures.count(shared) == 0);
  CHECK(cache.Stats().host_bytes <= (512ull << 10) / 10 * 9 + 2 * uint64_t(kBytes));

  // 8. Back again after the free: read from the file once more (a new texture), shared as before.
  std::memcpy(guest.data() + kA, content.data(), kBytes);
  watch.Bump(kA, kBytes);
  const size_t created_8 = backend.created.size();
  REQUIRE(cache.Bind(fa, 1u, 0u, 0u, 7, views, samplers) == 0);
  CHECK(backend.created.size() == created_8 + 1);
  CHECK(backend.created.back().desc.width == 1024);
  CHECK(views[0].texture == backend.created.back().handle);

  SetTexturePacksForTests("", "");
  ClearDynamicOverlays();
  fs::remove_all(tmp, ec);
}
