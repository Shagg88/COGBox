// Layer 0 (game-free): the API-independent command layer of our own renderer (S3.5, notes/42 section 2 "backend
// interface", decision P1/D26: D3D12 first, Vulkan second). Everything above this interface (EDRAM model,
// render-target cache, resolves, pipeline state from registers, the shadow renderer) is written against it; a backend
// (d3d12_backend.cpp now, a Vulkan one later) implements it.
//
// The binding layout is fixed by the translator's contract (notes/39), so it is not described per pipeline:
//   graphics: b0 system constants, b1 the stage's 256 float constants, b2 bool/loop constants, b3 fetch constants
//             (separate data per stage for b0-b3: the vertex and pixel shader each get their own copy), t0-t31 2D,
//             t32-t63 cube, t64-t95 3D textures, t96 guest physical memory (raw buffer), s0-s31 samplers.
//   compute:  b0 root constants (up to 16 dwords), t0-t3 textures (with a plane: depth = 0, stencil = 1), u0 a
//             texture or u0 a raw buffer.
// Vulkan uses the same numbers as (set, binding): set 0 constants, set 1 textures + guest memory, set 2 samplers.
//
// Threading: one recording thread (the GPU worker thread that calls the draw observer). Not thread-safe.
// Our code (MIT).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// Everything here is in xenos::renderer::gpu so the register-side types of xenos/renderer/pipeline_state.h (same
// names: CompareFunc, StencilOp, CullMode, Topology) can live next to it in xenos::renderer.
namespace xenos::renderer::gpu {

enum class ShaderIL : uint32_t { kDxil, kSpirv };
enum class ShaderStage : uint32_t { kVertex, kPixel, kGeometry, kCompute };

enum class Format : uint32_t {
  kUnknown,
  kR8G8B8A8Unorm,
  kR16G16B16A16Float,
  kR16G16Snorm,         // colour targets k_16_16 (M2 build B)
  kR16G16B16A16Snorm,   // k_16_16_16_16
  kR32Uint,
  kR32Float,
  kR32G32B32A32Float,
  kR8Unorm,
  // Texture-cache formats (M2). Those with a signed twin are created typeless so a signed view can be made.
  kR8G8Unorm,
  kR16Unorm,
  kR16G16Unorm,
  kR16G16B16A16Unorm,
  kR16Float,
  kR16G16Float,
  kR32G32Float,
  kR10G10B10A2Unorm,
  kB5G6R5Unorm,
  kB5G5R5A1Unorm,
  kB4G4R4A4Unorm,
  kBC1,
  kBC2,
  kBC3,
  kBC4,
  kBC5,
  kD32FloatS8X24,  // float depth + 8-bit stencil: D24FS8 targets (D29), and D24S8 with the exact-math option
  kD24UnormS8,     // D24S8 targets by default: the same host format as the reference (Canary/SDK RTV path), so
                   // depth quantisation and depth bias units match it bit for bit (job 097: +-1 otherwise)
};
constexpr bool IsDepthFormat(Format f) { return f == Format::kD32FloatS8X24 || f == Format::kD24UnormS8; }

enum TextureUsage : uint32_t {
  kUsageSampled = 1u << 0,
  kUsageRenderTarget = 1u << 1,
  kUsageDepthStencil = 1u << 2,
  kUsageStorage = 1u << 3,  // UAV / storage image
};

enum class TextureDimension : uint32_t { k2D, k3D, kCube };

struct TextureDesc {
  TextureDimension dimension = TextureDimension::k2D;
  uint32_t width = 1, height = 1, depth_or_layers = 1, mips = 1;
  Format format = Format::kUnknown;
  uint32_t samples = 1;
  uint32_t usage = kUsageSampled;
  // Depth/stencil textures start cleared to this (D3D12 optimised clear value); colour ignores it.
  float clear_depth = 1.0f;
  const char* debug_name = nullptr;
};

// Handles are small integers owned by the backend; 0 means "none".
using TextureHandle = uint32_t;
using ShaderHandle = uint32_t;
using PipelineHandle = uint32_t;
using ReadbackHandle = uint32_t;
using BufferHandle = uint32_t;  // a persistent GPU index buffer (upl-n108)
using OcclusionHandle = uint32_t;  // a host occlusion query (Stage 4, upl-s4c)

// Values are the Xenos encodings (xenos::CompareFunction / StencilOp), so registers convert by a cast.
enum class CompareFunc : uint8_t { kNever, kLess, kEqual, kLessEqual, kGreater, kNotEqual, kGreaterEqual, kAlways };
enum class StencilOp : uint8_t { kKeep, kZero, kReplace, kIncrSat, kDecrSat, kInvert, kIncrWrap, kDecrWrap };
enum class CullMode : uint8_t { kNone, kFront, kBack };
enum class Topology : uint8_t { kPointList, kLineList, kLineStrip, kTriangleList, kTriangleStrip };

struct StencilFace {
  StencilOp fail = StencilOp::kKeep, depth_fail = StencilOp::kKeep, pass = StencilOp::kKeep;
  CompareFunc func = CompareFunc::kAlways;
  bool operator==(const StencilFace&) const = default;
};

// Blending (M2). The reference's D3D12 path has no separate constant-alpha factor: the guest's constant alpha uses
// BLEND_FACTOR / INV_BLEND_FACTOR (SDK pipeline_cache.cpp kBlendFactorMap), and so do we.
enum class BlendFactor : uint8_t {
  kZero, kOne, kSrcColor, kInvSrcColor, kSrcAlpha, kInvSrcAlpha, kDestColor, kInvDestColor, kDestAlpha,
  kInvDestAlpha, kBlendFactor, kInvBlendFactor, kSrcAlphaSat
};
enum class BlendOp : uint8_t { kAdd, kSubtract, kMin, kMax, kRevSubtract };
struct BlendDesc {
  bool enable = false;
  BlendFactor src = BlendFactor::kOne, dst = BlendFactor::kZero;
  BlendOp op = BlendOp::kAdd;
  BlendFactor src_alpha = BlendFactor::kOne, dst_alpha = BlendFactor::kZero;
  BlendOp op_alpha = BlendOp::kAdd;
  bool operator==(const BlendDesc&) const = default;
};

struct GraphicsPipelineDesc {
  ShaderHandle vs = 0, ps = 0;  // ps 0 = no pixel shader (depth-only)
  // Optional geometry shader, e.g. the renderer's rectangle-list expansion (3 vertices per rectangle -> a 4-vertex
  // strip, 4th = -v[a] + v[b] + v[c], xenos/primitives.h); its HLSL is the renderer's, compiled by the shader cache.
  ShaderHandle gs = 0;
  Topology topology = Topology::kTriangleList;
  bool primitive_restart = false;  // strips: index 0xFFFFFFFF cuts (the caller rewrites the guest reset index)
  // Rasteriser.
  CullMode cull = CullMode::kNone;
  bool front_ccw = false;
  bool wireframe = false;
  int32_t depth_bias = 0;
  float depth_bias_slope = 0.0f;
  bool depth_clip = true;
  // Depth / stencil.
  bool depth_enable = false, depth_write = false;
  CompareFunc depth_func = CompareFunc::kAlways;
  bool stencil_enable = false;
  uint8_t stencil_read_mask = 0xFF, stencil_write_mask = 0xFF;
  StencilFace stencil_front, stencil_back;
  // Outputs: up to 5 slots (0-3 the guest's colour targets, 4 the shadow renderer's draw-ID diagnostic). A slot with
  // format kUnknown is unused (bound to a null view).
  static constexpr uint32_t kMaxColor = 5;
  uint32_t color_count = 0;
  Format color_formats[kMaxColor] = {};
  uint8_t color_write_masks[kMaxColor] = {};  // RGBA bits
  BlendDesc color_blend[kMaxColor] = {};
  Format depth_format = Format::kUnknown;
  uint32_t samples = 1;
  bool color_bits = false;  // colour targets written through their UINT bits views (EDRAM transfers, upl-n108)
  bool operator==(const GraphicsPipelineDesc&) const = default;
};
uint64_t HashPipelineDesc(const GraphicsPipelineDesc& d);

struct Viewport {
  float x = 0, y = 0, width = 1, height = 1, min_depth = 0, max_depth = 1;
  bool operator==(const Viewport&) const = default;
};
struct Rect {
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // [x0, x1) x [y0, y1)
  bool empty() const { return x1 <= x0 || y1 <= y0; }
  bool operator==(const Rect&) const = default;
};

// Constant buffer slots of the binding contract.
enum ConstantSlot : uint32_t { kCbSystem = 0, kCbFloat = 1, kCbBoolLoop = 2, kCbFetch = 3, kCbCount = 4 };

// A shader-visible texture view for one translator slot (t0-t95). Component sources: 0-3 = the resource's R/G/B/A,
// 4 = constant 0, 5 = constant 1 (the Xenos swizzle encoding).
enum class ViewDimension : uint8_t { k2D, kCube, k3D };
struct TextureView {
  TextureHandle texture = 0;  // 0 = the default white texture of the slot's dimension
  uint8_t components[4] = {0, 1, 2, 3};
  bool signed_view = false;  // SNORM view of a typeless resource (formats with a signed twin)
  ViewDimension dimension = ViewDimension::k2D;
  uint8_t plane = 0;  // depth/stencil textures: 0 depth (float), 1 stencil (uint in .y)
  // upl-n108: the raw bits of a 16/32-bit float or SNORM16 render target (UINT view of its typeless resource), so EDRAM
  // transfers copy NaN payloads and -32768 exactly. Ignored for textures without such a view.
  bool bits_view = false;
  // With texture 0: a null view (reads 0,0,0,0) instead of the white default - what Canary binds when the shader's fetch
  // dimension doesn't match the texture's (notes/51 section 14, the Hive water).
  bool zero = false;
  bool operator==(const TextureView&) const = default;
};
enum class SamplerAddress : uint8_t { kWrap, kMirror, kClamp, kMirrorOnce, kBorder };
struct SamplerDesc {
  uint8_t mag_linear = 1, min_linear = 1, mip_linear = 1;
  uint8_t max_anisotropy = 0;  // 0/1 = off
  SamplerAddress address[3] = {SamplerAddress::kWrap, SamplerAddress::kWrap, SamplerAddress::kWrap};
  uint8_t border = 0;  // 0 transparent black, 1 opaque white, 2 opaque black
  float lod_bias = 0.0f, min_lod = 0.0f, max_lod = 1000.0f;
  bool operator==(const SamplerDesc&) const = default;
};

struct ComputeBindings {
  uint32_t constants[16] = {};
  uint32_t constant_count = 0;
  TextureHandle srv[4] = {};
  uint32_t srv_plane[4] = {};  // depth/stencil textures: 0 depth, 1 stencil
  TextureHandle uav_texture = 0;
  uint32_t uav_mip = 0;    // mip level of uav_texture (a typed view of its format)
  bool guest_t3 = false;   // t3 = the guest-memory raw buffer (ByteAddressBuffer) instead of srv[3]
  bool guest_u0 = false;   // takeover T2: u0 = the guest-memory raw buffer (RWByteAddressBuffer) instead of uav_texture
  // Job 047 (resolve-through): u1 = mip 0 of this texture (typed view of its format); 0 = a null view.
  TextureHandle uav2_texture = 0;
  // Patch 0021 (Canary texture_cache, D36): u0 = the backend's scratch raw buffer (RWByteAddressBuffer), sized and laid
  // out by Backend::PrepareScratchFor for one texture subresource, instead of uav_texture.
  bool scratch_u0 = false;
};

// Notes/68 stage V3: the Vulkan backend on someone else's device (the SDK's Vulkan provider, which the presenter uses), so
// our image can be written into the presenter's guest-output image on the same queue. Handles as void* (no Vulkan types
// in this header); lock / unlock guard the shared queue (Vulkan requires it externally synchronised).
struct VulkanExternal {
  void* instance = nullptr;         // VkInstance
  void* physical_device = nullptr;  // VkPhysicalDevice
  void* device = nullptr;           // VkDevice (created with dynamic rendering, synchronization2, timeline semaphores)
  uint32_t queue_family = 0;
  void* queue = nullptr;            // VkQueue
  void (*lock)(void* user) = nullptr;
  void (*unlock)(void* user) = nullptr;
  void* lock_user = nullptr;
  // What the owner enabled on the device (we may use only these): SDK patch 33 turns on what our backend needs.
  bool push_descriptor = false, depth_clamp = false, fill_mode_non_solid = false, anisotropy = false;
  bool texture_compression_bc = false;
};
// Notes/68 V3: the presenter's guest-output image for Backend::PresentPacked (Vulkan: VkImage + VkImageView, format
// A2B10G10R10_UNORM_PACK32, usable as a colour attachment). It is left in SHADER_READ_ONLY_OPTIMAL, visible to
// fragment-shader reads (the SDK VulkanPresenter's kGuestOutputInternal* contract); first_write: its contents and
// layout are undefined before.
struct VulkanPresentTarget {
  void* image = nullptr;
  void* view = nullptr;
  uint32_t width = 0, height = 0;
  bool first_write = false;
};

struct BackendOptions {
  bool debug_layer = false;  // only works if nothing else in the process has created a D3D12 device yet
  uint32_t upload_ring_mb = 256;
  uint32_t frames_in_flight = 2;
  uint64_t guest_memory_bytes = 512ull << 20;  // the Xenos physical address space (0x1FFFFFFF mask)
  // Stage 5C-2: run on someone else's device and queue (the SDK presenter's D3D12 provider: ID3D12Device* and its
  // direct ID3D12CommandQueue*), so our image can be written straight into the presenter's texture, in queue order.
  // Null: our own device and queue.
  void* external_device = nullptr;
  void* external_queue = nullptr;
  const VulkanExternal* vulkan_external = nullptr;  // notes/68 V3 (Vulkan backend only)
  // Command-list calls replayed by a recording thread (notes/52 section 10) instead of on the caller's thread.
  bool threaded_recording = false;
  // Job 171 (notes/52 section 13): guest-memory copies into the upload ring (UploadGuestMemory, UploadTransient) run
  // on this many copy threads instead of inline; they all finish at FlushCopies and before each submission. 0 = inline.
  uint32_t copy_threads = 0;
  // Saved pipelines (notes/64 section 'Shader warm-up'): a D3D12 pipeline library file. Pipelines already in it load in
  // ~1 ms on first use, synchronously, so their draws are never skipped as "pending"; new ones compile as before and are
  // stored. Saved every ~15 s (patch 0056) while it grows and at shutdown. Empty = off. A driver change starts a new library.
  std::string pipeline_library_path;
  // Patch 0010 (stutter): the periodic save serializes the whole library (tens of MB) and writes the file - on the
  // recording thread that froze a frame for 100-400 ms. true: a background thread does it (the library is
  // thread-safe, CreatePipelineLibrary docs). The save at shutdown stays where it is.
  bool pipeline_save_background = false;
  // Patch 0010: with asynchronous pipelines, a pipeline already in the library is loaded by a pipeline worker too
  // (~1 ms each) instead of on the recording thread; its draws are pending meanwhile, like a new pipeline's. A new area
  // loading hundreds at once stalled one frame for 100-300 ms.
  bool pipeline_load_async = false;
  // Patch 0056 (Canary pipeline_cache.cc d3d12_pipeline_creation_threads): pipeline worker threads. -1 = 75% of the
  // logical processors (Canary's default), 0 = the old count (a quarter, at most 4), N = N (at most the processors).
  int pipeline_threads = -1;
  // Patch 0162: the workers' Windows thread priority (-2 lowest, -1 below normal, 0 normal).
  int pipeline_thread_priority = 0;  // job 518: normal by default, as Canary / Edge
  uint32_t warm_workers = 3;  // patch 0069: workers allowed on warm-only pipelines at once (WarmPixelShader)
  // Prebuild v2: a worker's Windows thread priority while it builds a warm-only pipeline (-2 lowest, -1 below normal,
  // 0 normal); back to pipeline_thread_priority after. Speculative builds then yield the CPU to the game, the renderer,
  // the audio and the draws' own pipelines instead of competing with them.
  int warm_thread_priority = -2;
  // Patch 0056: seconds between periodic library saves while it grows (was 60; a quit lost minutes of pipelines).
  int pipeline_save_seconds = 15;
  // Patch 0017 (stutter): sampled-only textures (no render-target / depth use, no MSAA) up to 16 MB are placed in
  // shared 64 MB heaps instead of each getting its own committed allocation. Job 299: CreateCommittedResource was 157
  // of the 208 ms that 376 new textures cost at the ch10 level start (~0.4 ms each).
  bool placed_textures = false;
  // Patch 0150 (deep-dive review): persistent index buffers (CreateIndexBuffer) live in GPU memory, copied once from an
  // upload buffer, instead of an upload-heap buffer the GPU reads over the bus at every draw (D3D12 only).
  bool index_buffers_in_vram = false;
  // Patch 0087 (Vulkan only): colour render targets whose format has other view formats (signed, UINT bits) are made
  // as plain, compressible images; 0 = mutable from the start (the old way), 1 = made mutable (contents copied) the
  // first time a view of another format is needed, 2 = diagnostic: never - such views read the image's own format.
  uint32_t vk_mutable_targets = 0;
  // Patch 0088 (Vulkan, our own device only - gn_vktest --bench): turn on robustBufferAccess, as the SDK's device does
  // with GPU emulation, to measure what its bounds checks cost.
  bool vk_robust_buffer_access = false;
  // Patch 0089 (Vulkan, stage V5b): 0 = a full barrier at every render pass start (as before); 1 = only when something
  // since the last barrier could conflict with the pass (outside-pass GPU work, memory export, its attachments sampled
  // or written since).
  uint32_t vk_pass_barriers = 0;  // patch 0115: 2 = as 1, and when the only conflict is that the pass's attachments
                                  // were drawn to since, a barrier between attachment writes only (not a full one)
  // Patch 0092 (Vulkan): clears of the current targets inside the open render pass (vkCmdClearAttachments).
  bool vk_clear_in_pass = false;
  // Patch 0097 (Vulkan): images the driver prefers in their own allocation (VkMemoryDedicatedRequirements: NVIDIA says
  // so for render targets) get one (VkMemoryDedicatedAllocateInfo), as D3D12's committed resources are.
  bool vk_dedicated_images = false;
  // Patch 0098 (Vulkan, a measurement with the GPU timestamps): passed samples per render-target slot.
  bool vk_count_pixels = false;
  // Patch 0100 (Vulkan): render passes draw to their attachments in COLOR_ / DEPTH_STENCIL_ATTACHMENT_OPTIMAL (as
  // Canary does) instead of GENERAL; they go back to GENERAL in the next full barrier outside a pass.
  bool vk_attachment_layouts = false;
  // Patch 0104 (Vulkan, own device only - gn_vktest): enable VK_KHR_pipeline_executable_properties and capture each
  // graphics pipeline's driver statistics (registers, instructions...) for Backend::PipelineStatistics.
  bool vk_pipeline_stats = false;
  // Patch 0107 (Vulkan): the barriers' source stages are the kinds of GPU work recorded since the last barrier
  // (graphics / compute / transfer) instead of ALL_COMMANDS; the destination stays ALL_COMMANDS.
  bool vk_barrier_src_stages = false;
  // Patch 0108 (Vulkan, a MEASUREMENT mode - may give wrong pictures): memory-only barriers are not sent (image
  // layout transitions still are, with full stages): how much of Vulkan's GPU time the barriers cost.
  bool vk_skip_memory_barriers = false;
  // Patch 0109 (both APIs, a measurement switch): false = GPU timestamps only at the start and end of each command
  // list (whole-frame GPU time, no split by category / target / draw), so the timestamps can't change the timing.
  bool gpu_timestamp_split = true;
};

// Patch 0101 (the draw report): the GPU time of one stretch between two timestamps whose tag (SetGpuTag) was not 0;
// category = the SetGpuCategory value of that stretch (0x20-0x3F draws into a target slot, 1 transfers, 2 resolves...).
struct GpuTagTime {
  uint32_t tag = 0;
  uint8_t category = 0;
  double ms = 0.0;
  double samples = -1.0;  // patch 0102: samples that passed depth / stencil (an entry of its own, ms 0); -1 = a time
};

struct BackendStats {
  uint64_t submits = 0, draws = 0, dispatches = 0, pipelines = 0, upload_bytes = 0, guest_upload_bytes = 0;
  uint64_t transient_upload_bytes = 0;  // UploadTransient
  uint64_t memexport_draws = 0;  // Gears 3 notes/11: draws with the guest-memory buffer writable (SetGuestWritable)
  uint64_t copies_deferred = 0, copy_deferred_bytes = 0, copy_flushes = 0;  // job 171: copy threads
  double gpu_ms = 0.0;           // job 176: GPU time of our command lists (first to last timestamp)
  uint64_t gpu_lists_timed = 0;
  double gpu_cat_ms[8] = {};     // job 177: GPU time by category (SetGpuCategory)
  double gpu_target_ms[32] = {};
  double gpu_target_px[32] = {};  // patch 0098 (Vulkan): samples that passed depth / stencil, by render-target slot  // job 234: draw time by render-target slot (categories 0x20-0x3F, also counted as draws)
  uint64_t index_draws_reused = 0;  // job 178: DrawIndexedAt draws
  uint64_t ring_waits = 0;  // times recording waited for the GPU to free upload ring space
  uint64_t texture_upload_bytes = 0, sampler_table_waits = 0, constant_uploads_skipped = 0;
  uint64_t scratch_texture_copies = 0;
  uint64_t early_submits = 0;  // patch 0138: mid-frame submits (gow2_renderer_submit_draws)
  uint64_t texture_uploads_refused = 0;
  uint64_t redone_after_submit = 0;  // patch 0029: draws whose textures/constants were rebuilt after a forced submit  // patch 0024: uploads into a mip / layer the texture doesn't have  // patch 0021: GPU-loaded BCn subresources copied in from the scratch buffer
  uint64_t textures_placed = 0, texture_heaps = 0;  // patch 0017: placed textures created, 64 MB heaps made
  uint64_t vk_targets_made_mutable = 0, vk_views_refused = 0;  // patch 0087 (Vulkan)
  uint64_t depth_decompressions = 0;  // patch 0089 (D3D12, MSAA depth with programmable sample positions)
  uint64_t vk_dedicated_images = 0, vk_images = 0;  // patch 0097 (Vulkan): images made / in a dedicated allocation
  uint64_t vk_passes = 0, vk_full_barriers = 0;  // patch 0089 (Vulkan): render passes begun, full barriers queued
  uint64_t vk_attachment_layout_changes = 0;     // patch 0100 (Vulkan): attachments moved into an attachment layout
  uint64_t vk_barriers_skipped = 0;              // patch 0089 (Vulkan): pass starts that needed no barrier
  uint64_t vk_barriers_attachment = 0;           // patch 0115 (Vulkan): pass starts with an attachment-only barrier
  uint64_t vk_clears_in_pass = 0;                // patch 0092 (Vulkan): clears done inside the open pass
  uint64_t vk_barrier_src[5] = {};  // patch 0107 (Vulkan): barrier calls by source: graphics, compute, transfer, mixed, all
  uint64_t vk_pass_end[10] = {};  // patch 0089 (Vulkan): why render passes ended (vulkan_backend.cpp EndPass)
  uint64_t fence_waits = 0;      // WaitForFence calls that really blocked
  double fence_wait_ms = 0.0;    // CPU time blocked in them (our GPU behind, ring full, readbacks)
  uint64_t texture_tables_reused = 0, state_calls_skipped = 0;
  uint64_t descriptors_allocated = 0, descriptor_ring_waits = 0;
  // Blocking waits by site (upl-n108): 0 renderer (frame pacing, comparisons), 1 readback release, 2 sampler table,
  // 3 upload ring, 4 descriptor ring, 5 other.
  uint64_t wait_count[6] = {};
  double wait_ms[6] = {};
  double pipeline_create_ms = 0.0;  // CPU time in CreateGraphicsPipelineState on the recording thread
  uint64_t barrier_calls = 0, barriers = 0;  // job 045: ResourceBarrier calls / barriers recorded (updated at Submit)
  uint64_t barrier_batches = 0, barriers_merged = 0;  // job 046: batches emitted, transitions merged away
  uint64_t pipelines_async = 0, pipelines_pending_draws = 0, sampler_tables_written = 0;
  uint64_t pipelines_loaded = 0, pipelines_stored = 0, pipeline_library_saves = 0;  // saved pipelines (notes/64)
  uint64_t pipelines_loaded_async = 0;  // patch 0010: of pipelines_loaded, loaded by a pipeline worker
  // Patch 0057: pipelines the workers created (not loaded): count, CPU ms in CreateGraphicsPipelineState (all workers
  // summed), the slowest one, their VS+PS+GS bytecode bytes, per priority (PipelinePriority), and jobs still queued.
  uint64_t pipelines_built_async = 0, pipeline_async_bytes = 0, pipeline_jobs_queued = 0;
  double pipeline_async_ms = 0.0, pipeline_async_max_ms = 0.0;
  uint64_t pipelines_built_by_priority[5] = {};
  // Patch 0062: start-up backlog jobs moved to the front because a draw (or a waiting user) needed them now.
  uint64_t pipelines_promoted = 0;
  // Patch 0068: warm-only pipelines (WarmPixelShader) built and released, failed, and the driver's ms for them.
  uint64_t pipelines_warmed = 0, pipelines_warm_failed = 0, pipelines_warm_loaded = 0;
  double pipeline_warm_ms = 0.0;
  uint64_t cached_stage_prefetches = 0, cached_stage_restores = 0;
  double cached_stage_restore_ms = 0.0;
  uint64_t sampler_heap_switches = 0;  // patch 0034: a full sampler heap swapped for a free one instead of waiting
  uint64_t sampler_table_hits = 0;                         // upl-n109: sampler sets found in the heap's cache
  uint64_t texture_table_hits = 0, texture_table_misses = 0;  // upl-n109: texture descriptor tables reused / written
  uint64_t index_buffers = 0, index_buffer_bytes = 0, index_buffer_draws = 0;  // persistent index buffers (live)
  // Record profile (upl-n108): CPU ms inside the draw calls' state application, and the two costly parts of it.
  double prepare_draw_ms = 0.0, apply_textures_ms = 0.0, upload_constants_ms = 0.0;
  // Occlusion queries (upl-s4c): begun, extra segments (a Submit inside a query), refused (pool full / one already
  // open / not recording).
  uint64_t occlusion_queries = 0, occlusion_segments = 0, occlusion_refused = 0;
};

class Backend {
 public:
  virtual ~Backend() = default;

