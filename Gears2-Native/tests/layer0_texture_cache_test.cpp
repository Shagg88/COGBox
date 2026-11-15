// Layer 0 unit tests: TextureCache overlay scale recreation, runtime overlay generation switching, and dual-hash
// (swap_hash + rex_hash) lookup. Game-free, GPU-free (mock gpu::Backend).
// Contributed by Flippage (COGBox PR #117); portions from gow3-recomp: BSD-3-Clause, Copyright (c) Flippage and the
// gow3-recomp contributors (THIRD_PARTY_NOTICES.md). The rest is our code (MIT).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <tuple>

#include <rex/hash.h>
#include <xenos/dev_tools.h>
#include <xenos/renderer/gpu.h>
#include <xenos/renderer/guest_watch.h>
#include <xenos/renderer/texture_cache.h>
#include <xenos/renderer/hdr_overlay.h>
#include <xenos/renderer/native_hdr.h>
#include <xenos/renderer/hdr_transport.h>

// Stub for guest_watch.cpp's diagnostic log when observer_hub.cpp is not linked into gn_tests.
uint64_t XenosSwapCount() { return 0; }

TEST_CASE("native HDR transport: content versions survive frames and reject stale or overlapping owners", "[native_hdr][hdr_transport]") {
  using namespace xenos::renderer::native_hdr;
  CompanionStore store;
  auto& target = store.targets[7];
  target.texture = 41; target.width = 3840; target.height = 2160;
  target.generation = 1; target.write_version = 17;
  REQUIRE(store.CurrentTarget(7, 17));
  target.used_frame = 100000;
  CHECK(store.CurrentTarget(7, 17)); // unchanged contents, regardless of frame
  CHECK_FALSE(store.CurrentTarget(7, 18)); // an unacknowledged clear/draw/alias write
  CHECK_FALSE(store.CurrentTarget(8, 17)); // different resource identity
  auto& resolved = store.resolves[0x1000];
  resolved.texture = 42; resolved.generation = 2; resolved.start = 0x1000; resolved.length = 0x2000;
  resolved.version = 19; resolved.format = 6; resolved.endian = 2; resolved.frame = 1;
  CHECK(store.CurrentResolve(0x1000,19,6,2));
  CHECK_FALSE(store.CurrentResolve(0x1000,20,6,2)); // CPU reuse or another resolve
  CHECK_FALSE(store.CurrentResolve(0x1000,19,7,2)); // incompatible interpretation
  CHECK_FALSE(store.CurrentResolve(0x1000,19,6,0));
  resolved.pending = true;
  CHECK_FALSE(store.CurrentResolve(0x1000,19,6,2)); // mutable source cannot stand in for a frozen copy
  resolved.pending = false;
  store.resolves[0x2000] = resolved;
  store.resolves[0x2000].start = 0x2000;
  store.InvalidateRange(0x3000,0); // empty write
  store.InvalidateRange(0x4000,1); // adjacent/unrelated
  CHECK(store.CurrentResolve(0x1000,19,6,2));
  store.InvalidateRange(0x2fff,1); // last overlapping byte, not just the base address
  CHECK_FALSE(store.CurrentResolve(0x1000,19,6,2));
  CHECK_FALSE(store.CurrentResolve(0x2000,19,6,2));
}

TEST_CASE("HDR partial writes preserve only a current interpretation", "[hdr_transport]") {
  using namespace xenos::renderer::native_hdr;
  CompanionStore store;
  auto& a=store.resolves[0x1000];
  a.texture=9; a.generation=4; a.start=0x1000; a.length=0x2000; a.version=8;
  auto& b=store.resolves[0x2000]; b=a; b.start=0x2000; b.version=7;
  store.BeginWrite(0x2800,32,[](uint32_t,uint32_t){return 8;});
  CHECK(a.patchable); CHECK_FALSE(a.generation);
  CHECK_FALSE(b.patchable); CHECK_FALSE(b.generation);
  // A second write cannot resurrect the old pre-write resource version.
  store.BeginWrite(0x2800,32,[](uint32_t,uint32_t){return 8;});
  CHECK_FALSE(a.patchable);
  a.generation=5; a.version=9;
  store.BeginWrite(0x4000,1,[](uint32_t,uint32_t){return 10;});
  CHECK(a.generation==5); // a disjoint write is harmless
}

TEST_CASE("HDR input proxies use distinct unused bindings", "[hdr_transport]") {
  using namespace xenos::renderer::native_hdr;
  const auto slots=SceneProxySlots((1u<<0)|(1u<<4),0x1f);
  CHECK(slots[0]==31); CHECK(slots[4]==30); CHECK(slots[2]==255);
  CHECK(SceneProxySlots(1,0xffffffffu)[0]==255);
  CHECK(SceneProxySlots(3,0x7fffffffu)[0]==255); // no partial binding on exhaustion
}

