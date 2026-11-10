// Layer 0 unit tests: 4x prompt overlay compositor, multi-controller & procedural keycap pre-allocation, Chainsaw Duel
// clear_box vs draw_rect semantics, and zero-hitch style switching in TextureCache.
// Contributed by Flippage (COGBox PR #117); portions from gow3-recomp: BSD-3-Clause, Copyright (c) Flippage and the
// gow3-recomp contributors (THIRD_PARTY_NOTICES.md). The rest is our code (MIT).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rex/hash.h>
#include <xenos/renderer/gpu.h>
#include <xenos/renderer/guest_watch.h>
#include <xenos/renderer/prompt_atlas_catalog.h>
#include <xenos/renderer/prompt_overlay_compositor.h>
#include <xenos/renderer/texture_cache.h>

namespace {

using namespace xenos::renderer;
using namespace xenos::renderer::prompt;

class CompositorMockBackend final : public gpu::Backend {
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
  gpu::BackendStats stats{};

  bool Init(const gpu::BackendOptions&, std::string&) override { return true; }
  std::string Describe() const override { return "CompositorMockBackend"; }
  gpu::ShaderIL GetShaderIL() const override { return gpu::ShaderIL::kDxil; }
  bool IsLost(std::string*) override { return false; }

  gpu::TextureHandle CreateTexture(const gpu::TextureDesc& desc) override {
    const gpu::TextureHandle h = next_handle++;
    created.push_back({h, desc});
    return h;
  }
  void DestroyTexture(gpu::TextureHandle texture) override { destroyed.push_back(texture); }
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

void MakeLinearDxt5Fetch(uint32_t tf[6], uint32_t base_addr, uint32_t width, uint32_t height) {
  const uint32_t pitch_div_32 = width / 32;
  const uint32_t format = 20;  // k_DXT4_5 -> host BC3
  tf[0] = 2u | (pitch_div_32 << 22);
  tf[1] = format | (base_addr & 0xFFFFF000u);
  tf[2] = (width - 1) | ((height - 1) << 13);
  const uint32_t swz = 0u | (1u << 3) | (2u << 6) | (3u << 9);
  tf[3] = swz << 1;
  tf[4] = 0;
  tf[5] = 1u << 9;
}

uint8_t OverlayAlphaAt(std::span<const uint8_t> ov, uint32_t ow, int x, int y) {
  return ov[8 + (size_t(y) * ow + x) * 4 + 3];
}

}  // namespace

TEST_CASE("PromptOverlayCompositor pre-composes 4x overlays for all device styles and atlas kinds",
          "[layer0][prompt][compositor]") {
  PromptOverlayCompositor compositor;
  REQUIRE(compositor.Initialize("assets/glyphs"));
  CHECK(compositor.IsInitialized());
  compositor.SetMaxResidentStyles(7);  // this test looks at every style at once

  const DeviceStyle kActiveStyles[] = {
      DeviceStyle::kXbox,       DeviceStyle::kPlayStation,     DeviceStyle::kSwitch,
      DeviceStyle::kSteamDeck,  DeviceStyle::kSteamController, DeviceStyle::kKeyboard,
  };

  for (const auto& layout : GetAllAtlasLayouts()) {
    const uint32_t expected_w = layout.width * 4;
    const uint32_t expected_h = layout.height * 4;
    const size_t expected_bytes = 8 + size_t(expected_w) * expected_h * 4;

    // kOriginal must always return empty.
    CHECK(compositor.GetOverlay(layout.hash, DeviceStyle::kOriginal).empty());
    CHECK(compositor.GetOverlayShared(layout.hash, DeviceStyle::kOriginal) == nullptr);

    for (DeviceStyle style : kActiveStyles) {
      INFO("Atlas hash 0x" << std::hex << layout.hash << " style " << DeviceStyleName(style));
      const auto ov = compositor.GetOverlay(layout.hash, style);
      REQUIRE(ov.size() == expected_bytes);
      uint32_t ow = 0, oh = 0;
      std::memcpy(&ow, ov.data(), 4);
      std::memcpy(&oh, ov.data() + 4, 4);
      CHECK(ow == expected_w);
      CHECK(oh == expected_h);
    }
  }

  // Verify shared buffers across hashes of the same AtlasKind.
  CHECK(compositor.GetOverlayShared(0xa000624a174e08baULL, DeviceStyle::kXbox) ==
        compositor.GetOverlayShared(0xce11e464b5064714ULL, DeviceStyle::kXbox));
  CHECK(compositor.GetOverlayShared(0xa52ee00be48fe5c5ULL, DeviceStyle::kPlayStation) ==
        compositor.GetOverlayShared(0xae5c68a9d7f7398cULL, DeviceStyle::kPlayStation));
}

TEST_CASE("PromptOverlayCompositor produces distinct controller & keyboard art with cleared and drawn cell pixels",
          "[layer0][prompt][compositor]") {
  PromptOverlayCompositor compositor;
  REQUIRE(compositor.Initialize("assets/glyphs"));
  compositor.SetMaxResidentStyles(7);  // the spans below must all stay valid

  const uint64_t g3_hud = 0xa000624a174e08baULL;
  const uint64_t g3_font = 0xae5c68a9d7f7398cULL;

  const auto ov_xbox = compositor.GetOverlay(g3_hud, DeviceStyle::kXbox);
  const auto ov_ps = compositor.GetOverlay(g3_hud, DeviceStyle::kPlayStation);
  const auto ov_sw = compositor.GetOverlay(g3_hud, DeviceStyle::kSwitch);
  const auto ov_deck = compositor.GetOverlay(g3_hud, DeviceStyle::kSteamDeck);
  const auto ov_sc = compositor.GetOverlay(g3_hud, DeviceStyle::kSteamController);
  const auto ov_kb = compositor.GetOverlay(g3_hud, DeviceStyle::kKeyboard);

  REQUIRE_FALSE(ov_xbox.empty());
  CHECK_FALSE(std::ranges::equal(ov_xbox, ov_ps));
  CHECK_FALSE(std::ranges::equal(ov_xbox, ov_sw));
  CHECK_FALSE(std::ranges::equal(ov_xbox, ov_deck));
  CHECK_FALSE(std::ranges::equal(ov_xbox, ov_sc));
  CHECK_FALSE(std::ranges::equal(ov_xbox, ov_kb));
  CHECK_FALSE(std::ranges::equal(ov_ps, ov_sw));
  CHECK_FALSE(std::ranges::equal(ov_ps, ov_kb));

  // Verify every cell on the 256x256 Font sheet has both cleared (alpha == 1) and drawn (alpha >= 2) texels.
  const auto* font_layout = GetAtlasLayout(g3_font);
  REQUIRE(font_layout != nullptr);
  const uint32_t ow = font_layout->width * 4;

  for (DeviceStyle style : {DeviceStyle::kXbox, DeviceStyle::kPlayStation, DeviceStyle::kSwitch,
                            DeviceStyle::kSteamDeck, DeviceStyle::kSteamController, DeviceStyle::kKeyboard}) {
    const auto ov = compositor.GetOverlay(g3_font, style);
    REQUIRE(ov.size() == 8 + size_t(ow) * (font_layout->height * 4) * 4);
    for (size_t i = 0; i < font_layout->cells.size(); ++i) {
      const PromptCell& cell = font_layout->cells[i];
      int cleared_count = 0;
      int drawn_count = 0;
      for (int y = cell.box[1] * 4; y < cell.box[3] * 4; ++y) {
        for (int x = cell.box[0] * 4; x < cell.box[2] * 4; ++x) {
          const uint8_t a = OverlayAlphaAt(ov, ow, x, y);
          if (a == 1) ++cleared_count;
          if (a >= 2) ++drawn_count;
        }
      }
      INFO("Style " << DeviceStyleName(style) << " cell " << i << " (" << cell.button << ")");
      CHECK(cleared_count > 0);
      CHECK(drawn_count > 0);
    }
  }

  // Rebuilding keyboard overlays with custom keybinds must change the keyboard overlay buffer.
  const std::vector<uint8_t> kb_default(ov_kb.begin(), ov_kb.end());
  compositor.RebuildKeyboardOverlays({{"keybind_a", "K"}, {"keybind_x", "J"}});
  const auto ov_kb_custom = compositor.GetOverlay(g3_hud, DeviceStyle::kKeyboard);
  CHECK_FALSE(std::ranges::equal(kb_default, ov_kb_custom));
}

TEST_CASE("Chainsaw Duel overlay clears full box for B and Y cells and preserves outside draw_rect for LT and RT",
          "[layer0][prompt][compositor]") {
  PromptOverlayCompositor compositor;
  REQUIRE(compositor.Initialize("assets/glyphs"));

  constexpr uint64_t kDuelHash = 0x30d01cc6c7ff5cd3ULL;
  const auto* duel = GetAtlasLayout(kDuelHash);
  REQUIRE(duel != nullptr);
  const uint32_t ow = duel->width * 4;
  const uint32_t oh = duel->height * 4;

  const auto ov = compositor.GetOverlay(kDuelHash, DeviceStyle::kPlayStation);
  REQUIRE(ov.size() == 8 + size_t(ow) * oh * 4);

  for (const PromptCell& cell : duel->cells) {
    const std::string_view btn(cell.button);
    INFO("Chainsaw Duel cell: " << btn << " box=[" << cell.box[0] << "," << cell.box[1] << "," << cell.box[2] << ","
                                << cell.box[3] << "]");
    if (cell.clear_box) {
      // B and Y cells have 3 context buttons in `box` outside `draw_rect`; the entire `box` must have alpha >= 1,
      // and the outer region of `box` outside `draw_rect` must be cleared (alpha == 1).
      CHECK((btn == "b" || btn == "y"));
      for (int y = cell.box[1] * 4; y < (cell.box[3] + 1) * 4; ++y) {
        for (int x = cell.box[0] * 4; x < (cell.box[2] + 1) * 4; ++x) {
          const uint8_t a = OverlayAlphaAt(ov, ow, x, y);
          REQUIRE(a >= 1);
          const bool in_draw = x >= cell.draw_rect[0] * 4 && x < cell.draw_rect[2] * 4 &&
                               y >= cell.draw_rect[1] * 4 && y < cell.draw_rect[3] * 4;
          if (!in_draw) {
            REQUIRE(a == 1);
          }
        }
      }
    } else {
      // LT and RT cells have `clear_box == false`: pixels inside `box` but outside `draw_rect` must stay alpha == 0.
      CHECK((btn == "lt" || btn == "rt"));
      int preserved_zero = 0;
      int drawn_inside = 0;
      for (int y = cell.box[1] * 4; y < cell.box[3] * 4; ++y) {
        for (int x = cell.box[0] * 4; x < cell.box[2] * 4; ++x) {
          bool in_other_clear_box = false;
          for (const PromptCell& other : duel->cells) {
            if (!other.clear_box) continue;
            if (x >= other.box[0] * 4 && x < (other.box[2] + 1) * 4 &&
                y >= other.box[1] * 4 && y < (other.box[3] + 1) * 4) {
              in_other_clear_box = true;
              break;
            }
          }
          if (in_other_clear_box) continue;
          const uint8_t a = OverlayAlphaAt(ov, ow, x, y);
          const bool in_draw = x >= cell.draw_rect[0] * 4 && x < cell.draw_rect[2] * 4 &&
                               y >= cell.draw_rect[1] * 4 && y < cell.draw_rect[3] * 4;
          if (!in_draw) {
            REQUIRE(a == 0);
            ++preserved_zero;
          } else {
            REQUIRE(a >= 1);
            if (a >= 2) ++drawn_inside;
          }
        }
      }
      CHECK(preserved_zero > 0);
      CHECK(drawn_inside > 0);
    }
  }
}

TEST_CASE("PromptOverlayCompositor builds a style only when needed and keeps at most the resident limit",
          "[layer0][prompt][compositor]") {
  PromptOverlayCompositor compositor;
  REQUIRE(compositor.Initialize("assets/glyphs"));
  // Initialize decodes nothing: no style is ready until asked for.
  CHECK(compositor.IsStyleReady(DeviceStyle::kOriginal));
  CHECK_FALSE(compositor.IsStyleReady(DeviceStyle::kXbox));
  CHECK_FALSE(compositor.IsStyleReady(DeviceStyle::kKeyboard));

  REQUIRE(compositor.PrepareStyle(DeviceStyle::kKeyboard));
  REQUIRE(compositor.PrepareStyle(DeviceStyle::kPlayStation));
  CHECK(compositor.IsStyleReady(DeviceStyle::kKeyboard));
  CHECK(compositor.IsStyleReady(DeviceStyle::kPlayStation));
  CHECK_FALSE(compositor.IsStyleReady(DeviceStyle::kSwitch));

  // A third style drops the least recently used one (default limit 2) ...
  REQUIRE(compositor.PrepareStyle(DeviceStyle::kSwitch));
  CHECK_FALSE(compositor.IsStyleReady(DeviceStyle::kKeyboard));
  CHECK(compositor.IsStyleReady(DeviceStyle::kPlayStation));
  CHECK(compositor.IsStyleReady(DeviceStyle::kSwitch));

  // ... unless someone (the texture cache) still holds its buffer: then the same buffer comes back.
  const uint64_t g3_hud = 0xa000624a174e08baULL;
  const auto held = compositor.GetOverlayShared(g3_hud, DeviceStyle::kPlayStation);
  REQUIRE(held);
  REQUIRE(compositor.PrepareStyle(DeviceStyle::kKeyboard));
  REQUIRE(compositor.PrepareStyle(DeviceStyle::kSteamDeck));
  CHECK_FALSE(compositor.IsStyleReady(DeviceStyle::kPlayStation));
  CHECK(compositor.GetOverlayShared(g3_hud, DeviceStyle::kPlayStation) == held);
}

TEST_CASE("SetActiveStyle integrates with TextureCache and reuses cached host handles with zero hitching",
          "[layer0][prompt][compositor][texture_cache]") {
  ClearDynamicOverlays();

  PromptOverlayCompositor& compositor = GlobalPromptCompositor();
  compositor.ClearAtlasAliases();
  REQUIRE(compositor.Initialize("assets/glyphs"));

  constexpr uint32_t kBase = 0x4000;
  constexpr uint32_t kWidth = 256, kHeight = 256;
  constexpr uint32_t kBaseBytes = (kWidth / 4) * (kHeight / 4) * 16;  // 65536 bytes
  std::vector<uint8_t> guest_mem(kBase + kBaseBytes + 0x1000, 0x42);

  const uint64_t guest_swap_hash = XXH3_64bits(guest_mem.data() + kBase, kBaseBytes);
  compositor.RegisterAtlasAlias(guest_swap_hash, 0xae5c68a9d7f7398cULL);

  CompositorMockBackend backend;
  GuestWatch watch(nullptr);
  TextureCache cache(backend, guest_mem.data(), watch);

  uint32_t fetch[192] = {};
  MakeLinearDxt5Fetch(fetch, kBase, kWidth, kHeight);
  gpu::TextureView views[96] = {};
  gpu::SamplerDesc samplers[32] = {};

  // 1. Activate Xbox style and bind: creates 4x (1024x1024) texture #1 and uploads once.
  compositor.SetActiveStyle(DeviceStyle::kXbox);
  REQUIRE(cache.Bind(fetch, 1u, 0u, 0u, 1, views, samplers) == 0);
  REQUIRE(backend.created.size() == 1);
  REQUIRE(backend.uploads.size() == 1);
  CHECK(backend.created[0].desc.width == 1024);
  CHECK(backend.created[0].desc.height == 1024);
  const gpu::TextureHandle tex_xbox = views[0].texture;
  const std::vector<uint8_t> bytes_xbox = backend.uploads.back().bytes;
  CHECK(tex_xbox == backend.created[0].handle);

  // 2. Switch to PlayStation style: creates 4x texture #2 and uploads PlayStation blocks while keeping tex_xbox cached.
  compositor.SetActiveStyle(DeviceStyle::kPlayStation);
  REQUIRE(cache.Bind(fetch, 1u, 0u, 0u, 2, views, samplers) == 0);
  REQUIRE(backend.created.size() == 2);
  REQUIRE(backend.uploads.size() == 2);
  CHECK(backend.destroyed.empty());
  const gpu::TextureHandle tex_ps = views[0].texture;
  const std::vector<uint8_t> bytes_ps = backend.uploads.back().bytes;
  CHECK(tex_ps != tex_xbox);
  CHECK(bytes_ps != bytes_xbox);

  // 3. Switch to Keyboard style: creates 4x texture #3 and uploads Keyboard blocks.
  compositor.SetActiveStyle(DeviceStyle::kKeyboard);
  REQUIRE(cache.Bind(fetch, 1u, 0u, 0u, 3, views, samplers) == 0);
  REQUIRE(backend.created.size() == 3);
  REQUIRE(backend.uploads.size() == 3);
  CHECK(backend.destroyed.empty());
  const gpu::TextureHandle tex_kb = views[0].texture;
  const std::vector<uint8_t> bytes_kb = backend.uploads.back().bytes;
  CHECK(tex_kb != tex_xbox);
  CHECK(tex_kb != tex_ps);
  CHECK(bytes_kb != bytes_xbox);
  CHECK(bytes_kb != bytes_ps);

  // 4. Switch to Original style: creates 1x (256x256) texture #4 and uploads unscaled guest blocks.
  compositor.SetActiveStyle(DeviceStyle::kOriginal);
  REQUIRE(cache.Bind(fetch, 1u, 0u, 0u, 4, views, samplers) == 0);
  REQUIRE(backend.created.size() == 4);
  REQUIRE(backend.uploads.size() == 4);
  CHECK(backend.created[3].desc.width == 256);
  CHECK(backend.created[3].desc.height == 256);
  CHECK(backend.destroyed.empty());
  const gpu::TextureHandle tex_orig = views[0].texture;
  CHECK(tex_orig == backend.created[3].handle);

  // 5. Switch back to Xbox -> PlayStation -> Keyboard -> Original: ZERO new CreateTexture or UploadTexture calls!
  compositor.SetActiveStyle(DeviceStyle::kXbox);
  REQUIRE(cache.Bind(fetch, 1u, 0u, 0u, 5, views, samplers) == 0);
  CHECK(views[0].texture == tex_xbox);
  CHECK(backend.created.size() == 4);
  CHECK(backend.uploads.size() == 4);
  CHECK(backend.destroyed.empty());

  compositor.SetActiveStyle(DeviceStyle::kPlayStation);
  REQUIRE(cache.Bind(fetch, 1u, 0u, 0u, 6, views, samplers) == 0);
  CHECK(views[0].texture == tex_ps);
  CHECK(backend.created.size() == 4);
  CHECK(backend.uploads.size() == 4);
  CHECK(backend.destroyed.empty());

  compositor.SetActiveStyle(DeviceStyle::kKeyboard);
  REQUIRE(cache.Bind(fetch, 1u, 0u, 0u, 7, views, samplers) == 0);
  CHECK(views[0].texture == tex_kb);
  CHECK(backend.created.size() == 4);
  CHECK(backend.uploads.size() == 4);
  CHECK(backend.destroyed.empty());

  compositor.SetActiveStyle(DeviceStyle::kOriginal);
  REQUIRE(cache.Bind(fetch, 1u, 0u, 0u, 8, views, samplers) == 0);
  CHECK(views[0].texture == tex_orig);
  CHECK(backend.created.size() == 4);
  CHECK(backend.uploads.size() == 4);
  CHECK(backend.destroyed.empty());

  // 6. Verify host_bytes and textures accounting across all 4 cached variants, and clean destruction on eviction.
  const uint64_t expected_host_bytes = 3ull * (1024 * 1024) + 65536ull;
  CHECK(cache.Stats().textures == 4);
  CHECK(cache.Stats().host_bytes == expected_host_bytes);

  cache.SetHostMemoryCap(1024);
  cache.EndFrame(10);
  CHECK(backend.destroyed.size() == 4);
  CHECK(cache.Stats().textures == 0);
  CHECK(cache.Stats().host_bytes == 0);

  compositor.ClearAtlasAliases();
  ClearDynamicOverlays();
}