  virtual bool Init(const BackendOptions& options, std::string& error) = 0;
  virtual std::string Describe() const = 0;
  virtual ShaderIL GetShaderIL() const = 0;
  // Pixel shaders can output the stencil reference (SV_StencilRef), so one pass writes all 8 stencil bits.
  virtual bool SupportsStencilRefExport() const { return false; }
  // Device removed / hung: everything after that is a no-op, and the reason is in the string.
  virtual bool IsLost(std::string* reason = nullptr) = 0;

  // ---- resources ----
  virtual TextureHandle CreateTexture(const TextureDesc& desc) = 0;
  // Released once the GPU has finished every submission recorded so far.
  virtual void DestroyTexture(TextureHandle texture) = 0;
  // Stage 5C-2: a texture owned by someone else on our device (D3D12: an ID3D12Resource* in `state_now`, a
  // D3D12_RESOURCE_STATES value), usable as a UAV / SRV like ours. ReleaseImport records a transition to
  // `final_state` and forgets the handle (the owner keeps the resource).
  virtual TextureHandle ImportTexture(void* native_resource, Format format, uint32_t state_now) = 0;
  virtual void ReleaseImport(TextureHandle texture, uint32_t final_state) = 0;
  // Notes/68 V3: our packed present words (an R32 uint texture: R, G, B in 10 bits each, as kPresentUnpackCs reads
  // them) drawn into someone else's image - the presenter's guest output, which can't be a storage image on Vulkan.
  // Recorded into the current command list (the caller submits). false = not supported / failed (D3D12: not used;
  // it imports the presenter's resource and dispatches the unpack instead).
  virtual bool PresentPacked(TextureHandle src, uint32_t width, uint32_t height, const void* target) {
    (void)src, (void)width, (void)height, (void)target;
    return false;
  }
  virtual ShaderHandle CreateShader(ShaderStage stage, const void* code, size_t size) = 0;
  // Cached by the full description; 0 on failure (logged once per description).
  // async: create on a worker thread; returns 0 with *pending = true until it is ready (upl-n108).
  virtual PipelineHandle GetGraphicsPipeline(const GraphicsPipelineDesc& desc, bool async = false,
                                             bool* pending = nullptr) = 0;
  // Patch 0058: GetGraphicsPipeline calls that did not find the description already made or queued (new to this run).
  virtual uint64_t PipelineMisses() const { return 0; }
  // Patch 0132: wait up to max_ms for an asynchronously queued pipeline to be built by a worker (never builds it on the
  // calling thread); true = it is ready now. Backends without workers: false.
  virtual bool WaitGraphicsPipeline(const GraphicsPipelineDesc& desc, double max_ms) { (void)desc; (void)max_ms; return false; }
  // Patch 0058: while on, asynchronous requests are start-up backlog (the recorded pipelines of earlier sessions): the
  // workers take them after every pipeline a live draw is waiting for.
  virtual void SetPipelineBacklog(bool on) { (void)on; }
  // Patch 0061 ("Preparing shaders"): asynchronous pipeline jobs not finished yet (queued + being built), and a save of
  // the saved-pipeline library now (on this thread) if it has new entries.
  virtual uint64_t PipelineJobsLeft() { return 0; }
  // Patch 0063: true while the asynchronous pipeline for this description is still being built (a waiting start-up
  // backlog job is moved to the front, as for a draw); false when ready, failed or never asked for.
  virtual bool PipelinePending(const GraphicsPipelineDesc& d) { (void)d; return false; }
  // Patch 0068 (shaders built while the game loads an area): a pipeline made only to put one pixel shader through the
  // driver, then released at once - not kept, not cached, not saved to the pipeline library. Job 362c [V, RTX 3080 Ti]:
  // the driver keeps each compiled shader stage, so a later pipeline with this pixel shader (any vertex shader, any
  // state) costs ~0.1 ms instead of 15-35 ms. SetWarmTemplate (the recording thread): the vertex shader and state those
  // pipelines use, once; false until it could be set. WarmPixelShader (any thread): queue one, built by the pipeline
  // workers after every pipeline a draw is waiting for and before the start-up backlog. WarmJobsLeft: queued + being
  // built. All false / 0 on a backend without it.
  virtual bool SetWarmTemplate(const GraphicsPipelineDesc& d) { (void)d; return false; }
  virtual bool WarmPixelShader(std::vector<uint8_t>&& ps_bytecode, uint64_t source_hash = 0) { (void)ps_bytecode; (void)source_hash; return false; }
  virtual bool WarmVertexShader(std::vector<uint8_t>&& bytecode, uint64_t source_hash = 0) { (void)bytecode; (void)source_hash; return false; }
  // Recording thread: restore a known cached stage on a background worker while
  // the engine loads its asset, before a draw needs a new state combination.
  virtual bool PrefetchShaderStage(ShaderHandle shader) { (void)shader; return false; }
  virtual uint64_t WarmJobsLeft() { return 0; }
  virtual void WaitWarmCapacity(uint64_t capacity) { (void)capacity; }
  virtual void ReleaseWarmupCache() {}
  virtual void ConfigureWarmSourceCache(const std::string& identity) { (void)identity; }
  virtual bool WarmSourceCached(bool vertex, uint64_t hash) { (void)vertex; (void)hash; return false; }
  virtual uint64_t WarmSourcesReused() const { return 0; }
  // Prebuild v2 (any thread): how many workers may be on warm-only pipelines from now on; 0 = none (queued warm jobs
  // wait). The renderer raises it while the game loads and lowers it during play.
  virtual void SetWarmWorkers(uint32_t n) { (void)n; }
  virtual void SavePipelinesNow() {}
  virtual PipelineHandle CreateComputePipeline(ShaderHandle cs) = 0;