TEST_CASE("HDR presentation follows framebuffer swizzle and endian permutations", "[hdr_transport]") {
  using namespace xenos::renderer::native_hdr;
  CHECK(PresentCompanionSwizzle(0xA0A,0,0)==0xA0A);
  CHECK(PresentCompanionSwizzle(0x688,2,2)==0x688);
  CHECK(PresentCompanionSwizzle(0x688,0,2)==0x053);
  CHECK(PresentCompanionSwizzle(0xA0A,1,3)==0xAD1); // 8in16 XOR 16in32 = 8in32
}

TEST_CASE("HDR clamp extension follows RGB definitions through branches and overwrites", "[hdr_transport]") {
  namespace uc=xenos::ucode;
  const auto fetch=[] {
    uc::Instruction i; i.kind=uc::Instruction::Kind::kTextureFetch;
    i.tfetch.opcode=uc::FetchOpcode::kTextureFetch; i.tfetch.fetch_constant=4;
    i.tfetch.result.target=uc::Target::kRegister; i.tfetch.result.write_mask=15;
    return i;
  };
  const auto move=[](bool constant,bool alpha=false) {
    uc::Instruction i; i.alu.vector_opcode=uc::AluVectorOpcode::kMax;
    i.alu.vector_operand_count=2;
    i.alu.vector_result.target=uc::Target::kRegister;
    i.alu.vector_result.index=1; i.alu.vector_result.write_mask=alpha?8:15; i.alu.vector_result.clamp=true;
    for(auto& o:i.alu.vector_operands) o.source=constant?uc::Source::kConstantFloat:uc::Source::kRegister;
    return i;
  };
  uc::Program program; program.control_flow.resize(4);
  program.control_flow[0].instructions={fetch()};
  program.control_flow[1].instructions={move(false),move(true),move(false,true)};
  auto overwrite=move(true); overwrite.alu.vector_result.index=0;
  program.control_flow[2].instructions={overwrite};
  program.control_flow[3].instructions={move(false)};
  xenos::renderer::native_hdr::ExtendSceneClamps(program,1u<<4);
  CHECK(program.control_flow[1].instructions[0].alu.vector_result.hdr_upper_extension);
  CHECK_FALSE(program.control_flow[1].instructions[1].alu.vector_result.hdr_upper_extension);
  CHECK_FALSE(program.control_flow[1].instructions[2].alu.vector_result.hdr_upper_extension);
  CHECK_FALSE(program.control_flow[3].instructions[0].alu.vector_result.hdr_upper_extension);
  // One branch skips the overwrite, so the merge must retain HDR dependency.
  program.control_flow[1].opcode=uc::CfOpcode::kCondJmp;
  program.control_flow[1].branch_type=uc::ControlFlow::ExecType::kConditional;
  program.control_flow[1].target=3;
  xenos::renderer::native_hdr::ExtendSceneClamps(program,1u<<4);
  CHECK(program.control_flow[3].instructions[0].alu.vector_result.hdr_upper_extension);
  // A predicated write cannot prove that the old scene data was replaced.
  program.control_flow[1].opcode=uc::CfOpcode::kNop;
  program.control_flow[2].instructions[0].alu.predicated=true;
  program.control_flow[3].instructions[0].alu.vector_result.hdr_upper_extension=false;
  xenos::renderer::native_hdr::ExtendSceneClamps(program,1u<<4);
  CHECK(program.control_flow[3].instructions[0].alu.vector_result.hdr_upper_extension);
}

TEST_CASE("native HDR: captured UI proof rejects changed microcode and incomplete fingerprints", "[native_hdr]") {
  using namespace xenos::renderer::native_hdr;
  uint32_t code = 0x12345678;
  const UiProfile profile{42, 1, 0x17ECF28A95E4A9F3ull};
  CHECK(MatchesUiProfile(&profile, &code, 1));
  CHECK_FALSE(MatchesUiProfile(&profile, &code, 0));
  CHECK_FALSE(MatchesUiProfile(&profile, nullptr, 1));
  CHECK_FALSE(MatchesUiProfile(nullptr, &code, 1));
  code ^= 1;
  CHECK_FALSE(MatchesUiProfile(&profile, &code, 1));
}