  // ---- guest memory mirror (t96) ----
  // Copies host bytes (guest byte order, as in guest memory) into the GPU copy of guest physical memory.
  virtual void UploadGuestMemory(uint32_t guest_physical, const void* data, uint32_t size) = 0;
  // Copies host bytes into the upload ring, where shaders read them directly (t97) - no copy into the mirror, so no
  // barrier around it (job 159: two barriers per draw serialised the GPU in ch10). Valid for draws recorded until the
  // next submission plus the ring's lap. Returns the byte offset in the ring, or UINT32_MAX (then use the mirror).
  virtual uint32_t UploadTransient(const void* data, uint32_t size) { (void)data; (void)size; return UINT32_MAX; }
  // Job 171: with copy threads, the two calls above may copy later - `data` (guest memory) must stay unchanged until
  // FlushCopies or the next Submit (which flushes). FlushCopies returns once every deferred copy is done.
  virtual void FlushCopies() {}
  // Job 177: the same constants for the vertex and the pixel shader (system, bool/loop, fetch): one compare, one upload,
  // both root slots point at it. Default: two SetConstants.
  virtual void SetConstantsBoth(uint32_t slot, const void* data, uint32_t size) {
    SetConstants(ShaderStage::kVertex, slot, data, size);
    SetConstants(ShaderStage::kPixel, slot, data, size);
  }
  // Job 177: GPU work recorded from now on is timed under this category (0 draws, 1 EDRAM transfers, 2 resolves,
  // 3 texture loads, 4 frame / present, 5 other). A timestamp is written only when GPU work of a new category starts.
  virtual void SetGpuCategory(uint8_t category) { (void)category; }
  // Patch 0101 (the draw report): work recorded after this carries the tag (0 = none); a new tag starts a new timestamp
  // stretch. TakeGpuTagTimes hands over (and forgets) the tagged stretches read back so far, in recording order.
  virtual void SetGpuTag(uint32_t tag) { (void)tag; }
  virtual void TakeGpuTagTimes(std::vector<GpuTagTime>& out) { out.clear(); }
  // Patch 0102: tagged draws also count the samples that pass depth / stencil (Vulkan: precise occlusion queries).
  virtual void SetGpuTagSamples(bool on) { (void)on; }
  // Patch 0101: one line about a texture as the API holds it (size, format, native flags), for the draw report.
  virtual std::string DescribeTexture(TextureHandle texture) const { (void)texture; return {}; }
  // Patch 0124: the description a texture was created with (false: no such texture), for the target-stats tool.
  virtual bool TextureInfo(TextureHandle texture, TextureDesc& out) const { (void)texture; (void)out; return false; }
  // Patch 0104: the driver's statistics of a graphics pipeline, one "stage: name value, ..." line per executable
  // (empty when not captured - BackendOptions::vk_pipeline_stats).
  virtual std::string PipelineStatistics(PipelineHandle pipeline) { (void)pipeline; return {}; }