TEST_CASE("native HDR: late overlay rules require UI evidence, resources and compatible blending", "[native_hdr]") {
  using namespace xenos::renderer;
  using namespace native_hdr;
  using F = gpu::BlendFactor;
  OverlayInputs input{true, true, true, false, false, false, 1};
  gpu::BlendDesc blend;
  CHECK(ClassifyOverlay(input, blend) == OverlayMode::kUi);
  blend.enable = true;
  for (const auto [src, dst, mode] : {
      std::tuple{F::kSrcAlpha, F::kInvSrcAlpha, OverlayMode::kUi},
      std::tuple{F::kSrcAlpha, F::kOne, OverlayMode::kUi},
      std::tuple{F::kOne, F::kInvSrcAlpha, OverlayMode::kPremultiplied},
      std::tuple{F::kOne, F::kOne, OverlayMode::kUi},
      std::tuple{F::kDestColor, F::kZero, OverlayMode::kModulation},
      std::tuple{F::kZero, F::kSrcColor, OverlayMode::kModulation},
      std::tuple{F::kDestColor, F::kSrcColor, OverlayMode::kModulation2x}}) {
    blend.src = src; blend.dst = dst;
    CHECK(ClassifyOverlay(input, blend) == mode);
  }
  for (auto member : {&OverlayInputs::owned_target, &OverlayInputs::screen_vertex,
                     &OverlayInputs::textures_available}) {
    auto uncertain = input; uncertain.*member = false;
    CHECK(ClassifyOverlay(uncertain, blend) == OverlayMode::kNone);
  }
  for (auto member : {&OverlayInputs::scene_texture, &OverlayInputs::cube_or_volume,
                     &OverlayInputs::shader_side_effects}) {
    auto uncertain = input; uncertain.*member = true;
    CHECK(ClassifyOverlay(uncertain, blend) == OverlayMode::kNone);
  }
  for (uint32_t outputs : {0u, 2u, 3u}) {
    auto uncertain = input; uncertain.color_outputs = outputs;
    CHECK(ClassifyOverlay(uncertain, blend) == OverlayMode::kNone);
  }
  blend.op = gpu::BlendOp::kSubtract;
  CHECK(ClassifyOverlay(input, blend) == OverlayMode::kNone);
  CHECK(PackedUiVertex((1ull << 25) | (1ull << 6), 0, false));
  CHECK_FALSE(PackedUiVertex(1ull << 38, 0, false));
  CHECK_FALSE(PackedUiVertex(1ull << 25, 1, false));
  CHECK_FALSE(PackedUiVertex(1ull << 25, 0, true));
}

TEST_CASE("native HDR: replay preserves tests and leaves depth/stencil updates to the guest", "[native_hdr]") {
  using namespace xenos::renderer;
  gpu::GraphicsPipelineDesc original;
  original.depth_enable = original.depth_write = original.stencil_enable = true;
  original.depth_func = gpu::CompareFunc::kLess;
  original.stencil_front.func = gpu::CompareFunc::kEqual;
  original.stencil_front.pass = gpu::StencilOp::kReplace;
  original.stencil_write_mask = 0x7f;
  original.color_blend[0].enable = true;
  original.color_blend[0].src = gpu::BlendFactor::kDestColor;
  original.color_blend[0].dst = gpu::BlendFactor::kSrcColor;
  const auto replay = native_hdr::OverlayPipeline(original, native_hdr::OverlayMode::kModulation2x);
  CHECK(replay.depth_enable);
  CHECK(replay.stencil_enable);
  CHECK(replay.depth_func == original.depth_func);
  CHECK(replay.stencil_front.func == original.stencil_front.func);
  CHECK_FALSE(replay.depth_write);
  CHECK(replay.stencil_write_mask == 0);
  CHECK(replay.stencil_front.pass == gpu::StencilOp::kKeep);
  CHECK(replay.color_blend[0].dst == gpu::BlendFactor::kZero);
  CHECK(original.depth_write);
  CHECK(original.stencil_front.pass == gpu::StencilOp::kReplace);
  using native_hdr::CompatibleOverlayDepth;
  CHECK(CompatibleOverlayDepth(3840, 2160, 1, 3840, 6144, 3840, 6144));
  CHECK_FALSE(CompatibleOverlayDepth(3840, 2160, 1, 3840, 6144, 3840, 2160));
  CHECK_FALSE(CompatibleOverlayDepth(3840, 2160, 2, 3840, 6144, 3840, 6144));
  CHECK_FALSE(CompatibleOverlayDepth(3840, 2160, 1, 3840, 2048, 3840, 2048));
  CHECK_FALSE(CompatibleOverlayDepth(0, 2160, 1, 3840, 6144, 3840, 6144));
}