  // ---- graphics recording ----
  // bits_view: bind the colour targets' UINT bits views (with a pipeline whose color_bits is set).
  virtual void SetRenderTargets(uint32_t color_count, const TextureHandle* colors, TextureHandle depth,
                                bool bits_view = false) = 0;
  virtual void ClearDepthStencil(TextureHandle depth, bool clear_depth, float depth_value, bool clear_stencil,
                                 uint8_t stencil_value, const Rect* rects, uint32_t rect_count) = 0;
  virtual void SetViewport(const Viewport& viewport) = 0;
  virtual void SetScissor(const Rect& scissor) = 0;
  virtual void SetStencilRef(uint8_t ref) = 0;
  // Patch 0089 (Canary cc4981a, --gow2_renderer_msaa_sample_positions=xenos): rasterize MSAA draws at these sample
  // positions (1/16 pixel units, `count` of them; count 0 = the API's standard pattern). Kept until changed. Backends
  // without programmable sample positions ignore it (SamplePositionsSupported() false).
  virtual void SetSamplePositions(uint32_t count, const int8_t (*positions)[2]) { (void)count; (void)positions; }
  virtual bool SamplePositionsSupported() const { return false; }
  // Gears 3 (notes/11): the following draws memexport - their shaders write the guest-memory buffer through u0 (and
  // read vertex data through it). Off again with false.
  virtual void SetGuestWritable(bool writable) { (void)writable; }
  virtual void SetBlendConstant(const float rgba[4]) = 0;
  virtual void ClearColor(TextureHandle texture, const float rgba[4], const Rect* rects = nullptr,
                          uint32_t rect_count = 0) = 0;
  virtual void SetPipeline(PipelineHandle pipeline) = 0;
  // Copies `size` bytes into the upload ring and binds them to slot b<slot> of one stage (vertex or pixel).
  virtual void SetConstants(ShaderStage stage, uint32_t slot, const void* data, uint32_t size) = 0;
  // The same, with the slot's data made of two pieces placed back to back (compacted float constants: the low block a
  // shader reads, then its high block). Default: gathered into one buffer, then SetConstants.
  virtual void SetConstantsSplit(ShaderStage stage, uint32_t slot, const void* a, uint32_t a_size, const void* b,
                                 uint32_t b_size) {
    std::vector<uint8_t> v(size_t(a_size) + b_size);
    if (a_size) std::memcpy(v.data(), a, a_size);
    if (b_size) std::memcpy(v.data() + a_size, b, b_size);
    SetConstants(stage, slot, v.data(), uint32_t(v.size()));
  }
  // Texture views for t0-t95 (index = register; the binding contract puts 2D at 0-31, cube at 32-63, 3D at 64-95) and
  // samplers s0-s31, for the following draws. Null = the static defaults (white textures, linear/wrap samplers).
  // view_count: slots [0, view_count) the shaders may read (the table is only that long; upl-n107). The guest-memory
  // buffer (t96) is bound separately and always.
  // sampler_count: samplers the shaders read (s0..count-1); <= 4 uses a small sampler table (upl-n108).
  virtual void SetTextures(const TextureView* views96, const SamplerDesc* samplers32, uint32_t view_count = 96,
                           uint32_t sampler_count = 32) = 0;
  // Writes one mip/layer of a texture (tightly packed rows, `row_bytes` apart; for 3D `slices` depth slices of
  // `rows` rows each). Block-compressed formats count rows of blocks.
  virtual void UploadTexture(TextureHandle texture, uint32_t mip, uint32_t layer, const void* data, uint32_t row_bytes,
                             uint32_t rows, uint32_t slices) = 0;
  virtual void Draw(uint32_t vertex_count) = 0;
  // 32-bit host-order indices (the caller converted the guest index buffer); copied into the upload ring.
  virtual void DrawIndexed(const uint32_t* indices, uint32_t index_count) = 0;
  // Job 178: as DrawIndexed, and returns the indices' offset in the upload ring (UINT64_MAX if not drawn) - valid for
  // draws in the same command list (Stats().submits unchanged).
  virtual uint64_t DrawIndexedKeep(const uint32_t* indices, uint32_t index_count) {
    DrawIndexed(indices, index_count);
    return UINT64_MAX;
  }
  // Job 178: draws indices already in the upload ring from DrawIndexedKeep in command list `list`; false (nothing drawn)
  // if a new list has begun meanwhile - the caller then draws with DrawIndexed.
  virtual bool DrawIndexedAt(uint64_t offset, uint32_t index_count, uint64_t list) {
    (void)offset; (void)index_count; (void)list;
    return false;
  }
  // Once per guest frame, after that frame's Submit (upl-n109): rotates the per-frame descriptor resources (sampler
  // heaps), waiting only if the frame that last used the next one is still on our GPU.
  virtual void NextFrame() = 0;
  // Persistent index buffers for index data that stays the same over many frames (upl-n108): written once, drawn from
  // without the per-draw copy into the upload ring. Release defers the free until our GPU is past its last use.
  virtual BufferHandle CreateIndexBuffer(const uint32_t* indices, uint32_t index_count) = 0;  // 0 = failed
  virtual void ReleaseIndexBuffer(BufferHandle buffer) = 0;
  virtual void DrawIndexedBuffer(BufferHandle buffer, uint32_t index_count) = 0;

  // ---- occlusion queries (Stage 4, notes/46; upl-s4c) ----
  // One query open at a time, counting the samples that pass depth/stencil for the draws recorded between Begin and
  // End (D3D12_QUERY_TYPE_OCCLUSION, exact counts). A Submit inside a query is allowed: the backend ends that part,
  // resolves it, and continues in the next command list; the result is the sum. Never waits: Begin returns 0 when the
  // pool has no free slot (or a query is already open), and the caller answers some other way.
  virtual OcclusionHandle BeginOcclusionQuery() = 0;
  virtual void EndOcclusionQuery(OcclusionHandle query) = 0;
  // True once every part has completed on our GPU; *samples = the sum. Never waits.
  virtual bool GetOcclusionResult(OcclusionHandle query, uint64_t* samples) = 0;
  // The slots come back once our GPU is past them (no wait).
  virtual void ReleaseOcclusionQuery(OcclusionHandle query) = 0;

  // ---- compute ----
  virtual void Dispatch(PipelineHandle pipeline, const ComputeBindings& bindings, uint32_t groups_x,
                        uint32_t groups_y, uint32_t groups_z) = 0;

  // ---- readback ----
  // Queues a copy of a region of a colour texture (not depth: resolve depth into an R32 texture first).
  virtual ReadbackHandle ReadbackTexture(TextureHandle texture, const Rect& region) = 0;
  // Gears 3 (notes/11): queues a copy of [address, address + size) of the guest-memory buffer (bytes as stored, i.e.
  // guest byte order) - memexport results going back to guest memory. Row pitch reported = size. 0 = unsupported.
  virtual ReadbackHandle ReadbackGuest(uint32_t address, uint32_t size) { (void)address; (void)size; return 0; }
  // Patch 0021: GPU loads into formats a UAV can't store (BCn). PrepareScratchFor sizes the scratch buffer for one
  // subresource (mip + layer) and returns its copy layout (row pitch in bytes, rows = block rows for BCn); a Dispatch
  // with ComputeBindings::scratch_u0 writes the subresource's bytes there; CopyScratchToTexture copies them in.
  virtual bool PrepareScratchFor(TextureHandle texture, uint32_t mip, uint32_t layer, uint32_t& row_pitch,
                                 uint32_t& rows) {
    (void)texture; (void)mip; (void)layer; row_pitch = rows = 0;
    return false;
  }
  virtual void CopyScratchToTexture(TextureHandle texture, uint32_t mip, uint32_t layer) {
    (void)texture; (void)mip; (void)layer;
  }
  // True once the submission containing the copy has completed (never waits).
  virtual bool IsReadbackReady(ReadbackHandle readback) = 0;
  virtual const uint8_t* ReadbackData(ReadbackHandle readback, uint32_t& row_pitch_bytes) = 0;
  virtual void ReleaseReadback(ReadbackHandle readback) = 0;