TEST_CASE("native HDR: scene pairing rejects partially redirected scene sampling", "[native_hdr]") {
  using xenos::renderer::native_hdr::SceneUiPairSource;
  const std::string header = "float xe_degamma1(float g) { return g; }\n"
      "XePsOut xe_scene_ui_material(XePsIn xe_in) {\n";
  const std::string sample = "xe_tex_2d_0.Sample(xe_sampler_0, tc);\n";
  CHECK_FALSE(SceneUiPairSource(header + sample + "}", true).empty());
  CHECK(SceneUiPairSource(header + sample + "}").empty());
  CHECK_FALSE(SceneUiPairSource(header + sample + sample + sample + sample + "}").empty());
  CHECK_FALSE(SceneUiPairSource(header + "xe_tex_2d_0.GetDimensions(w,h);" + sample + "}", true).empty());
  CHECK(SceneUiPairSource(header + sample + "xe_tex_2d_0.SampleLevel(xe_sampler_0,tc,0);}", true).empty());
  CHECK(SceneUiPairSource(header + sample + "xe_tex_2d_0.Load(int3(0,0,0));}", true).empty());
  CHECK(SceneUiPairSource(header + "}", true).empty());
}

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

TEST_CASE("native HDR: CPU atlases are distinguished from resolved textures and reused memory", "[native_hdr][texture_cache]") {
  ClearDynamicOverlays();
  constexpr uint32_t base = 0x2000, other = 0x12000;
  std::vector<uint8_t> memory(0x20000, 0x55);
  MockBackend backend;
  GuestWatch watch(nullptr);
  TextureCache cache(backend, memory.data(), watch);
  uint32_t fetch[192]{};
  MakeLinearDxt5Fetch(fetch, base, 64, 64);
  MakeLinearDxt5Fetch(fetch + 6, other, 64, 64);
  gpu::TextureView views[96]{};
  gpu::SamplerDesc samplers[32]{};
  REQUIRE(cache.Bind(fetch, 3, 0, 0, 1, views, samplers) == 0);
  CHECK(cache.RenderSourceMask(3) == 0);
  CHECK(cache.RenderSourceMask(4) == 4); // unbound slot is never assumed to be an atlas
  REQUIRE(cache.Bind(fetch, 3, 0, 0, 2, views, samplers) == 0);
  CHECK(cache.RenderSourceMask(3) == 0); // slot-cache hit
  watch.MarkGpuWritten(base + 4096, 4);
  REQUIRE(cache.Bind(fetch, 3, 0, 0, 3, views, samplers) == 0);
  CHECK(cache.RenderSourceMask(3) == 1); // later page in the texture's allocation
  CHECK(cache.RenderSourceMask(3, false) == 1);
  memory[base] = 0x12; // partial CPU reuse must not be mistaken for a restored atlas
  watch.Bump(base, 8192);
  REQUIRE(cache.Bind(fetch, 3, 0, 0, 4, views, samplers) == 0);
  CHECK(cache.RenderSourceMask(3) == 1); // conservative after CPU reuse
  CHECK(cache.RenderSourceMask(3, false) == 0);
  watch.MarkGpuWritten(0x1e000, 4096);
  REQUIRE(cache.Bind(fetch, 3, 0, 0, 5, views, samplers) == 0);
  CHECK(cache.RenderSourceMask(3) == 1); // unrelated resolves don't affect the other atlas
  memory[base] = 0x55; // restore all bytes to the previously proven CPU atlas
  watch.Bump(base, 8192);
  REQUIRE(cache.Bind(fetch, 3, 0, 0, 6, views, samplers) == 0);
  CHECK(cache.RenderSourceMask(3) == 0);
  CHECK_FALSE(watch.NeverResolved((base + 4096) >> GuestWatch::kPageShift)); // preserve other callers' semantics
  watch.MarkGpuWritten(base + 4096, 4);
  REQUIRE(cache.Bind(fetch, 3, 0, 0, 7, views, samplers) == 0);
  CHECK(cache.RenderSourceMask(3) == 1); // a new GPU version revokes the restoration

  // Mip provenance matters even when the base image was loaded only by the CPU.
  uint32_t mipFetch[192]{};
  MakeLinearDxt5Fetch(mipFetch, other, 64, 64);
  mipFetch[4] = 1u << 6;
  mipFetch[5] |= 0x1a000;
  REQUIRE(cache.Bind(mipFetch, 1, 0, 0, 6, views, samplers) == 0);
  CHECK(cache.RenderSourceMask(1) == 0);
  watch.MarkGpuWritten(0x1a000, 4);
  REQUIRE(cache.Bind(mipFetch, 1, 0, 0, 7, views, samplers) == 0);
  CHECK(cache.RenderSourceMask(1) == 1);
  watch.Bump(0x1a000, 4096);
  memory[0x1a000] = 0x12;
  REQUIRE(cache.Bind(mipFetch, 1, 0, 0, 8, views, samplers) == 0);
  CHECK(cache.RenderSourceMask(1) == 1); // replacing only mip data with different contents is not proof
}