  // ---- submission ----
  // Closes and submits what was recorded, returns its fence value.
  virtual uint64_t Submit() = 0;
  // Patch 0138: a mid-frame submit so the GPU starts on the first part of the frame while the rest is recorded. It
  // does not wait for the recording thread to hand the list over (Submit does, for presentation). Default: Submit.
  virtual uint64_t SubmitEarly() { return Submit(); }
  virtual uint64_t CompletedFence() = 0;
  virtual void WaitForFence(uint64_t fence) = 0;
  // Debug markers (PIX / RenderDoc), no-ops when unsupported.
  virtual void BeginEvent(const char* name) = 0;
  virtual void EndEvent() = 0;

  // Vendor SDKs (DLSS / FSR, Gears 2 notes/64; Gears 3 job 052): direct access to the native objects, D3D12 only.
  // BeginNative opens the command list if needed, emits queued barriers and waits until the recording thread
  // (threaded_recording) has replayed everything queued so far, and returns the device (ID3D12Device*) and the real
  // command list (ID3D12GraphicsCommandList*), which the caller may record into until EndNative. NativeTexture (only
  // between Begin and End) moves a texture to `state` (D3D12_RESOURCE_STATES) with a barrier recorded straight into
  // that list and returns its ID3D12Resource*. EndNative re-applies our descriptor heaps, root signature and root views
  // and marks every binding dirty (the SDK changed them).
  virtual bool BeginNative(void** device, void** command_list) { (void)device; (void)command_list; return false; }
  virtual void* NativeTexture(TextureHandle texture, uint32_t state) { (void)texture; (void)state; return nullptr; }
  virtual void EndNative() {}
  // Patch 0032: this process's local video memory use and the OS budget for it (DXGI QueryVideoMemoryInfo). Use over
  // budget means the driver pages resources in and out - everything on the GPU slows down.
  virtual bool VideoMemory(uint64_t* used, uint64_t* budget) { (void)used; (void)budget; return false; }
  virtual const BackendStats& Stats() const = 0;
};

// Null on platforms without D3D12.
std::unique_ptr<Backend> CreateD3D12Backend();
// Notes/68 (stage V1): null when built without Vulkan headers (vulkan_backend.cpp). The loader is opened at Init.
std::unique_ptr<Backend> CreateVulkanBackend();
// Patch 0006 (Gears 3, 2026-10-23): D3D12 DRED (auto-breadcrumbs + page-fault reporting) for every device created
// after this call - call it before the presenter's device exists. Works on the release runtime (Windows 10 1903+), no
// debug layer. Returns false where unavailable. When a device is lost, the backend then logs what the GPU was doing.
bool EnableDeviceLossDiagnostics();

}  // namespace xenos::renderer::gpu
