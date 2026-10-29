// Layer 0 (game-free): the D3D12 implementation of xenos/renderer/gpu.h (S3.5 step 1, notes/42 section 4 item 1):
// device, direct queue, fence, command allocator pool, one shader-visible CBV/SRV/UAV heap (static graphics table +
// a ring for per-dispatch tables), a sampler heap, CPU heaps for RTV/DSV/SRV/UAV, a fence-tracked upload ring
// (constants, indices, guest-memory uploads), readback buffers, the GPU copy of guest physical memory, a pipeline cache.
//
// Our own device, next to the SDK's emulated one in the same process (two devices on one adapter). Nothing here
// touches the SDK's device or globals. The debug layer is never enabled from here: enabling it after another device
// exists removes that device. With --d3d12_debug (the SDK's switch, applied before any device exists) the layer is on
// process-wide, and Init(debug_layer = true) then forwards our device's messages to the log.
// Device creation follows tools/gn_shader_oracle_d3d12.cpp (works on the RTX 3080 Ti, Shader Model 6.6, job 088).
// Our code (MIT).
#include <xenos/renderer/gpu.h>
#include <xenos/renderer/mapped_file.h>
#include <xenos/renderer/queries.h>
#include <xenos/renderer/warm_source_cache.h>
#include <xenos/renderer/warm_stage_store.h>

#include <cstring>

namespace xenos::renderer::gpu {

uint64_t HashPipelineDesc(const GraphicsPipelineDesc& d) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
  };
  auto bits = [](float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
  };
  mix(d.vs); mix(d.ps); mix(d.gs);
  mix(uint64_t(d.topology) | uint64_t(d.primitive_restart) << 8 | uint64_t(d.cull) << 16 | uint64_t(d.front_ccw) << 24 |
      uint64_t(d.wireframe) << 32 | uint64_t(d.depth_clip) << 40);
  mix(uint32_t(d.depth_bias)); mix(bits(d.depth_bias_slope));
  mix(uint64_t(d.depth_enable) | uint64_t(d.depth_write) << 8 | uint64_t(d.depth_func) << 16 |
      uint64_t(d.stencil_enable) << 24 | uint64_t(d.stencil_read_mask) << 32 | uint64_t(d.stencil_write_mask) << 40);
  for (const StencilFace* f : {&d.stencil_front, &d.stencil_back})
    mix(uint64_t(f->fail) | uint64_t(f->depth_fail) << 8 | uint64_t(f->pass) << 16 | uint64_t(f->func) << 24);
  mix(d.color_count);
  for (uint32_t i = 0; i < GraphicsPipelineDesc::kMaxColor; ++i) {
    const BlendDesc& b = d.color_blend[i];
    mix(uint64_t(d.color_formats[i]) | uint64_t(d.color_write_masks[i]) << 32 | uint64_t(b.enable) << 40);
    mix(uint64_t(b.src) | uint64_t(b.dst) << 8 | uint64_t(b.op) << 16 | uint64_t(b.src_alpha) << 24 |
        uint64_t(b.dst_alpha) << 32 | uint64_t(b.op_alpha) << 40);
  }
  mix(uint64_t(d.depth_format) | uint64_t(d.samples) << 32 | uint64_t(d.color_bits) << 40);
  return h;
}

}  // namespace xenos::renderer::gpu

#ifndef _WIN32

namespace xenos::renderer::gpu {
std::unique_ptr<Backend> CreateD3D12Backend() { return nullptr; }
bool EnableDeviceLossDiagnostics() { return false; }
}  // namespace xenos::renderer::gpu

#else

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>

#include <rex/logging/macros.h>
#include <rex/hash.h>  // XXH3 (pipeline library names)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xenos::renderer::gpu {
namespace {

template <typename T>
void SafeRelease(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

// Per-draw timing statistics use the time-stamp counter (job 169: steady_clock = QueryPerformanceCounter, up to six
// calls per draw, was ~2% of the GPU thread), converted with a ratio measured once against steady_clock.
double TscMsPerTick() {
  static const double ratio = [] {
    const auto c0 = std::chrono::steady_clock::now();
    const uint64_t t0 = __rdtsc();
    while (std::chrono::steady_clock::now() - c0 < std::chrono::milliseconds(20)) {
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
    const uint64_t ticks = __rdtsc() - t0;
    return ticks ? ms / double(ticks) : 0.0;
  }();
  return ratio;
}
// Drop-in for the steady_clock pattern below: now() -> ticks, Ms(a, b) -> milliseconds.
struct TscClock {
  static uint64_t now() { return __rdtsc(); }
};
inline double TscMs(uint64_t t0) { return double(__rdtsc() - t0) * TscMsPerTick(); }

std::string Hr(const char* what, HRESULT hr) {
  char b[160];
  std::snprintf(b, sizeof(b), "%s failed (0x%08lX)%s", what, static_cast<unsigned long>(hr),
                hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG ? " DEVICE_REMOVED" : "");
  return b;
}

// The unsigned (or only) view format of each Format.
DXGI_FORMAT ResourceFormat(Format f) {
  switch (f) {
    case Format::kR8G8B8A8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::kR16G16B16A16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case Format::kR16G16Snorm: return DXGI_FORMAT_R16G16_SNORM;
    case Format::kR16G16B16A16Snorm: return DXGI_FORMAT_R16G16B16A16_SNORM;
    case Format::kR32Uint: return DXGI_FORMAT_R32_UINT;
    case Format::kR32Float: return DXGI_FORMAT_R32_FLOAT;
    case Format::kR32G32B32A32Float: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case Format::kR8Unorm: return DXGI_FORMAT_R8_UNORM;
    case Format::kR8G8Unorm: return DXGI_FORMAT_R8G8_UNORM;
    case Format::kR16Unorm: return DXGI_FORMAT_R16_UNORM;
    case Format::kR16G16Unorm: return DXGI_FORMAT_R16G16_UNORM;
    case Format::kR16G16B16A16Unorm: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case Format::kR16Float: return DXGI_FORMAT_R16_FLOAT;
    case Format::kR16G16Float: return DXGI_FORMAT_R16G16_FLOAT;
    case Format::kR32G32Float: return DXGI_FORMAT_R32G32_FLOAT;
    case Format::kR10G10B10A2Unorm: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case Format::kB5G6R5Unorm: return DXGI_FORMAT_B5G6R5_UNORM;
    case Format::kB5G5R5A1Unorm: return DXGI_FORMAT_B5G5R5A1_UNORM;
    case Format::kB4G4R4A4Unorm: return DXGI_FORMAT_B4G4R4A4_UNORM;
    case Format::kBC1: return DXGI_FORMAT_BC1_UNORM;
    case Format::kBC2: return DXGI_FORMAT_BC2_UNORM;
    case Format::kBC3: return DXGI_FORMAT_BC3_UNORM;
    case Format::kBC4: return DXGI_FORMAT_BC4_UNORM;
    case Format::kBC5: return DXGI_FORMAT_BC5_UNORM;
    case Format::kD32FloatS8X24: return DXGI_FORMAT_R32G8X24_TYPELESS;
    case Format::kD24UnormS8: return DXGI_FORMAT_R24G8_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}
// Formats with an SNORM twin: the resource is typeless (when it is only sampled) and SignedFormat gives the view.
DXGI_FORMAT TypelessFormat(Format f) {
  switch (f) {
    case Format::kR8Unorm: return DXGI_FORMAT_R8_TYPELESS;
    case Format::kR8G8Unorm: return DXGI_FORMAT_R8G8_TYPELESS;
    case Format::kR8G8B8A8Unorm: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case Format::kR16Unorm: return DXGI_FORMAT_R16_TYPELESS;
    case Format::kR16G16Unorm: return DXGI_FORMAT_R16G16_TYPELESS;
    case Format::kR16G16B16A16Unorm: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case Format::kBC4: return DXGI_FORMAT_BC4_TYPELESS;
    case Format::kBC5: return DXGI_FORMAT_BC5_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}
DXGI_FORMAT SignedFormat(Format f) {
  switch (f) {
    case Format::kR8Unorm: return DXGI_FORMAT_R8_SNORM;
    case Format::kR8G8Unorm: return DXGI_FORMAT_R8G8_SNORM;
    case Format::kR8G8B8A8Unorm: return DXGI_FORMAT_R8G8B8A8_SNORM;
    case Format::kR16Unorm: return DXGI_FORMAT_R16_SNORM;
    case Format::kR16G16Unorm: return DXGI_FORMAT_R16G16_SNORM;
    case Format::kR16G16B16A16Unorm: return DXGI_FORMAT_R16G16B16A16_SNORM;
    case Format::kBC4: return DXGI_FORMAT_BC4_SNORM;
    case Format::kBC5: return DXGI_FORMAT_BC5_SNORM;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}
// Render-target formats whose EDRAM bits are their host bits but which a float/SNORM write would change (NaN
// payloads, SNORM -32768): created typeless with an extra UINT view for transfers (Canary's
// GetColorOwnershipTransferDXGIFormat), upl-n108.
DXGI_FORMAT BitsFormat(Format f) {
  switch (f) {
    case Format::kR16G16Float: case Format::kR16G16Snorm: return DXGI_FORMAT_R16G16_UINT;
    case Format::kR16G16B16A16Float: case Format::kR16G16B16A16Snorm: return DXGI_FORMAT_R16G16B16A16_UINT;
    case Format::kR32Float: return DXGI_FORMAT_R32_UINT;
    case Format::kR32G32Float: return DXGI_FORMAT_R32G32_UINT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}
DXGI_FORMAT BitsTypeless(Format f) {
  switch (f) {
    case Format::kR16G16Float: case Format::kR16G16Snorm: return DXGI_FORMAT_R16G16_TYPELESS;
    case Format::kR16G16B16A16Float: case Format::kR16G16B16A16Snorm: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case Format::kR32Float: return DXGI_FORMAT_R32_TYPELESS;
    case Format::kR32G32Float: return DXGI_FORMAT_R32G32_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}
DXGI_FORMAT ViewFormat(Format f) {
  return f == Format::kD32FloatS8X24 ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT
         : f == Format::kD24UnormS8  ? DXGI_FORMAT_D24_UNORM_S8_UINT
                                     : ResourceFormat(f);
}
// SRV formats of the two planes of a depth/stencil texture.
DXGI_FORMAT DepthPlaneFormat(Format f, uint32_t plane) {
  if (f == Format::kD24UnormS8) return plane ? DXGI_FORMAT_X24_TYPELESS_G8_UINT : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
  return plane ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
}
uint32_t BytesPerTexel(Format f) {
  switch (f) {  // patch 0029 (Gears 3 bug hunt C3): every 8/2-byte format, so readback footprints are big enough
    case Format::kR8G8B8A8Unorm: case Format::kR32Uint: case Format::kR32Float: return 4;
    case Format::kR16G16B16A16Float: case Format::kD32FloatS8X24: case Format::kR16G16B16A16Snorm:
    case Format::kR16G16B16A16Unorm: case Format::kR32G32Float: return 8;
    case Format::kR32G32B32A32Float: return 16;
    case Format::kR8Unorm: return 1;
    case Format::kR8G8Unorm: case Format::kR16Unorm: case Format::kR16Float: case Format::kB5G6R5Unorm:
    case Format::kB5G5R5A1Unorm: case Format::kB4G4R4A4Unorm: return 2;
    case Format::kD24UnormS8: return 4;
    default: return 4;
  }
}

D3D12_COMPARISON_FUNC Compare(CompareFunc f) {
  // Xenos order: never, less, equal, less-equal, greater, not-equal, greater-equal, always (D3D12 = that + 1).
  return D3D12_COMPARISON_FUNC(uint32_t(f) + 1);
}
D3D12_STENCIL_OP StencilOpD3D(StencilOp o) {
  switch (o) {
    case StencilOp::kKeep: return D3D12_STENCIL_OP_KEEP;
    case StencilOp::kZero: return D3D12_STENCIL_OP_ZERO;
    case StencilOp::kReplace: return D3D12_STENCIL_OP_REPLACE;
    case StencilOp::kIncrSat: return D3D12_STENCIL_OP_INCR_SAT;
    case StencilOp::kDecrSat: return D3D12_STENCIL_OP_DECR_SAT;
    case StencilOp::kInvert: return D3D12_STENCIL_OP_INVERT;
    case StencilOp::kIncrWrap: return D3D12_STENCIL_OP_INCR;
    case StencilOp::kDecrWrap: return D3D12_STENCIL_OP_DECR;
  }
  return D3D12_STENCIL_OP_KEEP;
}

constexpr uint32_t kGraphicsSrvCount = 97;   // t0-t96
constexpr uint32_t kSamplerCount = 32;       // s0-s31
constexpr uint32_t kComputeSrvCount = 4;     // t0-t3
// Static graphics table at 0, the rest a ring. 500k (the tier-2+ maximum is 1M): with about 1000 textured draws a
// frame and two frames in flight the old 65536 wrapped every frame and waited on our GPU (job 107/109).
constexpr uint32_t kShaderHeapSize = 500000;
constexpr uint32_t kShaderHeapRingStart = 256;
// Patch 0048 (map: "a full heap silently skips a view"): CPU-only heaps have no small limit, so they are sized well
// past what a session uses (a texture holds up to two SRVs, a UAV, mip UAVs and sub-views; resolve destinations and
// scaled copies come and go). An allocation that still fails is logged instead of leaving a black texture unexplained.
constexpr uint32_t kCpuHeapSize = 65536;       // RTVs / DSVs (was 8192)
constexpr uint32_t kViewHeapSize = 1048576;    // CPU-only SRV/UAV descriptors (texture views; was 65536)
// Sampler heap (2048 = the D3D12 limit), upl-n108: the static table at 0, 16 tables of 32 samplers, then 369 tables of 4
// for draws that only use s0-s3 (most). Every table is bound as a 32-sampler range, so each starts at most 2016; the
// whole heap is filled with the default sampler at start-up so the slots past a small table are always valid. With 64
// tables of 32 the pool ran out within a command list and waited on our GPU (job 110: 418-1325 waits).
// upl-n109: size classes of 4, 8, 16 and 32 samplers (88, 48, 40 and 20 tables, the 32s last so the last one starts at
// 2016), and three such heaps rotated per frame (NextFrame): a heap is reused only once the frame three back is done,
// so a whole frame's worth of tables fits without waiting (job 114: 1012-1685 sampler-table waits per 300 frames with
// 16 tables of 32).
constexpr uint32_t kSamplerHeapSize = 2048;
// Patch 0034: 16 heaps, not 3. A frame of Gears 3 at 2x needs ~400 distinct sampler sets (196 tables per heap), so with 3
// heaps the table search submitted and waited on our GPU ~3 times a frame (5.9 ms/frame, Gears 3 play log 11:34). Now a
// full heap switches the list to the next heap the GPU is done with (SetDescriptorHeaps) instead of waiting. 64 KB each.
constexpr uint32_t kSamplerPools = 16;
constexpr uint32_t kSamplerClasses = 4;
constexpr uint32_t kSamplerClassSize[kSamplerClasses] = {4, 8, 16, 32};
constexpr uint32_t kSamplerClassTables[kSamplerClasses] = {88, 48, 40, 20};
constexpr uint32_t kSamplerTables = 1 + 88 + 48 + 40 + 20;
static_assert(32 + 88 * 4 + 48 * 8 + 40 * 16 + 20 * 32 == kSamplerHeapSize, "sampler heap layout");
// Graphics root parameters: 0-3 root CBVs b0-b3 for the vertex shader, 4-7 the same for the pixel shader,
// 8 the SRV table t0-t96, 9 the sampler table s0-s31.
// kRootGuestSrv: t96, the guest-memory raw buffer, as a root SRV (upl-n107) so the texture table covers only the slots
// a draw uses (t0-t95) instead of 97 descriptors per draw.
// kRootUploadSrv: t97, the upload ring as a raw buffer: vertex data the shaders read straight from it (UploadTransient).
constexpr UINT kRootVsCbv = 0, kRootPsCbv = 4, kRootSrvTable = 8, kRootSamplerTable = 9, kRootGuestSrv = 10,
               kRootUploadSrv = 11, kRootGuestUav = 12, kRootGraphicsCount = 13;
// kRootGuestUav: u0, the guest-memory raw buffer as a root UAV - written by shaders that memexport (Gears 3, notes/11),
// which then also read their vertex data through it (the buffer is in UNORDERED_ACCESS state for those draws).
constexpr uint32_t kTextureSrvCount = 96;  // t0-t95 in the table
// Compute: 0 root constants b0 (16 dwords), 1 SRV table t0-t3, 2 UAV table u0.
constexpr UINT kRootCsConstants = 0, kRootCsSrv = 1, kRootCsUav = 2;

struct Texture {
  ID3D12Resource* resource = nullptr;
  TextureDesc desc;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
  int32_t rtv = -1, dsv = -1;
  int32_t srv[2] = {-1, -1};  // CPU heap slots (plane 0 / plane 1 for depth-stencil)
  int32_t uav = -1;
  std::vector<int32_t> uav_mips;  // typed UAVs of mips 1.. (created on first use)
  bool typeless = false;
  bool bits = false;     // typeless render target with a UINT bits view (BitsFormat)
  uint64_t serial = 0;   // creation number: a reused handle is a different texture (texture-table cache, upl-n109)
  int32_t rtv_bits = -1;
  std::vector<std::pair<uint64_t, int32_t>> views;  // texture views by key -> CPU heap slot
  int32_t heap = -1;  // patch 0017: placed in tex_heaps_[heap] at heap_offset (heap_size bytes); -1 = committed
  uint64_t heap_offset = 0, heap_size = 0;
  bool scratch_pending = false;  // batched texture loads: a scratch copy into it is queued (FlushScratchCopies)
  uint64_t uav_unbarriered = 0;  // batched texture loads: mips written as a UAV with the barrier left out (bit = mip)
};

struct Pipeline {
  ID3D12PipelineState* pso = nullptr;  // set before `ready` (release) by a worker for asynchronous creations
  bool compute = false;
  D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  std::atomic<bool> ready{true};   // false while a worker is creating it
  std::atomic<bool> failed{false};
  // Patch 0062: the start-up backlog class this pipeline's job was queued in (0xFF = not a waiting backlog job). A draw
  // that needs it moves the job to the front (PromoteBacklogJob) instead of waiting behind the whole recorded set.
  std::atomic<uint8_t> backlog_class{0xFF};
};
// One asynchronous pipeline creation (upl-n108): the description with its own copies of the bytecode.
struct PipelineJob {
  uint32_t handle = 0;
  struct Pipeline* target = nullptr;  // deque element: stable while the recording thread appends
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  // Patch 0058: shared with the shader (immutable bytecode), not copied - the start-up backlog queues thousands of jobs.
  std::shared_ptr<const std::vector<uint8_t>> code[3];
  std::wstring library_name;  // saved pipelines: stored under this name once created (empty = library off)
  uint8_t priority = 0;       // patch 0057: which queue (PipelinePriority)
  bool stage_prefetch = false;
};
// Patch 0057 (Canary pipeline_util.h CalculatePipelinePriority, refined): the order the workers take new pipelines.
// 4 = writes colour target 0 and depth (opaque scene geometry: characters, walls, ground), 3 = writes colour target 0
// (particles, decals, full-screen passes), 2 = writes another colour target, 1 = depth / stencil only, 0 = the rest.
// Canary: RT0 > any visible RT > depth only > the rest; same order, with opaque RT0 draws ahead of blended ones.
inline uint8_t PipelinePriority(const GraphicsPipelineDesc& d) {
  uint32_t colour = 0;
  for (uint32_t i = 0; i < d.color_count && i < 4; ++i)
    if (d.color_formats[i] != Format::kUnknown && (d.color_write_masks[i] & 0xF)) colour |= 1u << i;
  const bool depth = d.depth_format != Format::kUnknown && (d.depth_write || (d.stencil_enable && d.stencil_write_mask));
  // Patch 0132 (pop-in, Gears 3 / Judgment 21:55 / 21:58): a missing depth-only / shadow draw darkens or blocks the
  // lighting of everything after it, a late particle only pops in - so depth / stencil only now comes right after opaque.
  if (colour & 1u) return depth ? 4 : 2;
  if (colour) return 1;
  return depth ? 3 : 0;
}
constexpr uint32_t kPipelineClasses = 5;
// Patch 0058: live requests (a draw is waiting) use classes + 5, start-up backlog requests the class itself.
constexpr uint32_t kPipelinePriorities = 2 * kPipelineClasses;

struct Readback {
  ID3D12Resource* buffer = nullptr;
  uint64_t size = 0;
  uint8_t* mapped = nullptr;
  uint32_t row_pitch = 0;
  uint64_t fence = 0;  // 0 = not yet submitted
  bool in_use = false;
};

// A CPU-only descriptor heap with a free list.
class CpuHeap {
 public:
  bool Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count) {
    type_ = type;
    D3D12_DESCRIPTOR_HEAP_DESC d{type, count, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&heap_)))) return false;
    increment_ = device->GetDescriptorHandleIncrementSize(type);
    start_ = heap_->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = count; i-- > 0;) free_.push_back(int32_t(i));
    return true;
  }
  void Shutdown() { SafeRelease(heap_); }
  int32_t Alloc() {
    if (free_.empty()) {
      if (++failures_ <= 10 || (failures_ & (failures_ - 1)) == 0)  // patch 0048: the first 10, then powers of two
        REXLOG_ERROR("gow2_renderer: CPU descriptor heap (type {}) is full - a view was not created ({} times so far)",
                     int(type_), failures_);
      return -1;
    }
    const int32_t i = free_.back();
    free_.pop_back();
    return i;
  }
  void Free(int32_t i) {
    if (i >= 0) free_.push_back(i);
  }
  D3D12_CPU_DESCRIPTOR_HANDLE Handle(int32_t i) const { return {start_.ptr + SIZE_T(i) * increment_}; }

 private:
  ID3D12DescriptorHeap* heap_ = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE start_{};
  UINT increment_ = 0;
  std::vector<int32_t> free_;
  D3D12_DESCRIPTOR_HEAP_TYPE type_ = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  uint64_t failures_ = 0;  // patch 0048
};

// ---- Copy threads (job 171, notes/52 section 13) ----
// Guest-memory copies into the upload ring run on worker threads. Reading guest memory is the slow part (the bytes
// were usually just written by the title's CPU cores, so they come from other cores' caches: ~1 GB/s measured in
// job 170), and several cores waiting on those reads at once overlap the waits. The caller only reserves ring space
// and queues (destination, source, size); the copies finish at Flush, which every Submit and every release point
// (anything that tells the title the GPU got further) calls - the caller's thread helps with what is left there.
// Copies below kInline bytes stay inline (queueing would cost more); big ones are split so the workers share them.
class CopyPool {
 public:
  ~CopyPool() { Stop(); }
  void Start(uint32_t threads, BackendStats* stats) {
    stats_ = stats;
    threads = std::min<uint32_t>(threads, 16);
    for (uint32_t i = 0; i < threads; ++i) workers_.emplace_back([this] { Run(); });
    if (threads) pending_.reserve(kBatchCopies);
  }
  void Stop() {
    if (workers_.empty()) return;
    Flush();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
    workers_.clear();
  }
  bool Active() const { return !workers_.empty(); }
  void Copy(void* dst, const void* src, size_t n) {
    if (workers_.empty() || n < kInline) {
      std::memcpy(dst, src, n);
      return;
    }
    ++stats_->copies_deferred;
    stats_->copy_deferred_bytes += n;
    auto* d = static_cast<uint8_t*>(dst);
    auto* s = static_cast<const uint8_t*>(src);
    while (n) {
      const size_t part = std::min(n, kPiece);
      pending_.push_back({d, s, part});
      pending_bytes_ += part;
      d += part;
      s += part;
      n -= part;
      if (pending_.size() >= kBatchCopies || pending_bytes_ >= kBatchBytes) Publish();
    }
  }
  // Every deferred copy is done when this returns (the caller's thread runs queued batches itself meanwhile).
  void Flush() {
    if (workers_.empty()) return;
    Publish();
    if (!outstanding_.load(std::memory_order_acquire)) return;
    ++stats_->copy_flushes;
    std::unique_lock<std::mutex> lock(mutex_);
    while (!batches_.empty()) {
      std::vector<Job> b = std::move(batches_.front());
      batches_.pop_front();
      available_.store(uint32_t(batches_.size()), std::memory_order_relaxed);
      lock.unlock();
      RunBatch(b);
      lock.lock();
      outstanding_.fetch_sub(1, std::memory_order_acq_rel);
    }
    // The last batches are on workers: spin briefly (a wake-up costs more than a short copy), then sleep.
    lock.unlock();
    for (int i = 0; i < kSpin && outstanding_.load(std::memory_order_acquire); ++i) _mm_pause();
    if (!outstanding_.load(std::memory_order_acquire)) return;
    lock.lock();
    flush_waiting_ = true;
    done_cv_.wait(lock, [this] { return outstanding_.load(std::memory_order_acquire) == 0; });
    flush_waiting_ = false;
  }

 private:
  struct Job {
    uint8_t* dst;
    const uint8_t* src;
    size_t n;
  };
  // Job 172: workers spin ~100 us for new work before sleeping, and are only woken when asleep - job 171's wake-up
  // per batch (condition variable signal) cost the GPU thread 4.8% and ate the gain.
  static constexpr size_t kInline = 1024, kPiece = 64u << 10, kBatchBytes = 96u << 10, kBatchCopies = 48;
  static constexpr int kSpin = 2000;
  static void RunBatch(const std::vector<Job>& b) {
    for (const Job& j : b) std::memcpy(j.dst, j.src, j.n);
  }
  void Publish() {
    if (pending_.empty()) return;
    bool wake;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      batches_.push_back(std::move(pending_));
      outstanding_.fetch_add(1, std::memory_order_acq_rel);
      available_.store(uint32_t(batches_.size()), std::memory_order_release);
      wake = sleepers_ > 0;
    }
    if (wake) cv_.notify_one();
    pending_ = {};
    pending_.reserve(kBatchCopies);
    pending_bytes_ = 0;
  }
  void Run() {
    using SetDesc = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    if (auto fn = reinterpret_cast<SetDesc>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")))
      fn(GetCurrentThread(), L"GPU Copy (ours)");
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      if (batches_.empty() && !stop_) {
        lock.unlock();
        for (int i = 0; i < kSpin && !available_.load(std::memory_order_acquire); ++i) _mm_pause();
        lock.lock();
        if (batches_.empty() && !stop_) {
          ++sleepers_;
          cv_.wait(lock, [this] { return stop_ || !batches_.empty(); });
          --sleepers_;
        }
      }
      if (batches_.empty()) {
        if (stop_) break;
        continue;
      }
      std::vector<Job> b = std::move(batches_.front());
      batches_.pop_front();
      available_.store(uint32_t(batches_.size()), std::memory_order_relaxed);
      lock.unlock();
      RunBatch(b);
      lock.lock();
      if (outstanding_.fetch_sub(1, std::memory_order_acq_rel) == 1 && flush_waiting_) done_cv_.notify_all();
    }
  }
  BackendStats* stats_ = nullptr;
  std::vector<std::thread> workers_;
  std::vector<Job> pending_;  // caller's thread only
  size_t pending_bytes_ = 0;
  std::mutex mutex_;
  std::condition_variable cv_, done_cv_;
  std::deque<std::vector<Job>> batches_;
  std::atomic<uint32_t> outstanding_{0};  // published batches not yet finished
  std::atomic<uint32_t> available_{0};    // batches queued (read while spinning)
  uint32_t sleepers_ = 0;
  bool stop_ = false, flush_waiting_ = false;
};

// ---- Threaded command recording (notes/52 section 10) ----
// Every command-list call of the backend goes through this proxy (same method names as ID3D12GraphicsCommandList, so
// the call sites read as before). Immediate mode forwards each call at once. Threaded mode encodes the call into a
// block of bytes and a recording thread replays the blocks, in order, onto the real list - so the driver's recording
// work runs on another core while the caller translates the next draws. Close / Reset / Execute+Signal are ops too, so
// the real list, its allocators and the queue submissions are only touched by the recording thread. Drain() waits
// until everything encoded so far has been replayed (Submit drains, so presentation order and fence waits are
// unchanged). All arguments are plain data or COM pointers whose lifetime the backend already manages by fence.
class CommandListProxy {
 public:
  ~CommandListProxy() { Stop(); }

  void Start(ID3D12GraphicsCommandList* real, ID3D12CommandQueue* queue, bool threaded) {
    real_ = real;
    queue_ = queue;
    threaded_ = threaded;
    if (threaded_) worker_ = std::thread([this] { Run(); });
  }
  void Stop() {
    if (!worker_.joinable()) return;
    Drain();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    worker_.join();
  }
  bool Threaded() const { return threaded_; }
  ID3D12GraphicsCommandList* Real() const { return real_; }
  // First failure the recording thread saw (Close / Reset), S_OK if none.
  HRESULT Error() const { return error_.load(); }
  const char* ErrorWhat() const { return error_what_.load(); }

  // Patch 0138: hands what is encoded so far to the recording thread now, without waiting for it.
  void Kick() {
    FlushBarriers();
    if (threaded_) Flush();
  }
  // Waits until every encoded call has been replayed.
  void Drain() {
    FlushBarriers();
    if (!threaded_) return;
    Flush();
    std::unique_lock<std::mutex> lock(mutex_);
    if (ready_.empty() && !busy_) return;
    const auto start = std::chrono::steady_clock::now();
    idle_cv_.wait(lock, [this] { return ready_.empty() && !busy_; });
    recording_wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  }

  // ---- lifecycle ----
  HRESULT Close() {
    FlushBarriers();
    if (!threaded_) return real_->Close();
    Put(kClose);
    return S_OK;  // a failure shows in Error() after the next Drain()
  }
  // Resets the allocator, then the list on it.
  HRESULT Reset(ID3D12CommandAllocator* allocator, ID3D12PipelineState* state) {
    pending_.clear();  // a new list starts with nothing queued (Close flushed the old one)
    if (!threaded_) {
      allocator->Reset();
      return real_->Reset(allocator, state);
    }
    Put(kReset);
    Pod(allocator);
    Pod(state);
    return S_OK;
  }
  void ExecuteAndSignal(ID3D12Fence* fence, UINT64 value) {
    FlushBarriers();
    if (!threaded_) {
      ID3D12CommandList* lists[] = {real_};
      queue_->ExecuteCommandLists(1, lists);
      queue_->Signal(fence, value);
      return;
    }
    Put(kExecuteSignal);
    Pod(fence);
    Pod(value);
  }

  // ---- mirrored ID3D12GraphicsCommandList calls ----
  void CopyBufferRegion(ID3D12Resource* dst, UINT64 dst_offset, ID3D12Resource* src, UINT64 src_offset, UINT64 bytes) {
    FlushBarriers();
    if (!threaded_) return real_->CopyBufferRegion(dst, dst_offset, src, src_offset, bytes);
    Put(kCopyBufferRegion);
    Pod(dst); Pod(dst_offset); Pod(src); Pod(src_offset); Pod(bytes);
  }
  void ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil,
                             UINT rect_count, const D3D12_RECT* rects) {
    FlushBarriers();
    if (!threaded_) return real_->ClearDepthStencilView(dsv, flags, depth, stencil, rect_count, rects);
    Put(kClearDepthStencil);
    Pod(dsv); Pod(flags); Pod(depth); Pod(stencil);
    Array(rects, rects ? rect_count : 0);
  }
  void ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT rgba[4], UINT rect_count,
                             const D3D12_RECT* rects) {
    FlushBarriers();
    if (!threaded_) return real_->ClearRenderTargetView(rtv, rgba, rect_count, rects);
    Put(kClearRenderTarget);
    Pod(rtv);
    Bytes(rgba, 16);
    Array(rects, rects ? rect_count : 0);
  }
  void DrawInstanced(UINT vertices, UINT instances, UINT first_vertex, UINT first_instance) {
    FlushBarriers();
    if (!threaded_) return real_->DrawInstanced(vertices, instances, first_vertex, first_instance);
    Put(kDrawInstanced);
    Pod(vertices); Pod(instances); Pod(first_vertex); Pod(first_instance);
  }
  void IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view) {
    FlushBarriers();
    if (!threaded_) return real_->IASetIndexBuffer(view);
    Put(kIASetIndexBuffer);
    Pod(*view);
  }
  void DrawIndexedInstanced(UINT indices, UINT instances, UINT first_index, INT base_vertex, UINT first_instance) {
    FlushBarriers();
    if (!threaded_) return real_->DrawIndexedInstanced(indices, instances, first_index, base_vertex, first_instance);
    Put(kDrawIndexedInstanced);
    Pod(indices); Pod(instances); Pod(first_index); Pod(base_vertex); Pod(first_instance);
  }
  void CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
                         const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    FlushBarriers();
    if (!threaded_) return real_->CopyTextureRegion(dst, x, y, z, src, box);
    Put(kCopyTextureRegion);
    Pod(*dst); Pod(x); Pod(y); Pod(z); Pod(*src);
    Array(box, box ? 1u : 0u);
  }
  void SetComputeRootSignature(ID3D12RootSignature* root) {
    FlushBarriers();
    if (!threaded_) return real_->SetComputeRootSignature(root);
    Put(kSetComputeRootSignature);
    Pod(root);
  }
  void SetPipelineState(ID3D12PipelineState* pso) {
    FlushBarriers();
    if (!threaded_) return real_->SetPipelineState(pso);
    Put(kSetPipelineState);
    Pod(pso);
  }
  void SetComputeRoot32BitConstants(UINT param, UINT count, const void* data, UINT offset) {
    FlushBarriers();
    if (!threaded_) return real_->SetComputeRoot32BitConstants(param, count, data, offset);
    Put(kSetComputeRoot32BitConstants);
    Pod(param); Pod(offset);
    Array(static_cast<const uint32_t*>(data), count);
  }
  void SetComputeRootDescriptorTable(UINT param, D3D12_GPU_DESCRIPTOR_HANDLE table) {
    FlushBarriers();
    if (!threaded_) return real_->SetComputeRootDescriptorTable(param, table);
    Put(kSetComputeRootDescriptorTable);
    Pod(param); Pod(table);
  }
  void Dispatch(UINT x, UINT y, UINT z) {
    FlushBarriers();
    if (!threaded_) return real_->Dispatch(x, y, z);
    Put(kDispatch);
    Pod(x); Pod(y); Pod(z);
  }
  uint64_t barrier_calls = 0, barriers = 0;  // job 045: barrier counts (each call can stall the GPU pipeline)
  double recording_wait_ms = 0.0;  // caller thread only
  uint64_t barrier_batches = 0, barriers_merged = 0;  // job 046: emitted batches, transitions merged away
  // Job 046: barriers are queued and emitted as ONE ResourceBarrier call right before the next command of any kind,
  // so consecutive barriers (render targets, textures, the guest buffer) cost one GPU sync instead of one each. A
  // transition of a resource whose last queued entry is a transition ending in this one's start state is merged into
  // it (A->B, B->C = A->C; A->B, B->A = nothing). Command order is unchanged: every other call flushes first.
  void ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* in) {
    ++barrier_calls;
    barriers += count;
    for (UINT i = 0; i < count; ++i) {
      const D3D12_RESOURCE_BARRIER& b = in[i];
      ID3D12Resource* res = b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ? b.Transition.pResource
                            : b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV       ? b.UAV.pResource
                                                                             : nullptr;
      if (res && b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
          b.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        // The last queued entry touching this resource.
        size_t k = pending_.size();
        while (k > 0) {
          const D3D12_RESOURCE_BARRIER& q = pending_[k - 1];
          const ID3D12Resource* qr = q.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ? q.Transition.pResource
                                     : q.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV       ? q.UAV.pResource
                                                                                      : nullptr;
          if (qr == res || q.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) break;
          --k;
        }
        if (k > 0) {
          D3D12_RESOURCE_BARRIER& q = pending_[k - 1];
          if (q.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
              q.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES &&
              q.Transition.StateAfter == b.Transition.StateBefore && q.Flags == b.Flags) {
            ++barriers_merged;
            q.Transition.StateAfter = b.Transition.StateAfter;
            if (q.Transition.StateBefore == q.Transition.StateAfter) pending_.erase(pending_.begin() + (k - 1));
            continue;
          }
        }
      }
      pending_.push_back(b);
    }
  }
  void FlushBarriers() {
    if (pending_.empty()) return;
    ++barrier_batches;
    const UINT n = UINT(pending_.size());
    if (!threaded_) {
      real_->ResourceBarrier(n, pending_.data());
    } else {
      Put(kResourceBarrier);
      Array(pending_.data(), n);
    }
    pending_.clear();
  }
  void BeginQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
    FlushBarriers();
    if (!threaded_) return real_->BeginQuery(heap, type, index);
    Put(kBeginQuery);
    Pod(heap); Pod(type); Pod(index);
  }
  void EndQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
    FlushBarriers();
    if (!threaded_) return real_->EndQuery(heap, type, index);
    Put(kEndQuery);
    Pod(heap); Pod(type); Pod(index);
  }
  void ResolveQueryData(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT start, UINT count, ID3D12Resource* dst,
                        UINT64 dst_offset) {
    FlushBarriers();
    if (!threaded_) return real_->ResolveQueryData(heap, type, start, count, dst, dst_offset);
    Put(kResolveQueryData);
    Pod(heap); Pod(type); Pod(start); Pod(count); Pod(dst); Pod(dst_offset);
  }
  void BeginEvent(UINT metadata, const void* data, UINT size) {
    FlushBarriers();
    if (!threaded_) return real_->BeginEvent(metadata, data, size);
    Put(kBeginEvent);
    Pod(metadata);
    Array(static_cast<const uint8_t*>(data), size);
  }
  void EndEvent() {
    FlushBarriers();
    if (!threaded_) return real_->EndEvent();
    Put(kEndEvent);
  }
  void SetDescriptorHeaps(UINT count, ID3D12DescriptorHeap* const* heaps) {
    FlushBarriers();
    if (!threaded_) return real_->SetDescriptorHeaps(count, heaps);
    Put(kSetDescriptorHeaps);
    Array(heaps, count);
  }
  void SetGraphicsRootSignature(ID3D12RootSignature* root) {
    FlushBarriers();
    if (!threaded_) return real_->SetGraphicsRootSignature(root);
    Put(kSetGraphicsRootSignature);
    Pod(root);
  }
  void SetGraphicsRootShaderResourceView(UINT param, D3D12_GPU_VIRTUAL_ADDRESS address) {
    FlushBarriers();
    if (!threaded_) return real_->SetGraphicsRootShaderResourceView(param, address);
    Put(kSetGraphicsRootSrv);
    Pod(param); Pod(address);
  }
  void SetGraphicsRootUnorderedAccessView(UINT param, D3D12_GPU_VIRTUAL_ADDRESS address) {
    FlushBarriers();
    if (!threaded_) return real_->SetGraphicsRootUnorderedAccessView(param, address);
    Put(kSetGraphicsRootUav);
    Pod(param); Pod(address);
  }
  void SetGraphicsRootDescriptorTable(UINT param, D3D12_GPU_DESCRIPTOR_HANDLE table) {
    FlushBarriers();
    if (!threaded_) return real_->SetGraphicsRootDescriptorTable(param, table);
    Put(kSetGraphicsRootDescriptorTable);
    Pod(param); Pod(table);
  }
  void SetGraphicsRootConstantBufferView(UINT param, D3D12_GPU_VIRTUAL_ADDRESS address) {
    FlushBarriers();
    if (!threaded_) return real_->SetGraphicsRootConstantBufferView(param, address);
    Put(kSetGraphicsRootCbv);
    Pod(param); Pod(address);
  }
  void OMSetRenderTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single,
                          const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    FlushBarriers();
    if (!threaded_) return real_->OMSetRenderTargets(count, rtvs, single, dsv);
    Put(kOMSetRenderTargets);
    Pod(single);
    Array(rtvs, rtvs ? count : 0);
    Array(dsv, dsv ? 1u : 0u);
  }
  void IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology) {
    FlushBarriers();
    if (!threaded_) return real_->IASetPrimitiveTopology(topology);
    Put(kIASetPrimitiveTopology);
    Pod(topology);
  }
  void RSSetViewports(UINT count, const D3D12_VIEWPORT* viewports) {
    FlushBarriers();
    if (!threaded_) return real_->RSSetViewports(count, viewports);
    Put(kRSSetViewports);
    Array(viewports, count);
  }
  void RSSetScissorRects(UINT count, const D3D12_RECT* rects) {
    FlushBarriers();
    if (!threaded_) return real_->RSSetScissorRects(count, rects);
    Put(kRSSetScissorRects);
    Array(rects, count);
  }
  void OMSetStencilRef(UINT ref) {
    FlushBarriers();
    if (!threaded_) return real_->OMSetStencilRef(ref);
    Put(kOMSetStencilRef);
    Pod(ref);
  }
  void OMSetBlendFactor(const FLOAT rgba[4]) {
    FlushBarriers();
    if (!threaded_) return real_->OMSetBlendFactor(rgba);
    Put(kOMSetBlendFactor);
    Bytes(rgba, 16);
  }
  // Patch 0089: ID3D12GraphicsCommandList1 calls (programmable sample positions; depth decompression).
  void SetSamplePositions(UINT samples, const D3D12_SAMPLE_POSITION* positions) {
    FlushBarriers();
    if (!threaded_) return DoSetSamplePositions(real_, samples, positions);
    Put(kSetSamplePositions);
    Array(positions, samples);
  }
  void DecompressDepth(ID3D12Resource* resource, DXGI_FORMAT format) {
    FlushBarriers();
    if (!threaded_) return DoDecompressDepth(real_, resource, format);
    Put(kDecompressDepth);
    Pod(resource);
    Pod(format);
  }
  static void DoSetSamplePositions(ID3D12GraphicsCommandList* l, UINT samples, const D3D12_SAMPLE_POSITION* positions) {
    ID3D12GraphicsCommandList1* l1 = nullptr;
    if (FAILED(l->QueryInterface(IID_PPV_ARGS(&l1))) || !l1) return;
    l1->SetSamplePositions(samples, samples ? 1 : 0, samples ? const_cast<D3D12_SAMPLE_POSITION*>(positions) : nullptr);
    l1->Release();
  }
  static void DoDecompressDepth(ID3D12GraphicsCommandList* l, ID3D12Resource* resource, DXGI_FORMAT format) {
    ID3D12GraphicsCommandList1* l1 = nullptr;
    if (FAILED(l->QueryInterface(IID_PPV_ARGS(&l1))) || !l1) return;
    l1->ResolveSubresourceRegion(resource, 0, 0, 0, resource, 0, nullptr, format, D3D12_RESOLVE_MODE_DECOMPRESS);
    l1->Release();
  }

 private:
  enum Op : uint32_t {
    kClose = 1, kReset, kExecuteSignal, kCopyBufferRegion, kClearDepthStencil, kClearRenderTarget, kDrawInstanced,
    kIASetIndexBuffer, kDrawIndexedInstanced, kCopyTextureRegion, kSetComputeRootSignature, kSetPipelineState,
    kSetComputeRoot32BitConstants, kSetComputeRootDescriptorTable, kDispatch, kResourceBarrier, kBeginQuery, kEndQuery,
    kResolveQueryData, kBeginEvent, kEndEvent, kSetDescriptorHeaps, kSetGraphicsRootSignature, kSetGraphicsRootSrv,
    kSetGraphicsRootDescriptorTable, kSetGraphicsRootCbv, kOMSetRenderTargets, kIASetPrimitiveTopology,
    kRSSetViewports, kRSSetScissorRects, kOMSetStencilRef, kOMSetBlendFactor, kSetGraphicsRootUav,
    kSetSamplePositions, kDecompressDepth  // patch 0089
  };
  static constexpr size_t kBlockBytes = 32 << 10;  // handed to the recording thread when full (and at Drain)

  // ---- encoding (caller's thread) ----
  void Bytes(const void* p, size_t n) {
    const size_t at = cur_.size();
    cur_.resize(at + n);
    if (n) std::memcpy(cur_.data() + at, p, n);
  }
  template <typename T>
  void Pod(const T& v) {
    static_assert(std::is_trivially_copyable_v<T>);
    Bytes(&v, sizeof(T));
  }
  template <typename T>
  void Array(const T* p, uint32_t count) {
    Pod(count);
    if (count) Bytes(p, sizeof(T) * count);
  }
  void Put(Op op) {
    if (cur_.size() >= kBlockBytes) Flush();
    if (cur_.capacity() < kBlockBytes + 1024) cur_.reserve(kBlockBytes + 4096);
    Pod(uint32_t(op));
  }
  void Flush() {
    if (cur_.empty()) return;
    std::vector<uint8_t> next;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ready_.push_back(std::move(cur_));
      if (!free_.empty()) {
        next = std::move(free_.back());
        free_.pop_back();
      }
    }
    cv_.notify_one();
    next.clear();
    cur_ = std::move(next);
  }

  // ---- replay (recording thread) ----
  struct Reader {
    const uint8_t* p;
    template <typename T>
    T Pod() {
      T v;
      std::memcpy(&v, p, sizeof(T));
      p += sizeof(T);
      return v;
    }
    template <typename T>
    const T* Array(uint32_t& count) {
      count = Pod<uint32_t>();
      const T* a = reinterpret_cast<const T*>(p);
      p += sizeof(T) * count;
      return count ? a : nullptr;
    }
  };
  void Fail(HRESULT hr, const char* what) {
    HRESULT ok = S_OK;
    if (error_.compare_exchange_strong(ok, hr)) error_what_.store(what);
  }
  void Replay(const std::vector<uint8_t>& block) {
    Reader r{block.data()};
    const uint8_t* end = block.data() + block.size();
    ID3D12GraphicsCommandList* l = real_;
    while (r.p < end) {
      uint32_t n = 0;
      switch (Op(r.Pod<uint32_t>())) {
        case kClose: {
          const HRESULT hr = l->Close();
          if (FAILED(hr)) Fail(hr, "ID3D12GraphicsCommandList::Close");
          break;
        }
        case kReset: {
          auto* allocator = r.Pod<ID3D12CommandAllocator*>();
          auto* state = r.Pod<ID3D12PipelineState*>();
          allocator->Reset();
          const HRESULT hr = l->Reset(allocator, state);
          if (FAILED(hr)) Fail(hr, "ID3D12GraphicsCommandList::Reset");
          break;
        }
        case kExecuteSignal: {
          auto* fence = r.Pod<ID3D12Fence*>();
          const UINT64 value = r.Pod<UINT64>();
          ID3D12CommandList* lists[] = {l};
          if (Error() == S_OK) queue_->ExecuteCommandLists(1, lists);
          queue_->Signal(fence, value);
          break;
        }
        case kCopyBufferRegion: {
          auto* d = r.Pod<ID3D12Resource*>(); const UINT64 doff = r.Pod<UINT64>();
          auto* s = r.Pod<ID3D12Resource*>(); const UINT64 soff = r.Pod<UINT64>(); const UINT64 bytes = r.Pod<UINT64>();
          l->CopyBufferRegion(d, doff, s, soff, bytes);
          break;
        }
        case kClearDepthStencil: {
          const auto h = r.Pod<D3D12_CPU_DESCRIPTOR_HANDLE>(); const auto f = r.Pod<D3D12_CLEAR_FLAGS>();
          const FLOAT depth = r.Pod<FLOAT>(); const UINT8 st = r.Pod<UINT8>();
          const D3D12_RECT* rects = r.Array<D3D12_RECT>(n);
          l->ClearDepthStencilView(h, f, depth, st, n, rects);
          break;
        }
        case kClearRenderTarget: {
          const auto h = r.Pod<D3D12_CPU_DESCRIPTOR_HANDLE>();
          FLOAT rgba[4];
          std::memcpy(rgba, r.p, 16);
          r.p += 16;
          const D3D12_RECT* rects = r.Array<D3D12_RECT>(n);
          l->ClearRenderTargetView(h, rgba, n, rects);
          break;
        }
        case kDrawInstanced: {
          const UINT a = r.Pod<UINT>(), b = r.Pod<UINT>(), c = r.Pod<UINT>(), d = r.Pod<UINT>();
          l->DrawInstanced(a, b, c, d);
          break;
        }
        case kIASetIndexBuffer: {
          const auto v = r.Pod<D3D12_INDEX_BUFFER_VIEW>();
          l->IASetIndexBuffer(&v);
          break;
        }
        case kDrawIndexedInstanced: {
          const UINT a = r.Pod<UINT>(), b = r.Pod<UINT>(), c = r.Pod<UINT>();
          const INT d = r.Pod<INT>();
          const UINT e = r.Pod<UINT>();
          l->DrawIndexedInstanced(a, b, c, d, e);
          break;
        }
        case kCopyTextureRegion: {
          const auto dst = r.Pod<D3D12_TEXTURE_COPY_LOCATION>();
          const UINT x = r.Pod<UINT>(), y = r.Pod<UINT>(), z = r.Pod<UINT>();
          const auto src = r.Pod<D3D12_TEXTURE_COPY_LOCATION>();
          const D3D12_BOX* box = r.Array<D3D12_BOX>(n);
          l->CopyTextureRegion(&dst, x, y, z, &src, box);
          break;
        }
        case kSetComputeRootSignature: l->SetComputeRootSignature(r.Pod<ID3D12RootSignature*>()); break;
        case kSetPipelineState: l->SetPipelineState(r.Pod<ID3D12PipelineState*>()); break;
        case kSetComputeRoot32BitConstants: {
          const UINT param = r.Pod<UINT>(), offset = r.Pod<UINT>();
          const uint32_t* data = r.Array<uint32_t>(n);
          l->SetComputeRoot32BitConstants(param, n, data, offset);
          break;
        }
        case kSetComputeRootDescriptorTable: {
          const UINT param = r.Pod<UINT>();
          l->SetComputeRootDescriptorTable(param, r.Pod<D3D12_GPU_DESCRIPTOR_HANDLE>());
          break;
        }
        case kDispatch: {
          const UINT x = r.Pod<UINT>(), y = r.Pod<UINT>(), z = r.Pod<UINT>();
          l->Dispatch(x, y, z);
          break;
        }
        case kResourceBarrier: {
          const D3D12_RESOURCE_BARRIER* b = r.Array<D3D12_RESOURCE_BARRIER>(n);
          l->ResourceBarrier(n, b);
          break;
        }
        case kBeginQuery: {
          auto* heap = r.Pod<ID3D12QueryHeap*>();
          const auto type = r.Pod<D3D12_QUERY_TYPE>();
          l->BeginQuery(heap, type, r.Pod<UINT>());
          break;
        }
        case kEndQuery: {
          auto* heap = r.Pod<ID3D12QueryHeap*>();
          const auto type = r.Pod<D3D12_QUERY_TYPE>();
          l->EndQuery(heap, type, r.Pod<UINT>());
          break;
        }
        case kResolveQueryData: {
          auto* heap = r.Pod<ID3D12QueryHeap*>();
          const auto type = r.Pod<D3D12_QUERY_TYPE>();
          const UINT start = r.Pod<UINT>(), count = r.Pod<UINT>();
          auto* dst = r.Pod<ID3D12Resource*>();
          const UINT64 offset = r.Pod<UINT64>();
          l->ResolveQueryData(heap, type, start, count, dst, offset);
          break;
        }
        case kBeginEvent: {
          const UINT meta = r.Pod<UINT>();
          const uint8_t* data = r.Array<uint8_t>(n);
          l->BeginEvent(meta, data, n);
          break;
        }
        case kEndEvent: l->EndEvent(); break;
        case kSetDescriptorHeaps: {
          ID3D12DescriptorHeap* const* heaps = r.Array<ID3D12DescriptorHeap*>(n);
          l->SetDescriptorHeaps(n, heaps);
          break;
        }
        case kSetGraphicsRootSignature: l->SetGraphicsRootSignature(r.Pod<ID3D12RootSignature*>()); break;
        case kSetGraphicsRootUav: {
          const UINT param = r.Pod<UINT>();
          l->SetGraphicsRootUnorderedAccessView(param, r.Pod<D3D12_GPU_VIRTUAL_ADDRESS>());
          break;
        }
        case kSetGraphicsRootSrv: {
          const UINT param = r.Pod<UINT>();
          l->SetGraphicsRootShaderResourceView(param, r.Pod<D3D12_GPU_VIRTUAL_ADDRESS>());
          break;
        }
        case kSetGraphicsRootDescriptorTable: {
          const UINT param = r.Pod<UINT>();
          l->SetGraphicsRootDescriptorTable(param, r.Pod<D3D12_GPU_DESCRIPTOR_HANDLE>());
          break;
        }
        case kSetGraphicsRootCbv: {
          const UINT param = r.Pod<UINT>();
          l->SetGraphicsRootConstantBufferView(param, r.Pod<D3D12_GPU_VIRTUAL_ADDRESS>());
          break;
        }
        case kOMSetRenderTargets: {
          const BOOL single = r.Pod<BOOL>();
          const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs = r.Array<D3D12_CPU_DESCRIPTOR_HANDLE>(n);
          uint32_t has_dsv = 0;
          const D3D12_CPU_DESCRIPTOR_HANDLE* dsv = r.Array<D3D12_CPU_DESCRIPTOR_HANDLE>(has_dsv);
          l->OMSetRenderTargets(n, rtvs, single, dsv);
          break;
        }
        case kIASetPrimitiveTopology: l->IASetPrimitiveTopology(r.Pod<D3D12_PRIMITIVE_TOPOLOGY>()); break;
        case kRSSetViewports: {
          const D3D12_VIEWPORT* v = r.Array<D3D12_VIEWPORT>(n);
          l->RSSetViewports(n, v);
          break;
        }
        case kRSSetScissorRects: {
          const D3D12_RECT* rects = r.Array<D3D12_RECT>(n);
          l->RSSetScissorRects(n, rects);
          break;
        }
        case kOMSetStencilRef: l->OMSetStencilRef(r.Pod<UINT>()); break;
        case kSetSamplePositions: {  // patch 0089
          const D3D12_SAMPLE_POSITION* p = r.Array<D3D12_SAMPLE_POSITION>(n);
          DoSetSamplePositions(l, n, p);
          break;
        }
        case kDecompressDepth: {  // patch 0089
          ID3D12Resource* res = r.Pod<ID3D12Resource*>();
          const DXGI_FORMAT fmt = r.Pod<DXGI_FORMAT>();
          DoDecompressDepth(l, res, fmt);
          break;
        }
        case kOMSetBlendFactor: {
          FLOAT rgba[4];
          std::memcpy(rgba, r.p, 16);
          r.p += 16;
          l->OMSetBlendFactor(rgba);
          break;
        }
        default:
          Fail(E_UNEXPECTED, "command proxy: unknown op");
          return;
      }
    }
  }
  void Run() {
    using SetDesc = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    if (auto fn = reinterpret_cast<SetDesc>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")))
      fn(GetCurrentThread(), L"GPU Recording (ours)");
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      cv_.wait(lock, [this] { return stop_ || !ready_.empty(); });
      if (ready_.empty() && stop_) break;
      std::vector<uint8_t> block = std::move(ready_.front());
      ready_.pop_front();
      busy_ = true;
      lock.unlock();
      Replay(block);
      lock.lock();
      busy_ = false;
      block.clear();
      if (free_.size() < 64) free_.push_back(std::move(block));
      if (ready_.empty()) idle_cv_.notify_all();
    }
  }

  ID3D12GraphicsCommandList* real_ = nullptr;
  ID3D12CommandQueue* queue_ = nullptr;
  bool threaded_ = false;
  std::vector<D3D12_RESOURCE_BARRIER> pending_;  // job 046: queued barriers, emitted together before the next call
  std::vector<uint8_t> cur_;
  std::mutex mutex_;
  std::condition_variable cv_, idle_cv_;
  std::deque<std::vector<uint8_t>> ready_;
  std::vector<std::vector<uint8_t>> free_;
  bool busy_ = false, stop_ = false;
  std::thread worker_;
  std::atomic<HRESULT> error_{S_OK};
  std::atomic<const char*> error_what_{nullptr};
};

class D3D12Backend final : public Backend {
 public:
  ~D3D12Backend() override { Shutdown(); }

  bool Init(const BackendOptions& options, std::string& error) override {
    options_ = options;
    HRESULT hr = S_OK;
    if (options.external_device) {  // Stage 5C-2: the presenter's device
      device_ = static_cast<ID3D12Device*>(options.external_device);
      device_->AddRef();
    } else {
      hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_));
      if (FAILED(hr)) { error = Hr("D3D12CreateDevice", hr); return false; }
    }
    // 0.2.0 (#9): which Direct3D 12 runtime is loaded - the Agility SDK copy in <exe>\D3D12\ (D3D12SDKVersion export,
    // agility.cpp) or Windows' own; Windows 10's own runtime doesn't know Shader Model 6.6 at all.
    std::string runtime = "Windows' own";
    if (HMODULE core = GetModuleHandleW(L"D3D12Core.dll")) {
      wchar_t p[MAX_PATH] = L"";
      GetModuleFileNameW(core, p, MAX_PATH);
      if (wcsstr(p, L"\\D3D12\\D3D12Core.dll")) runtime = "Agility SDK (bundled)";
    }
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
    const HRESULT smhr = device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
    if (FAILED(smhr) || sm.HighestShaderModel < D3D_SHADER_MODEL_6_6) {
      // Microsoft's advice: an older runtime rejects a model it doesn't know - ask again with lower ones
      unsigned highest = FAILED(smhr) ? 0u : unsigned(sm.HighestShaderModel);
      for (D3D_SHADER_MODEL m : {D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2,
                                 D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0, D3D_SHADER_MODEL_5_1}) {
        if (highest) break;
        D3D12_FEATURE_DATA_SHADER_MODEL t{m};
        if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &t, sizeof(t)))) highest = unsigned(t.HighestShaderModel);
      }
      char b[256];
      std::snprintf(b, sizeof b, "Shader Model 6.6 is not available (query 0x%08X, highest %u.%u, Direct3D 12 runtime: %s)",
                    unsigned(smhr), highest >> 4, highest & 15u, runtime.c_str());
      error = b;
      return false;
    }
    REXLOG_INFO("gow2_renderer: Direct3D 12 runtime: {}; Shader Model {}.{}", runtime, unsigned(sm.HighestShaderModel) >> 4,
                unsigned(sm.HighestShaderModel) & 15u);
    {
      D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
      stencil_ref_export_ = SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o))) &&
                            o.PSSpecifiedStencilRefSupported;
    }
    {  // patch 0089: SetSamplePositions and ResolveSubresourceRegion need tier 1 (Canary: they remove the device without)
      D3D12_FEATURE_DATA_D3D12_OPTIONS2 o2{};
      sample_positions_ = SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &o2, sizeof(o2))) &&
                          o2.ProgrammableSamplePositionsTier != D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED;
    }
    description_ = "D3D12";
    {
      IDXGIFactory4* factory = nullptr;
      if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        IDXGIAdapter1* adapter = nullptr;
        if (SUCCEEDED(factory->EnumAdapterByLuid(device_->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) {
          DXGI_ADAPTER_DESC1 d{};
          adapter->GetDesc1(&d);
          char name[128] = {};
          WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
          description_ = std::string("D3D12 ") + name + ", Shader Model 6.6";
          adapter->QueryInterface(IID_PPV_ARGS(&adapter3_));  // patch 0032: VideoMemory
          adapter->Release();
        }
        factory->Release();
      }
    }
    if (options.debug_layer) {
      if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&info_queue_)))) {
        description_ += ", debug layer messages forwarded";
      } else {
        description_ += ", debug layer NOT active (add the SDK's --d3d12_debug)";
      }
    }
    threaded_recording_ = options.threaded_recording;
    copies_.Start(options.copy_threads, &stats_);
    if (options.copy_threads) description_ += ", " + std::to_string(std::min<uint32_t>(options.copy_threads, 16)) + " copy threads";
    if (options.external_queue) {  // Stage 5C-2: the presenter's direct queue (its work and ours stay in order)
      queue_ = static_cast<ID3D12CommandQueue*>(options.external_queue);
      queue_->AddRef();
      description_ += ", SHARED device and queue (the presenter's)";
    } else {
      D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
      if (FAILED(hr = device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)))) { error = Hr("CreateCommandQueue", hr); return false; }
    }
    if (FAILED(hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) { error = Hr("CreateFence", hr); return false; }
    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) { error = "CreateEvent failed"; return false; }
    if (!CreateRootSignatures(error)) return false;
    if (!options.pipeline_library_path.empty()) OpenPipelineLibrary();

    // Descriptor heaps.
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kShaderHeapSize,
                                  D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(hr = device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&shader_heap_)))) { error = Hr("CreateDescriptorHeap(view)", hr); return false; }
    D3D12_DESCRIPTOR_HEAP_DESC sd{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize,
                                  D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    for (SamplerPool& pool : sampler_pools_)
      if (FAILED(hr = device_->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&pool.heap)))) { error = Hr("CreateDescriptorHeap(sampler)", hr); return false; }
    view_increment_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!rtv_heap_.Init(device_, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kCpuHeapSize) ||
        !dsv_heap_.Init(device_, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kCpuHeapSize) ||
        !view_heap_.Init(device_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViewHeapSize) ||
        !default_table_heap_.Init(device_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kGraphicsSrvCount)) {
      error = "CreateDescriptorHeap(CPU) failed";
      return false;
    }
    null_rtv_ = rtv_heap_.Alloc();
    if (null_rtv_ >= 0) {
      D3D12_RENDER_TARGET_VIEW_DESC nd{};
      nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      nd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
      device_->CreateRenderTargetView(nullptr, &nd, rtv_heap_.Handle(null_rtv_));
    }

    // Upload ring (persistently mapped).
    upload_size_ = uint64_t(std::max<uint32_t>(options.upload_ring_mb, 16)) << 20;
    if (!CreateBuffer(upload_size_, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ,
                      &upload_, error)) return false;
    D3D12_RANGE none{0, 0};
    if (FAILED(hr = upload_->Map(0, &none, reinterpret_cast<void**>(&upload_mapped_)))) { error = Hr("Map(upload)", hr); return false; }
    upload_gpu_ = upload_->GetGPUVirtualAddress();
    // 64 KB of zeros for constant slots that were never set.
    if (!CreateBuffer(65536, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ,
                      &zero_cb_, error)) return false;
    {
      void* z = nullptr;
      if (FAILED(hr = zero_cb_->Map(0, &none, &z))) { error = Hr("Map(zero)", hr); return false; }
      std::memset(z, 0, 65536);
      zero_cb_->Unmap(0, nullptr);
      zero_cb_gpu_ = zero_cb_->GetGPUVirtualAddress();
    }

    // Guest physical memory on the GPU (t96), starts zeroed by the driver (committed resources are zeroed unless
    // D3D12_HEAP_FLAG_CREATE_NOT_ZEROED is passed).
    // UAV-capable since takeover T2: our resolves write their results into it (ComputeBindings::guest_u0).
    if (!CreateBuffer(options.guest_memory_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COPY_DEST, &guest_, error)) return false;
    guest_state_ = D3D12_RESOURCE_STATE_COMMON;

    // Default textures for every graphics slot (opaque white, so texture-dependent kills keep the geometry; M1 has no
    // texture cache - notes/44) and the static graphics table.
    if (!CreateDefaultTextures(error)) return false;
    D3D12_CPU_DESCRIPTOR_HANDLE table = shader_heap_->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < 96; ++i) {
      const uint32_t kind = i / 32;  // 0 2D, 1 cube, 2 3D
      D3D12_SHADER_RESOURCE_VIEW_DESC v{};
      v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      if (kind == 0) { v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = 1; }
      else if (kind == 1) { v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE; v.TextureCube.MipLevels = 1; }
      else { v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; v.Texture3D.MipLevels = 1; }
      device_->CreateShaderResourceView(default_textures_[kind], &v, {table.ptr + SIZE_T(i) * view_increment_});
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC gv{};
    gv.Format = DXGI_FORMAT_R32_TYPELESS;
    gv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    gv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    gv.Buffer.NumElements = UINT(options.guest_memory_bytes / 4);
    gv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device_->CreateShaderResourceView(guest_, &gv, {table.ptr + SIZE_T(96) * view_increment_});
    // Null views per dimension (read 0,0,0,0): TextureView::zero with no texture (Canary's null SRV for a fetch whose
    // dimension doesn't match the texture - notes/51 section 14).
    for (uint32_t kind = 0; kind < 3; ++kind) {
      null_views_[kind] = view_heap_.Alloc();
      if (null_views_[kind] < 0) continue;
      D3D12_SHADER_RESOURCE_VIEW_DESC nv{};
      nv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      nv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      if (kind == 0) { nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; nv.Texture2D.MipLevels = 1; }
      else if (kind == 1) { nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE; nv.TextureCube.MipLevels = 1; }
      else { nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; nv.Texture3D.MipLevels = 1; }
      device_->CreateShaderResourceView(nullptr, &nv, view_heap_.Handle(null_views_[kind]));
    }
    guest_uav_ = view_heap_.Alloc();  // T2: raw UAV of the guest-memory buffer (copied into compute tables)
    if (guest_uav_ >= 0) {
      D3D12_UNORDERED_ACCESS_VIEW_DESC gu{};
      gu.Format = DXGI_FORMAT_R32_TYPELESS;
      gu.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      gu.Buffer.NumElements = UINT(options.guest_memory_bytes / 4);
      gu.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
      device_->CreateUnorderedAccessView(guest_, nullptr, &gu, view_heap_.Handle(guest_uav_));
    }
    // The same 97 descriptors in a CPU-only heap: the source of per-draw texture tables (descriptor copies must not
    // read from a shader-visible heap).
    for (uint32_t i = 0; i < kGraphicsSrvCount; ++i) default_table_heap_.Alloc();  // slots 0..96 in order
    {
      D3D12_CPU_DESCRIPTOR_HANDLE cpu = default_table_heap_.Handle(0);
      for (uint32_t i = 0; i < 96; ++i) {
        const uint32_t kind = i / 32;
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (kind == 0) { v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = 1; }
        else if (kind == 1) { v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE; v.TextureCube.MipLevels = 1; }
        else { v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; v.Texture3D.MipLevels = 1; }
        device_->CreateShaderResourceView(default_textures_[kind], &v, {cpu.ptr + SIZE_T(i) * view_increment_});
      }
      device_->CreateShaderResourceView(guest_, &gv, {cpu.ptr + SIZE_T(96) * view_increment_});
    }
    sampler_increment_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    for (SamplerPool& pool : sampler_pools_) {
      pool.tables.resize(kSamplerTables);
      pool.tables[0].size = 32;
      pool.tables[0].offset = 0;
      uint32_t index = 1, offset = 32;
      for (uint32_t c = 0; c < kSamplerClasses; ++c)
        for (uint32_t k = 0; k < kSamplerClassTables[c]; ++k, ++index) {
          pool.tables[index].size = kSamplerClassSize[c];
          pool.tables[index].offset = offset;
          offset += kSamplerClassSize[c];
        }
    }
    // Samplers: linear, wrap (M1 stub; the texture cache will write per-draw sampler tables).
    const UINT sinc = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    for (SamplerPool& pool : sampler_pools_)
    for (uint32_t i = 0; i < kSamplerHeapSize; ++i) {
      const D3D12_CPU_DESCRIPTOR_HANDLE st = pool.heap->GetCPUDescriptorHandleForHeapStart();
      D3D12_SAMPLER_DESC s{};
      s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
      s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
      s.MaxLOD = D3D12_FLOAT32_MAX;
      s.MaxAnisotropy = 1;
      s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
      device_->CreateSampler(&s, {st.ptr + SIZE_T(i) * sinc});
    }
    ring_head_ = kShaderHeapRingStart;
    ring_tail_ = kShaderHeapRingStart;
    textures_.resize(1);   // handle 0 = none
    shaders_.resize(1);
    pipelines_.emplace_back();  // handle 0 = none
    readbacks_.resize(1);
    ready_ = true;
    return true;
  }

  std::string Describe() const override { return description_; }
  ShaderIL GetShaderIL() const override { return ShaderIL::kDxil; }
  bool SupportsStencilRefExport() const override { return stencil_ref_export_; }

  bool IsLost(std::string* reason) override {
    // GetDeviceRemovedReason is a kernel call (job 156: ~11% of our GPU thread when asked per draw): ask at most every
    // 50 ms, or whenever the caller wants the reason. A removed device also fails our fence waits and submits.
    const uint64_t now_ms = GetTickCount64();
    if (!lost_ && device_ && (reason || now_ms - lost_checked_ms_ >= 50)) {
      lost_checked_ms_ = now_ms;
      const HRESULT r = device_->GetDeviceRemovedReason();
      if (FAILED(r)) {
        lost_ = true;
        lost_reason_ = Hr("device", r);
      }
    }
    if (lost_ && !loss_reported_ && device_) ReportDeviceLoss();
    if (reason) *reason = lost_reason_;
    return lost_ || !ready_;
  }

  // Patch 0006: once, when the loss is first seen - the removal reason and, with DRED on (EnableDeviceLossDiagnostics),
  // the command lists the GPU had not finished (their last completed operation and the ones around it) and the GPU
  // virtual address of a page fault with the allocations at / near it (our textures carry debug names).
  void ReportDeviceLoss() {
    loss_reported_ = true;
    const HRESULT why = device_->GetDeviceRemovedReason();
    const char* name = why == DXGI_ERROR_DEVICE_HUNG             ? "DEVICE_HUNG (the GPU stopped making progress - "
                                                                   "a very long or endless shader / wait)"
                       : why == DXGI_ERROR_DEVICE_REMOVED        ? "DEVICE_REMOVED (driver update / adapter removed)"
                       : why == DXGI_ERROR_DEVICE_RESET          ? "DEVICE_RESET (bad command or timeout - TDR)"
                       : why == DXGI_ERROR_DRIVER_INTERNAL_ERROR ? "DRIVER_INTERNAL_ERROR"
                       : why == DXGI_ERROR_INVALID_CALL          ? "INVALID_CALL (an invalid command reached the GPU)"
                       : why == S_OK                             ? "S_OK (device not removed?)"
                                                                 : "other";
    REXLOG_ERROR("gow2_renderer: DEVICE LOST: removed reason 0x{:08X} = {}; first failure seen: {}",
                 static_cast<uint32_t>(why), name, lost_reason_);
    ID3D12DeviceRemovedExtendedData* dred = nullptr;
    if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&dred))) || !dred) {
      REXLOG_ERROR("gow2_renderer: DEVICE LOST: no DRED data (not enabled before the device was created, or Windows "
                   "older than 1903)");
      return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs))) {
      uint32_t nodes = 0, unfinished = 0;
      for (const D3D12_AUTO_BREADCRUMB_NODE* n = crumbs.pHeadAutoBreadcrumbNode; n && nodes < 4096;
           n = n->pNext, ++nodes) {
        const uint32_t count = n->BreadcrumbCount;
        const uint32_t done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
        if (done >= count) continue;  // this list finished on the GPU
        if (++unfinished > 6) continue;
        std::string ops;
        const uint32_t from = done >= 4 ? done - 4 : 0, to = std::min(count, done + 4);
        for (uint32_t i = from; n->pCommandHistory && i < to; ++i) {
          if (!ops.empty()) ops += ' ';
          ops += (i == done ? "[" : "") + std::to_string(int(n->pCommandHistory[i])) + (i == done ? "]" : "");
        }
        auto narrow = [](const wchar_t* w) {
          std::string s;
          for (; w && *w && s.size() < 80; ++w) s += char(*w < 128 ? *w : '?');
          return s;
        };
        REXLOG_ERROR("gow2_renderer: DEVICE LOST: unfinished command list '{}' on queue '{}': {} of {} operations done; "
                     "ops around the stop (D3D12_AUTO_BREADCRUMB_OP numbers, [next]): {}",
                     narrow(n->pCommandListDebugNameW), narrow(n->pCommandQueueDebugNameW), done, count, ops);
      }
      REXLOG_ERROR("gow2_renderer: DEVICE LOST: DRED breadcrumbs: {} command list(s) recorded, {} unfinished", nodes,
                   unfinished);
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault))) {
      auto list = [](const D3D12_DRED_ALLOCATION_NODE* a) {
        std::string s;
        for (uint32_t i = 0; a && i < 8; a = a->pNext, ++i) {
          if (!s.empty()) s += ", ";
          const wchar_t* w = a->ObjectNameW;
          std::string n;
          for (; w && *w && n.size() < 60; ++w) n += char(*w < 128 ? *w : '?');
          s += (n.empty() ? std::string("(unnamed)") : n) + " type " + std::to_string(int(a->AllocationType));
        }
        return s.empty() ? std::string("none") : s;
      };
      REXLOG_ERROR("gow2_renderer: DEVICE LOST: page fault at GPU VA 0x{:016X}; live allocations there: {}; recently "
                   "freed there: {}",
                   static_cast<uint64_t>(fault.PageFaultVA), list(fault.pHeadExistingAllocationNode),
                   list(fault.pHeadRecentFreedAllocationNode));
    }
    dred->Release();
  }

  // ---- resources ----
  TextureHandle CreateTexture(const TextureDesc& desc) override {
    if (!ready_ || lost_) return 0;
    Texture t;
    t.desc = desc;
    t.serial = ++texture_serial_;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = desc.dimension == TextureDimension::k3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                                           : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = std::max<uint32_t>(desc.width, 1);
    rd.Height = std::max<uint32_t>(desc.height, 1);
    rd.DepthOrArraySize = UINT16(desc.dimension == TextureDimension::kCube ? 6 : std::max<uint32_t>(desc.depth_or_layers, 1));
    rd.MipLevels = UINT16(std::max<uint32_t>(desc.mips, 1));
    rd.Format = ResourceFormat(desc.format);
    if (TypelessFormat(desc.format) != DXGI_FORMAT_UNKNOWN && !(desc.usage & (kUsageRenderTarget | kUsageDepthStencil))) {
      rd.Format = TypelessFormat(desc.format);
      t.typeless = true;
    }
    if ((desc.usage & kUsageRenderTarget) && !(desc.usage & kUsageStorage) && BitsFormat(desc.format) != DXGI_FORMAT_UNKNOWN) {
      rd.Format = BitsTypeless(desc.format);
      t.bits = true;
    }
    rd.SampleDesc.Count = std::max<uint32_t>(desc.samples, 1);
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (desc.usage & kUsageRenderTarget) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (desc.usage & kUsageDepthStencil) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (desc.usage & kUsageStorage) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_CLEAR_VALUE clear{};
    const D3D12_CLEAR_VALUE* clear_ptr = nullptr;
    D3D12_RESOURCE_STATES initial = D3D12_RESOURCE_STATE_COMMON;
    if (desc.usage & kUsageDepthStencil) {
      clear.Format = ViewFormat(desc.format);
      clear.DepthStencil.Depth = desc.clear_depth;
      clear.DepthStencil.Stencil = 0;
      clear_ptr = &clear;
      initial = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    } else if (desc.usage & kUsageRenderTarget) {
      clear.Format = ViewFormat(desc.format);
      clear_ptr = &clear;
      initial = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    HRESULT hr = E_FAIL;
    // Patch 0017: a sampled-only texture goes into a shared heap (no allocation of its own; its bytes are undefined
    // until written - every level the texture cache creates is uploaded or untiled before it is sampled).
    if (options_.placed_textures && !(desc.usage & (kUsageRenderTarget | kUsageDepthStencil)) && rd.SampleDesc.Count == 1) {
      const D3D12_RESOURCE_ALLOCATION_INFO ai = device_->GetResourceAllocationInfo(0, 1, &rd);
      if (ai.SizeInBytes != UINT64_MAX && ai.SizeInBytes <= kTexturePlacedMax && ai.Alignment <= kTexturePlacedAlign) {
        const uint64_t size = (ai.SizeInBytes + kTexturePlacedAlign - 1) & ~(kTexturePlacedAlign - 1);
        int32_t heap = -1;
        uint64_t offset = 0;
        if (AllocTextureRange(size, heap, offset)) {
          hr = device_->CreatePlacedResource(tex_heaps_[size_t(heap)].heap, offset, &rd, initial, nullptr,
                                             IID_PPV_ARGS(&t.resource));
          if (SUCCEEDED(hr)) {
            t.heap = heap;
            t.heap_offset = offset;
            t.heap_size = size;
            ++stats_.textures_placed;
            placed_live_bytes_ += size;  // job 306
            ++placed_live_count_;
          } else {
            FreeTextureRange(heap, offset, size);
            t.resource = nullptr;
          }
        }
      }
    }
    if (!t.resource) {
      D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
      hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initial, clear_ptr, IID_PPV_ARGS(&t.resource));
    }
    if (FAILED(hr)) {
      REXLOG_ERROR("gow2_renderer: {} ({}x{} format {})", Hr("CreateCommittedResource(texture)", hr), desc.width,
                   desc.height, uint32_t(desc.format));
      return 0;
    }
    t.state = initial;
    if (desc.debug_name) {
      wchar_t w[128] = {};
      MultiByteToWideChar(CP_UTF8, 0, desc.debug_name, -1, w, 127);
      t.resource->SetName(w);
    }
    if (desc.usage & kUsageRenderTarget) {
      t.rtv = rtv_heap_.Alloc();
      if (t.bits) {
        D3D12_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = ResourceFormat(desc.format);
        rv.ViewDimension = desc.samples > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
        if (t.rtv >= 0) device_->CreateRenderTargetView(t.resource, &rv, rtv_heap_.Handle(t.rtv));
        rv.Format = BitsFormat(desc.format);
        t.rtv_bits = rtv_heap_.Alloc();
        if (t.rtv_bits >= 0) device_->CreateRenderTargetView(t.resource, &rv, rtv_heap_.Handle(t.rtv_bits));
      } else if (t.rtv >= 0) {
        device_->CreateRenderTargetView(t.resource, nullptr, rtv_heap_.Handle(t.rtv));
      }
    }
    if (desc.usage & kUsageDepthStencil) {
      t.dsv = dsv_heap_.Alloc();
      D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
      dv.Format = ViewFormat(desc.format);
      dv.ViewDimension = desc.samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
      if (t.dsv >= 0) device_->CreateDepthStencilView(t.resource, &dv, dsv_heap_.Handle(t.dsv));
    }
    if (desc.usage & kUsageSampled) {
      const bool ms = desc.samples > 1;
      if (IsDepthFormat(desc.format)) {
        for (uint32_t plane = 0; plane < 2; ++plane) {
          t.srv[plane] = view_heap_.Alloc();
          if (t.srv[plane] < 0) break;
          D3D12_SHADER_RESOURCE_VIEW_DESC v{};
          v.Format = DepthPlaneFormat(desc.format, plane);
          v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
          if (ms) {
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
          } else {
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Texture2D.MipLevels = 1;
            v.Texture2D.PlaneSlice = plane;
          }
          device_->CreateShaderResourceView(t.resource, &v, view_heap_.Handle(t.srv[plane]));
        }
      } else if (t.bits) {
        t.srv[0] = view_heap_.Alloc();
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = ResourceFormat(desc.format);
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (ms) {
          v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        } else {
          v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
          v.Texture2D.MipLevels = UINT(std::max<uint32_t>(desc.mips, 1));
        }
        if (t.srv[0] >= 0) device_->CreateShaderResourceView(t.resource, &v, view_heap_.Handle(t.srv[0]));
      } else if (!t.typeless) {
        t.srv[0] = view_heap_.Alloc();
        if (t.srv[0] >= 0) device_->CreateShaderResourceView(t.resource, nullptr, view_heap_.Handle(t.srv[0]));
      }
    }
    if (desc.usage & kUsageStorage) {
      t.uav = view_heap_.Alloc();
      if (t.uav >= 0) {
        // Typed view (the resource may be typeless when the format has a signed twin).
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = ResourceFormat(desc.format);
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        if (desc.dimension == TextureDimension::k3D) {  // Gears 3: 3D storage textures (stacked loads), all slices
          u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
          u.Texture3D.MipSlice = 0;
          u.Texture3D.FirstWSlice = 0;
          u.Texture3D.WSize = UINT(-1);
        }
        device_->CreateUnorderedAccessView(
            t.resource, nullptr,
            desc.dimension == TextureDimension::k2D || desc.dimension == TextureDimension::k3D ? &u : nullptr,
            view_heap_.Handle(t.uav));
      }
    }
    for (size_t i = 1; i < textures_.size(); ++i) {
      if (!textures_[i].resource) {
        textures_[i] = t;
        return TextureHandle(i);
      }
    }
    textures_.push_back(t);
    return TextureHandle(textures_.size() - 1);
  }

  bool VideoMemory(uint64_t* used, uint64_t* budget) override {
    if (!adapter3_) return false;
    DXGI_QUERY_VIDEO_MEMORY_INFO m{};
    if (FAILED(adapter3_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &m))) return false;
    *used = m.CurrentUsage;
    *budget = m.Budget;
    return true;
  }
  void DestroyTexture(TextureHandle h) override {
    if (h == 0 || h >= textures_.size() || !textures_[h].resource) return;
    if (textures_[h].scratch_pending) FlushScratchCopies();  // batched texture loads: before its copy can't happen
    // Job 188: with threaded recording, encoded calls still name this texture's CPU descriptors (RTV/DSV are read when
    // the recording thread replays OMSetRenderTargets / clears). Freed now, a texture created next would reuse those
    // slots before the replay and the old calls would hit the new texture - the transfer self-test chains read 0 from
    // job 175 on. Let the recording thread catch up first (rare: evictions, size changes, the self-test).
    list_->Drain();
    Texture& t = textures_[h];
    textures_dirty_ = textures_dirty_ || textures_custom_;  // its handle may come back for another texture
    deferred_releases_.push_back({t.resource, next_fence_, t.heap, t.heap_offset, t.heap_size});
    rtv_heap_.Free(t.rtv);
    rtv_heap_.Free(t.rtv_bits);
    dsv_heap_.Free(t.dsv);
    view_heap_.Free(t.srv[0]);
    view_heap_.Free(t.srv[1]);
    view_heap_.Free(t.uav);
    for (int32_t slot : t.uav_mips) view_heap_.Free(slot);
    for (const auto& [key, slot] : t.views) view_heap_.Free(slot);
    t = Texture{};
    for (TextureHandle& b : bound_colors_) if (b == h) b = 0;
    if (bound_depth_ == h) bound_depth_ = 0;
    for (TextureView& v : bound_views_)  // a reused handle must not match the old table (texture eviction, upl-n108)
      if (v.texture == h) {
        v.texture = 0;
        textures_dirty_ = true;
      }
  }

  TextureHandle ImportTexture(void* native_resource, Format format, uint32_t state_now) override {
    if (!ready_ || lost_ || !native_resource) return 0;
    Texture t;
    t.resource = static_cast<ID3D12Resource*>(native_resource);
    t.resource->AddRef();  // balanced by DestroyTexture's deferred release
    const D3D12_RESOURCE_DESC rd = t.resource->GetDesc();
    t.desc.width = uint32_t(rd.Width);
    t.desc.height = rd.Height;
    t.desc.format = format;
    t.desc.usage = kUsageSampled | ((rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ? kUsageStorage : 0u);
    t.serial = ++texture_serial_;
    t.state = D3D12_RESOURCE_STATES(state_now);
    t.srv[0] = view_heap_.Alloc();
    if (t.srv[0] >= 0) device_->CreateShaderResourceView(t.resource, nullptr, view_heap_.Handle(t.srv[0]));
    if (t.desc.usage & kUsageStorage) {
      t.uav = view_heap_.Alloc();
      if (t.uav >= 0) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = ResourceFormat(format);
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(t.resource, nullptr, &u, view_heap_.Handle(t.uav));
      }
    }
    for (size_t i = 1; i < textures_.size(); ++i) {
      if (!textures_[i].resource) {
        textures_[i] = t;
        return TextureHandle(i);
      }
    }
    textures_.push_back(t);
    return TextureHandle(textures_.size() - 1);
  }
  void ReleaseImport(TextureHandle h, uint32_t final_state) override {
    if (!ValidTexture(h)) return;
    if (Recording()) Transition(textures_[h], D3D12_RESOURCE_STATES(final_state));
    DestroyTexture(h);
  }

  ShaderHandle CreateShader(ShaderStage stage, const void* code, size_t size) override {
    if (!code || !size) return 0;
    Shader s;
    s.stage = stage;
    s.code = std::make_shared<const std::vector<uint8_t>>(static_cast<const uint8_t*>(code),
                                                          static_cast<const uint8_t*>(code) + size);  // patch 0058
    shaders_.push_back(std::move(s));
    return ShaderHandle(shaders_.size() - 1);
  }

  // Patch 0068: the D3D12 description of a pipeline (was inline in GetGraphicsPipeline; also the warm template).
  void BuildPipelineDesc(const GraphicsPipelineDesc& d, D3D12_GRAPHICS_PIPELINE_STATE_DESC& pd,
                         D3D_PRIMITIVE_TOPOLOGY& topology) {
    pd.pRootSignature = graphics_root_;
    auto bytecode = [&](ShaderHandle h) {
      D3D12_SHADER_BYTECODE b{};
      if (h && h < shaders_.size() && shaders_[h].code) b = {shaders_[h].code->data(), shaders_[h].code->size()};
      return b;
    };
    pd.VS = bytecode(d.vs);
    pd.PS = bytecode(d.ps);
    pd.GS = bytecode(d.gs);
    for (uint32_t i = 0; i < 8; ++i) pd.BlendState.RenderTarget[i].RenderTargetWriteMask = 0;
    static const D3D12_BLEND kFactor[] = {
        D3D12_BLEND_ZERO,         D3D12_BLEND_ONE,           D3D12_BLEND_SRC_COLOR,  D3D12_BLEND_INV_SRC_COLOR,
        D3D12_BLEND_SRC_ALPHA,    D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_COLOR, D3D12_BLEND_INV_DEST_COLOR,
        D3D12_BLEND_DEST_ALPHA,   D3D12_BLEND_INV_DEST_ALPHA, D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_INV_BLEND_FACTOR,
        D3D12_BLEND_SRC_ALPHA_SAT};
    static const D3D12_BLEND_OP kOp[] = {D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_SUBTRACT, D3D12_BLEND_OP_MIN,
                                         D3D12_BLEND_OP_MAX, D3D12_BLEND_OP_REV_SUBTRACT};
    for (uint32_t i = 0; i < d.color_count && i < GraphicsPipelineDesc::kMaxColor; ++i) {
      D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[i];
      const BlendDesc& s = d.color_blend[i];
      b.LogicOpEnable = FALSE;
      b.BlendEnable = s.enable ? TRUE : FALSE;
      b.SrcBlend = s.enable ? kFactor[uint32_t(s.src)] : D3D12_BLEND_ONE;
      b.DestBlend = s.enable ? kFactor[uint32_t(s.dst)] : D3D12_BLEND_ZERO;
      b.BlendOp = s.enable ? kOp[uint32_t(s.op)] : D3D12_BLEND_OP_ADD;
      b.SrcBlendAlpha = s.enable ? kFactor[uint32_t(s.src_alpha)] : D3D12_BLEND_ONE;
      b.DestBlendAlpha = s.enable ? kFactor[uint32_t(s.dst_alpha)] : D3D12_BLEND_ZERO;
      b.BlendOpAlpha = s.enable ? kOp[uint32_t(s.op_alpha)] : D3D12_BLEND_OP_ADD;
      b.LogicOp = D3D12_LOGIC_OP_NOOP;
      b.RenderTargetWriteMask = d.color_formats[i] == Format::kUnknown ? 0 : (d.color_write_masks[i] & 0xF);
    }
    pd.BlendState.IndependentBlendEnable = TRUE;
    pd.SampleMask = UINT_MAX;
    D3D12_RASTERIZER_DESC& r = pd.RasterizerState;
    r.FillMode = d.wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    r.CullMode = d.cull == CullMode::kFront ? D3D12_CULL_MODE_FRONT
                 : d.cull == CullMode::kBack ? D3D12_CULL_MODE_BACK
                                             : D3D12_CULL_MODE_NONE;
    r.FrontCounterClockwise = d.front_ccw ? TRUE : FALSE;
    r.DepthBias = d.depth_bias;
    r.DepthBiasClamp = 0.0f;
    r.SlopeScaledDepthBias = d.depth_bias_slope;
    r.DepthClipEnable = d.depth_clip ? TRUE : FALSE;
    r.MultisampleEnable = d.samples > 1 ? TRUE : FALSE;
    r.AntialiasedLineEnable = FALSE;
    r.ForcedSampleCount = 0;
    r.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    D3D12_DEPTH_STENCIL_DESC& ds = pd.DepthStencilState;
    ds.DepthEnable = d.depth_enable ? TRUE : FALSE;
    ds.DepthWriteMask = d.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = Compare(d.depth_func);
    ds.StencilEnable = d.stencil_enable ? TRUE : FALSE;
    ds.StencilReadMask = d.stencil_read_mask;
    ds.StencilWriteMask = d.stencil_write_mask;
    auto face = [](const StencilFace& f) {
      D3D12_DEPTH_STENCILOP_DESC o{};
      o.StencilFailOp = StencilOpD3D(f.fail);
      o.StencilDepthFailOp = StencilOpD3D(f.depth_fail);
      o.StencilPassOp = StencilOpD3D(f.pass);
      o.StencilFunc = Compare(f.func);
      return o;
    };
    ds.FrontFace = face(d.stencil_front);
    ds.BackFace = face(d.stencil_back);
    pd.IBStripCutValue = d.primitive_restart ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                             : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    switch (d.topology) {
      case Topology::kPointList:
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        break;
      case Topology::kLineList:
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        break;
      case Topology::kLineStrip:
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        break;
      case Topology::kTriangleList:
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        break;
      case Topology::kTriangleStrip:
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        break;
    }
    pd.NumRenderTargets = std::min<uint32_t>(d.color_count, GraphicsPipelineDesc::kMaxColor);
    for (uint32_t i = 0; i < pd.NumRenderTargets; ++i)
      pd.RTVFormats[i] = d.color_formats[i] == Format::kUnknown ? DXGI_FORMAT_UNKNOWN
                         : d.color_bits && BitsFormat(d.color_formats[i]) != DXGI_FORMAT_UNKNOWN
                             ? BitsFormat(d.color_formats[i])
                             : ViewFormat(d.color_formats[i]);
    pd.DSVFormat = d.depth_format == Format::kUnknown ? DXGI_FORMAT_UNKNOWN : ViewFormat(d.depth_format);
    pd.SampleDesc.Count = std::max<uint32_t>(d.samples, 1);
    pd.NodeMask = 0;
  }

  PipelineHandle GetGraphicsPipeline(const GraphicsPipelineDesc& d, bool async, bool* pending) override {
    if (pending) *pending = false;
    if (!ready_ || lost_) return 0;
    // Consecutive draws very often use the same pipeline: the last ready one is answered from here, without hashing
    // the description and looking it up (pipelines are never destroyed, and a ready one stays ready).
    if (last_pipeline_ && d == last_pipeline_desc_) {
      ++stats_.pipeline_memo_hits;
      return last_pipeline_;
    }
    const uint64_t key = HashPipelineDesc(d);
    auto it = pipeline_cache_.find(key);
    if (it != pipeline_cache_.end()) {
      for (const auto& [desc, handle] : it->second) {
        if (!(desc == d)) continue;
        if (!handle) return 0;
        Pipeline& p = pipelines_[handle];
        if (p.ready.load(std::memory_order_acquire)) {
          if (p.failed.load()) return 0;
          last_pipeline_desc_ = d;
          last_pipeline_ = handle;
          return handle;
        }
        if (!pipeline_backlog_ && (pending || !async)) PromoteBacklogJob(handle, p);  // patch 0062
        if (async) {
          if (pending) {
            ++stats_.pipelines_pending_draws;
            *pending = true;
          }
          return 0;
        }
        while (!p.ready.load(std::memory_order_acquire)) std::this_thread::yield();  // a sync user of a pending one
        return p.failed.load() ? 0 : handle;
      }
    }
    ++pipeline_misses_;  // patch 0058: new to this run
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    Pipeline p;
    BuildPipelineDesc(d, pd, p.topology);
    // Saved pipelines: one already in the library loads in ~1 ms, synchronously - no pending draws for it.
    std::wstring library_name;
    if (pipeline_library_) library_name = PipelineLibraryName(pd);
    // Patch 0010: with pipeline_load_async, an asynchronous request leaves the library load to the worker.
    if (pipeline_library_ && !(async && options_.pipeline_load_async && StartPipelineWorkers())) {
      ID3D12PipelineState* loaded = nullptr;
      if (SUCCEEDED(LoadLibraryPipeline(library_name, pd, &loaded))) {
        Pipeline& slot = pipelines_.emplace_back();
        slot.pso = loaded;
        slot.topology = p.topology;
        const PipelineHandle handle = PipelineHandle(pipelines_.size() - 1);
        ++stats_.pipelines;
        ++stats_.pipelines_loaded;
        pipeline_cache_[key].push_back({d, handle});
        return handle;
      }
    }
    if (async && StartPipelineWorkers()) {
      // Created by a worker thread (D3D12 devices are free-threaded); draws that need it are skipped meanwhile.
      Pipeline& slot = pipelines_.emplace_back();
      slot.topology = p.topology;
      slot.ready.store(false);
      const PipelineHandle handle = PipelineHandle(pipelines_.size() - 1);
      auto job = std::make_unique<PipelineJob>();
      job->handle = handle;
      job->target = &slot;
      job->desc = pd;
      job->library_name = library_name;
      job->priority = uint8_t(PipelinePriority(d) + (pipeline_backlog_ ? 0u : kPipelineClasses));  // 0057, 0058
      if (pipeline_backlog_) slot.backlog_class.store(job->priority);  // patch 0062
      const ShaderHandle hs[3] = {d.vs, d.ps, d.gs};
      D3D12_SHADER_BYTECODE* bc[3] = {&job->desc.VS, &job->desc.PS, &job->desc.GS};
      for (uint32_t i = 0; i < 3; ++i) {
        if (hs[i] && hs[i] < shaders_.size()) job->code[i] = shaders_[hs[i]].code;
        *bc[i] = !job->code[i] || job->code[i]->empty() ? D3D12_SHADER_BYTECODE{}
                                                        : D3D12_SHADER_BYTECODE{job->code[i]->data(), job->code[i]->size()};
      }
      {
        std::lock_guard<std::mutex> lock(job_mutex_);
        jobs_[job->priority].push_back(std::move(job));  // patch 0057: one queue per priority
      }
      job_cv_.notify_one();
      ++stats_.pipelines_async;
      pipeline_cache_[key].push_back({d, handle});
      if (pending) {
        ++stats_.pipelines_pending_draws;
        *pending = true;
      }
      return 0;
    }
    const auto create_start = std::chrono::steady_clock::now();
    const HRESULT hr = device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&p.pso));
    stats_.pipeline_create_ms +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - create_start).count();
    PipelineHandle handle = 0;
    if (SUCCEEDED(hr)) {
      Pipeline& slot = pipelines_.emplace_back();
      slot.pso = p.pso;
      slot.topology = p.topology;
      handle = PipelineHandle(pipelines_.size() - 1);
      ++stats_.pipelines;
      StorePipeline(library_name, p.pso);
    } else {
      REXLOG_ERROR("gow2_renderer: {} (vs {} ps {} gs {} topology {} rts {} depth {})",
                   Hr("CreateGraphicsPipelineState", hr), d.vs, d.ps, d.gs, uint32_t(d.topology), d.color_count,
                   uint32_t(d.depth_format));
    }
    pipeline_cache_[key].push_back({d, handle});  // failures are cached too, so they are logged once
    return handle;
  }

  PipelineHandle CreateComputePipeline(ShaderHandle cs) override {
    if (!ready_ || lost_ || !cs || cs >= shaders_.size()) return 0;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = compute_root_;
    if (!shaders_[cs].code) return 0;
    pd.CS = {shaders_[cs].code->data(), shaders_[cs].code->size()};
    ID3D12PipelineState* pso = nullptr;
    const HRESULT hr = device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
      REXLOG_ERROR("gow2_renderer: {}", Hr("CreateComputePipelineState", hr));
      return 0;
    }
    Pipeline& slot = pipelines_.emplace_back();
    slot.pso = pso;
    slot.compute = true;
    ++stats_.pipelines;
    return PipelineHandle(pipelines_.size() - 1);
  }

  // ---- guest memory ----
  void UploadGuestMemory(uint32_t guest_physical, const void* data, uint32_t size) override {
    if (!OpenList() || !size) return;  // batched texture loads: touches neither the scratch buffer nor a texture
    guest_physical &= 0x1FFFFFFFu;
    if (uint64_t(guest_physical) + size > options_.guest_memory_bytes) size = uint32_t(options_.guest_memory_bytes - guest_physical);
    uint64_t offset;
    uint8_t* dst = AllocUpload(size, 16, offset);
    if (!dst) return;
    copies_.Copy(dst, data, size);
    TransitionGuest(D3D12_RESOURCE_STATE_COPY_DEST);
    TsCat();
    list_->CopyBufferRegion(guest_, guest_physical, upload_, offset, size);
    stats_.guest_upload_bytes += size;
  }

  uint32_t UploadTransient(const void* data, uint32_t size) override {
    if (!Recording() || !size || !upload_) return UINT32_MAX;
    uint64_t offset;
    uint8_t* dst = AllocUpload(size, 16, offset);
    if (!dst || offset > 0xFFFFFFF0ull) return UINT32_MAX;
    copies_.Copy(dst, data, size);
    stats_.transient_upload_bytes += size;
    return uint32_t(offset);
  }

  // ---- graphics ----
  void SetRenderTargets(uint32_t color_count, const TextureHandle* colors, TextureHandle depth,
                        bool bits_view) override {
    // Same bindings as before: nothing to record (rt_dirty_ is also set by transitions and new lists).
    const uint32_t n = std::min<uint32_t>(color_count, kMaxColorTargets);
    if (bits_view != bound_bits_) rt_dirty_ = true;
    bound_bits_ = bits_view;
    if (n != bound_color_count_) rt_dirty_ = true;
    bound_color_count_ = n;
    for (uint32_t i = 0; i < kMaxColorTargets; ++i) {
      const TextureHandle c = i < bound_color_count_ && colors ? colors[i] : 0;
      if (bound_colors_[i] != c) rt_dirty_ = true;
      bound_colors_[i] = c;
    }
    if (bound_depth_ != depth) rt_dirty_ = true;
    bound_depth_ = depth;
    if (!rt_dirty_) ++stats_.state_calls_skipped;
  }

  void ClearDepthStencil(TextureHandle h, bool clear_depth, float depth_value, bool clear_stencil, uint8_t stencil_value,
                         const Rect* rects, uint32_t rect_count) override {
    if (!Recording() || !ValidTexture(h) || textures_[h].dsv < 0 || (!clear_depth && !clear_stencil)) return;
    Transition(textures_[h], D3D12_RESOURCE_STATE_DEPTH_WRITE);
    std::vector<D3D12_RECT> r;
    for (uint32_t i = 0; i < rect_count; ++i)
      if (!rects[i].empty()) r.push_back({rects[i].x0, rects[i].y0, rects[i].x1, rects[i].y1});
    if (rect_count && r.empty()) return;
    D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAGS(0);
    if (clear_depth) flags |= D3D12_CLEAR_FLAG_DEPTH;
    if (clear_stencil) flags |= D3D12_CLEAR_FLAG_STENCIL;
    TsCat();
    if (const uint32_t s = textures_[h].desc.samples; s > 1 && s <= 4 && have_positions_for_[s]) {
      // Patch 0089: MSAA depth locks its sample pattern on clear (Canary) - clear it under its draws' pattern.
      int8_t p[4][2];
      for (uint32_t i = 0; i < s; ++i) { p[i][0] = positions_for_[s][2 * i]; p[i][1] = positions_for_[s][2 * i + 1]; }
      SetSamplePositions(s, p);
      ApplySamplePositions();
    }
    list_->ClearDepthStencilView(dsv_heap_.Handle(textures_[h].dsv), flags, depth_value, stencil_value, UINT(r.size()),
                                 r.empty() ? nullptr : r.data());
    rt_dirty_ = true;  // the bound depth may now be in a different state record
  }

  void SetViewport(const Viewport& v) override {
    if (!(viewport_ == v)) viewport_dirty_ = true;
    viewport_ = v;
  }
  void SetScissor(const Rect& s) override {
    if (!(scissor_ == s)) scissor_dirty_ = true;
    scissor_ = s;
  }
  void SetGuestWritable(bool writable) override { guest_writable_ = writable; }
  void SetStencilRef(uint8_t ref) override {
    if (stencil_ref_ != ref) stencil_dirty_ = true;
    stencil_ref_ = ref;
  }
  // Patch 0089: wanted sample positions, recorded with the next draw / clear when they differ from the list's.
  bool SamplePositionsSupported() const override { return sample_positions_; }
  void SetSamplePositions(uint32_t count, const int8_t (*positions)[2]) override {
    if (!sample_positions_) return;
    std::array<int8_t, 8> want{};
    count = std::min<uint32_t>(count, 4);
    for (uint32_t i = 0; i < count; ++i) { want[2 * i] = positions[i][0]; want[2 * i + 1] = positions[i][1]; }
    if (count != positions_count_ || want != positions_) positions_dirty_ = true;
    positions_count_ = count;
    positions_ = want;
    if (count) {
      positions_ever_ = true;
      positions_for_[count] = want;  // what a clear of a depth target with this many samples uses
      have_positions_for_[count] = true;
    }
  }
  void ApplySamplePositions() {
    if (!positions_dirty_) return;
    D3D12_SAMPLE_POSITION p[4] = {};
    for (uint32_t i = 0; i < positions_count_; ++i) { p[i].X = positions_[2 * i]; p[i].Y = positions_[2 * i + 1]; }
    list_->SetSamplePositions(positions_count_, p);
    positions_dirty_ = false;
  }
  void SetBlendConstant(const float rgba[4]) override {
    if (std::memcmp(blend_constant_, rgba, sizeof(blend_constant_)) == 0) return;  // patch 0149: was set every draw
    std::memcpy(blend_constant_, rgba, sizeof(blend_constant_));
    blend_dirty_ = true;
  }
  void ClearColor(TextureHandle h, const float rgba[4], const Rect* rects, uint32_t rect_count) override {
    if (!Recording() || !ValidTexture(h) || textures_[h].rtv < 0) return;
    std::vector<D3D12_RECT> r;
    for (uint32_t i = 0; i < rect_count; ++i)
      if (!rects[i].empty()) r.push_back({rects[i].x0, rects[i].y0, rects[i].x1, rects[i].y1});
    if (rect_count && r.empty()) return;
    Transition(textures_[h], D3D12_RESOURCE_STATE_RENDER_TARGET);
    TsCat();
    list_->ClearRenderTargetView(rtv_heap_.Handle(textures_[h].rtv), rgba, UINT(r.size()), r.empty() ? nullptr : r.data());
    rt_dirty_ = true;
  }
  void SetPipeline(PipelineHandle p) override {
    if (pipeline_ != p) pipeline_dirty_ = true;
    pipeline_ = p;
  }

  void SetConstantsBoth(uint32_t slot, const void* data, uint32_t size) override {
    if (!Recording() || slot >= kCbCount) return;
    SetConstants(ShaderStage::kVertex, slot, data, size);
    const uint32_t vs = kRootVsCbv + slot, ps = kRootPsCbv + slot;
    if (!cbv_shared_[slot]) {  // the pixel slot follows the vertex one from now on
      cbv_shared_[slot] = true;
      cbv_pending_ &= ~(1u << ps);
      if (!((cbv_pending_ >> vs) & 1) && root_cbv_[vs]) {
        root_cbv_[ps] = root_cbv_[vs];
        root_cbv_dirty_ |= 1u << ps;
      }
    }
  }

  void SetConstants(ShaderStage stage, uint32_t slot, const void* data, uint32_t size) override {
    if (!Recording() || slot >= kCbCount) return;
    const uint32_t param = (stage == ShaderStage::kPixel ? kRootPsCbv : kRootVsCbv) + slot;
    if (stage == ShaderStage::kPixel && cbv_shared_[slot]) {  // job 177: own data again - the shadow is stale
      cbv_shared_[slot] = false;
      cbv_shadow_[param].clear();
    }
    const uint32_t padded = (std::min<uint32_t>(size, 65536) + 255) & ~255u;
    // Same data as the slot already holds: nothing to upload (the draw keeps using the previous copy).
    if (cbv_shadow_[param].size() == padded && root_cbv_[param] && !((cbv_pending_ >> param) & 1) &&
        std::memcmp(cbv_shadow_[param].data(), data, std::min<uint32_t>(size, padded)) == 0) {
      ++stats_.constant_uploads_skipped;
      return;
    }
    // Job 172: one pass (was a zero fill of the whole slot, then the copy): the data, then zeros up to the padding.
    std::vector<uint8_t>& shadow = cbv_shadow_[param];
    const uint32_t n = std::min<uint32_t>(size, padded);
    if (shadow.size() != padded) shadow.resize(padded);
    std::memcpy(shadow.data(), data, n);
    if (padded > n) std::memset(shadow.data() + n, 0, padded - n);
    cbv_pending_ |= 1u << param;  // copied into the upload ring by the next draw
  }

  // Compacted float constants: the same compare-then-copy as SetConstants, over two source pieces (no gather copy).
  void SetConstantsSplit(ShaderStage stage, uint32_t slot, const void* a, uint32_t a_size, const void* b,
                         uint32_t b_size) override {
    if (!Recording() || slot >= kCbCount) return;
    if (uint64_t(a_size) + b_size > 65536) {  // not a shape we make; keep the plain path's behaviour
      Backend::SetConstantsSplit(stage, slot, a, a_size, b, b_size);
      return;
    }
    const uint32_t param = (stage == ShaderStage::kPixel ? kRootPsCbv : kRootVsCbv) + slot;
    if (stage == ShaderStage::kPixel && cbv_shared_[slot]) {
      cbv_shared_[slot] = false;
      cbv_shadow_[param].clear();
    }
    const uint32_t size = a_size + b_size, padded = (size + 255) & ~255u;
    std::vector<uint8_t>& shadow = cbv_shadow_[param];
    if (shadow.size() == padded && root_cbv_[param] && !((cbv_pending_ >> param) & 1) &&
        (!a_size || std::memcmp(shadow.data(), a, a_size) == 0) &&
        (!b_size || std::memcmp(shadow.data() + a_size, b, b_size) == 0)) {
      ++stats_.constant_uploads_skipped;
      return;
    }
    if (shadow.size() != padded) shadow.resize(padded);
    if (a_size) std::memcpy(shadow.data(), a, a_size);
    if (b_size) std::memcpy(shadow.data() + a_size, b, b_size);
    if (padded > size) std::memset(shadow.data() + size, 0, padded - size);
    cbv_pending_ |= 1u << param;
  }

  void Draw(uint32_t vertex_count) override {
    if (!vertex_count || !PrepareDraw()) return;
    TsCat();
    list_->DrawInstanced(vertex_count, 1, 0, 0);
    ++stats_.draws;
  }

  void DrawIndexed(const uint32_t* indices, uint32_t index_count) override { DrawIndexedKeep(indices, index_count); }

  bool DrawIndexedAt(uint64_t offset, uint32_t index_count, uint64_t list) override {
    if (!index_count || stats_.submits != list || !PrepareDraw() || stats_.submits != list) return false;
    D3D12_INDEX_BUFFER_VIEW ib{upload_gpu_ + offset, index_count * 4, DXGI_FORMAT_R32_UINT};
    list_->IASetIndexBuffer(&ib);
    TsCat();
    list_->DrawIndexedInstanced(index_count, 1, 0, 0, 0);
    ++stats_.draws;
    ++stats_.index_draws_reused;
    return true;
  }

  uint64_t DrawIndexedKeep(const uint32_t* indices, uint32_t index_count) override {
    if (!index_count) return UINT64_MAX;
    uint64_t offset = 0;
    for (int guard = 0;; ++guard) {
      if (guard == 4 || !PrepareDraw()) return UINT64_MAX;
      const uint64_t submits = stats_.submits;
      uint8_t* dst = AllocUpload(uint64_t(index_count) * 4, 16, offset);
      if (!dst) return UINT64_MAX;
      if (stats_.submits != submits) continue;  // a new list: state must be applied again first
      std::memcpy(dst, indices, size_t(index_count) * 4);
      break;
    }
    D3D12_INDEX_BUFFER_VIEW ib{upload_gpu_ + offset, index_count * 4, DXGI_FORMAT_R32_UINT};
    list_->IASetIndexBuffer(&ib);
    TsCat();
    list_->DrawIndexedInstanced(index_count, 1, 0, 0, 0);
    ++stats_.draws;
    return offset;
  }

  void NextFrame() override {
    if (!ready_ || lost_) return;
    if (list_open_) Submit();
    cur_pool_ = (cur_pool_ + 1) % kSamplerPools;
    if (Pool().fence > CompletedFence()) {
      ++stats_.sampler_table_waits;
      WaitAt(Pool().fence, 2);
    }
    for (SamplerTable& t : Pool().tables) t.in_open_list = false;
    textures_dirty_ = true;  // the bound tables are in the previous heap
  }

  BufferHandle CreateIndexBuffer(const uint32_t* indices, uint32_t index_count) override {
    if (!ready_ || lost_ || !indices || !index_count) return 0;
    const UINT64 bytes = UINT64(index_count) * 4;
    ID3D12Resource* buffer = nullptr;
    std::string error;
    // An upload-heap buffer: written once from the CPU, read by our GPU in place (static data, no copy pass).
    if (!CreateBuffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ,
                      &buffer, error)) {
      REXLOG_ERROR("gow2_renderer: index buffer: {}", error);
      return 0;
    }
    void* mapped = nullptr;
    const D3D12_RANGE none{0, 0};
    if (FAILED(buffer->Map(0, &none, &mapped))) {
      SafeRelease(buffer);
      return 0;
    }
    std::memcpy(mapped, indices, size_t(bytes));
    buffer->Unmap(0, nullptr);
    // Patch 0150: a GPU-memory copy, filled from that upload buffer in the open list (the upload buffer is freed once
    // the list's fence passes). Buffers start in COMMON: the copy promotes it to COPY_DEST, the barrier makes it an
    // index buffer for the draws after it in this list, and it decays to COMMON at the list's end (later lists promote
    // it to INDEX_BUFFER implicitly). Any failure keeps the upload-heap buffer, as before.
    if (options_.index_buffers_in_vram && Recording()) {
      ID3D12Resource* vram = nullptr;
      std::string vram_error;
      if (CreateBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, &vram,
                       vram_error)) {
        list_->CopyBufferRegion(vram, 0, buffer, 0, bytes);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = vram;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_INDEX_BUFFER;
        list_->ResourceBarrier(1, &b);
        deferred_releases_.push_back({buffer, next_fence_});  // the copy above reads it
        buffer = vram;
        if (!index_vram_logged_) {
          index_vram_logged_ = true;
          REXLOG_INFO("gow2_renderer: index buffers in GPU memory (patch 0150): first one {} KB", bytes / 1024);
        }
      } else if (!index_vram_logged_) {
        index_vram_logged_ = true;
        REXLOG_ERROR("gow2_renderer: index buffer in GPU memory: {} - upload heap kept", vram_error);
      }
    }
    uint32_t h;
    if (!free_index_buffers_.empty()) {
      h = free_index_buffers_.back();
      free_index_buffers_.pop_back();
    } else {
      if (index_buffers_.empty()) index_buffers_.emplace_back();  // handle 0 = none
      h = uint32_t(index_buffers_.size());
      index_buffers_.emplace_back();
    }
    index_buffers_[h] = {buffer, index_count};
    ++stats_.index_buffers;
    stats_.index_buffer_bytes += bytes;
    return h;
  }

  void ReleaseIndexBuffer(BufferHandle h) override {
    if (!h || h >= index_buffers_.size() || !index_buffers_[h].resource) return;
    IndexBuffer& b = index_buffers_[h];
    deferred_releases_.push_back({b.resource, next_fence_});  // the open list may still draw from it
    --stats_.index_buffers;
    stats_.index_buffer_bytes -= UINT64(b.count) * 4;
    b = {};
    free_index_buffers_.push_back(h);
  }

  void DrawIndexedBuffer(BufferHandle h, uint32_t index_count) override {
    if (!index_count || !h || h >= index_buffers_.size() || !index_buffers_[h].resource) return;
    if (!PrepareDraw()) return;
    const IndexBuffer& b = index_buffers_[h];
    index_count = std::min(index_count, b.count);
    D3D12_INDEX_BUFFER_VIEW ib{b.resource->GetGPUVirtualAddress(), b.count * 4, DXGI_FORMAT_R32_UINT};
    list_->IASetIndexBuffer(&ib);
    TsCat();
    list_->DrawIndexedInstanced(index_count, 1, 0, 0, 0);
    ++stats_.draws;
    ++stats_.index_buffer_draws;
  }

  void SetTextures(const TextureView* views96, const SamplerDesc* samplers32, uint32_t view_count,
                   uint32_t sampler_count) override {
    view_count = std::clamp<uint32_t>(view_count, 1u, kTextureSrvCount);
    sampler_count = std::clamp<uint32_t>(sampler_count, 1u, kSamplerCount);
    // Batched texture loads: the queued copies first - the table reuse below reads the textures' states.
    if (!scratch_copies_.empty()) FlushScratchCopies();
    if (!views96 && !samplers32) {
      if (textures_custom_) {
        textures_custom_ = false;
        textures_dirty_ = true;
      }
      return;
    }
    // The same views and samplers as the table already bound in this list: keep it, unless a bound texture left the
    // shader-resource state since (it was rendered to or uploaded) - the table rebuild records the transition.
    bool same = textures_custom_ && !textures_dirty_ && view_count <= bound_view_count_ &&
                sampler_count <= bound_sampler_count_;
    for (uint32_t i = 0; i < view_count && same; ++i) same = bound_views_[i] == (views96 ? views96[i] : TextureView{});
    for (uint32_t i = 0; i < sampler_count && same; ++i)
      same = bound_samplers_[i] == (samplers32 ? samplers32[i] : SamplerDesc{});
    if (same) {
      const D3D12_RESOURCE_STATES kSrv =
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      for (uint32_t i = 0; i < view_count && same; ++i)
        if (ValidTexture(bound_views_[i].texture)) same = textures_[bound_views_[i].texture].state == kSrv;
    }
    if (same) {
      ++stats_.texture_tables_reused;
      return;
    }
    for (uint32_t i = 0; i < 96; ++i) bound_views_[i] = views96 ? views96[i] : TextureView{};
    for (uint32_t i = 0; i < 32; ++i) bound_samplers_[i] = samplers32 ? samplers32[i] : SamplerDesc{};
    bound_view_count_ = view_count;
    bound_sampler_count_ = sampler_count;
    textures_custom_ = true;
    textures_dirty_ = true;
  }

  void UploadTexture(TextureHandle h, uint32_t mip, uint32_t layer, const void* data, uint32_t row_bytes, uint32_t rows,
                     uint32_t slices) override {
    if (!Recording() || !ValidTexture(h) || !data) return;
    Texture& t = textures_[h];
    D3D12_RESOURCE_DESC rd = t.resource->GetDesc();
    // Patch 0024: never a copy into a subresource the texture doesn't have (a device-removal risk).
    const UINT layers_have = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : rd.DepthOrArraySize;
    if (mip >= rd.MipLevels || layer >= layers_have) {
      ++stats_.texture_uploads_refused;
      return;
    }
    const UINT sub = mip + layer * rd.MipLevels;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT num_rows = 0;
    UINT64 row_size = 0, total = 0;
    device_->GetCopyableFootprints(&rd, sub, 1, 0, &fp, &num_rows, &row_size, &total);
    uint64_t offset;
    uint8_t* dst = AllocUpload(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, offset);
    if (!dst) return;
    const auto* src = static_cast<const uint8_t*>(data);
    const UINT depth = std::min<UINT>(fp.Footprint.Depth, std::max<uint32_t>(slices, 1));
    const UINT copy_rows = std::min<UINT>(num_rows, rows);
    const size_t copy_bytes = size_t(std::min<UINT64>(row_size, row_bytes));
    for (UINT z = 0; z < depth; ++z)
      for (UINT y = 0; y < copy_rows; ++y)
        std::memcpy(dst + (size_t(z) * num_rows + y) * fp.Footprint.RowPitch,
                    src + (size_t(z) * rows + y) * row_bytes, copy_bytes);
    fp.Offset = offset;
    Transition(t, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION d{};
    d.pResource = t.resource;
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d.SubresourceIndex = sub;
    D3D12_TEXTURE_COPY_LOCATION sl{};
    sl.pResource = upload_;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sl.PlacedFootprint = fp;
    TsCat();
    list_->CopyTextureRegion(&d, 0, 0, 0, &sl, nullptr);
    stats_.texture_upload_bytes += total;
  }

  // ---- compute ----
  void Dispatch(PipelineHandle p, const ComputeBindings& b, uint32_t gx, uint32_t gy, uint32_t gz) override {
    // Batched texture loads: a texture load's dispatch keeps the queued copies queued - it reads the guest-memory
    // mirror and writes its own scratch region or its own (not queued) texture.
    const bool scratch_batch = options_.batch_texture_loads && b.scratch_u0;
    const bool load_batch = scratch_batch || (options_.batch_texture_loads && b.defer_uav_barrier);
    if (!(load_batch ? OpenList() : Recording()) || !p || p >= pipelines_.size() || !pipelines_[p].compute || !gx ||
        !gy || !gz)
      return;
    const uint32_t base = AllocShaderDescriptors(kComputeSrvCount + 2);
    if (base == UINT32_MAX) return;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = shader_heap_->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < kComputeSrvCount; ++i) {
      D3D12_CPU_DESCRIPTOR_HANDLE dst{cpu.ptr + SIZE_T(base + i) * view_increment_};
      if (i == 3 && b.guest_t3) {
        // The guest-memory raw buffer (its descriptor is slot 96 of the default table).
        TransitionGuest(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        device_->CopyDescriptorsSimple(1, dst, default_table_heap_.Handle(96), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        continue;
      }
      const TextureHandle h = b.srv[i];
      int32_t slot = -1;
      if (ValidTexture(h)) {
        slot = textures_[h].srv[b.srv_plane[i] ? 1 : 0];
        Transition(textures_[h], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      }
      if (slot >= 0) {
        device_->CopyDescriptorsSimple(1, dst, view_heap_.Handle(slot), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      } else {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R32_FLOAT;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(nullptr, &v, dst);
      }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE udst{cpu.ptr + SIZE_T(base + kComputeSrvCount) * view_increment_};
    const int32_t uav_slot = !b.guest_u0 && !b.scratch_u0 && ValidTexture(b.uav_texture)
                                 ? UavSlot(textures_[b.uav_texture], b.uav_mip)
                                 : -1;
    if (b.scratch_u0 && scratch_uav_ >= 0 && scratch_) {  // patch 0021
      TransitionScratch(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      if (scratch_batch) {  // a raw view starting at this subresource's region (the shader addresses from 0)
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = DXGI_FORMAT_R32_TYPELESS;
        u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        u.Buffer.FirstElement = scratch_fp_.Offset / 4;
        u.Buffer.NumElements = UINT((scratch_size_ - scratch_fp_.Offset) / 4);
        u.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device_->CreateUnorderedAccessView(scratch_, nullptr, &u, udst);
      } else {
        device_->CopyDescriptorsSimple(1, udst, view_heap_.Handle(scratch_uav_), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      }
    } else if (b.guest_u0 && guest_uav_ >= 0) {
      TransitionGuest(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      device_->CopyDescriptorsSimple(1, udst, view_heap_.Handle(guest_uav_), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    } else if (uav_slot >= 0) {
      UavBarrierIfWritten(textures_[b.uav_texture], b.uav_mip);
      Transition(textures_[b.uav_texture], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      device_->CopyDescriptorsSimple(1, udst, view_heap_.Handle(uav_slot), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    } else {
      D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
      u.Format = DXGI_FORMAT_R32_UINT;
      u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      device_->CreateUnorderedAccessView(nullptr, nullptr, &u, udst);
    }
    // u1 (job 047 resolve-through): a second texture the dispatch writes, or a null view.
    D3D12_CPU_DESCRIPTOR_HANDLE udst2{cpu.ptr + SIZE_T(base + kComputeSrvCount + 1) * view_increment_};
    const int32_t uav2_slot = ValidTexture(b.uav2_texture) ? UavSlot(textures_[b.uav2_texture], 0) : -1;
    if (uav2_slot >= 0) {
      UavBarrierIfWritten(textures_[b.uav2_texture], 0);
      Transition(textures_[b.uav2_texture], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      device_->CopyDescriptorsSimple(1, udst2, view_heap_.Handle(uav2_slot), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    } else {
      D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
      u.Format = DXGI_FORMAT_R32_FLOAT;
      u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      device_->CreateUnorderedAccessView(nullptr, nullptr, &u, udst2);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = shader_heap_->GetGPUDescriptorHandleForHeapStart();
    list_->SetComputeRootSignature(compute_root_);
    list_->SetPipelineState(pipelines_[p].pso);
    pipeline_dirty_ = true;  // the graphics pipeline must be re-set before the next draw
    if (b.constant_count)
      list_->SetComputeRoot32BitConstants(kRootCsConstants, std::min<uint32_t>(b.constant_count, 16), b.constants, 0);
    list_->SetComputeRootDescriptorTable(kRootCsSrv, {gpu.ptr + UINT64(base) * view_increment_});
    list_->SetComputeRootDescriptorTable(kRootCsUav, {gpu.ptr + UINT64(base + kComputeSrvCount) * view_increment_});
    TsCat();
    list_->Dispatch(gx, gy, gz);
    if (b.scratch_u0 && scratch_) {  // patch 0021: CopyScratchToTexture transitions it next
    } else if (b.guest_u0) {
      D3D12_RESOURCE_BARRIER u{};
      u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      u.UAV.pResource = guest_;
      list_->ResourceBarrier(1, &u);
    } else if (ValidTexture(b.uav_texture) && options_.batch_texture_loads && b.defer_uav_barrier) {
      textures_[b.uav_texture].uav_unbarriered |= 1ull << std::min<uint32_t>(b.uav_mip, 63);
      ++stats_.uav_barriers_deferred;
    } else if (ValidTexture(b.uav_texture)) {
      D3D12_RESOURCE_BARRIER u{};
      u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      u.UAV.pResource = textures_[b.uav_texture].resource;
      list_->ResourceBarrier(1, &u);
    }
    if (uav2_slot >= 0) {
      D3D12_RESOURCE_BARRIER u{};
      u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      u.UAV.pResource = textures_[b.uav2_texture].resource;
      list_->ResourceBarrier(1, &u);
    }
    rt_dirty_ = true;  // a bound render target may have been transitioned for reading
    ++stats_.dispatches;
  }

  // ---- occlusion queries (Stage 4, upl-s4c) ----
  OcclusionHandle BeginOcclusionQuery() override {
    if (occ_open_ || !EnsureOcclusion() || !Recording()) {
      ++stats_.occlusion_refused;
      return 0;
    }
    const int64_t slot = AcquireOcclusionSlot();
    if (slot < 0) {
      ++stats_.occlusion_refused;
      return 0;
    }
    OcclusionHandle h;
    if (!occ_free_handles_.empty()) {
      h = occ_free_handles_.back();
      occ_free_handles_.pop_back();
    } else {
      occ_queries_.emplace_back();
      h = OcclusionHandle(occ_queries_.size() - 1);
    }
    OcclusionQuery& q = occ_queries_[h];
    q = OcclusionQuery{};
    q.in_use = true;
    q.slot[q.parts++] = uint32_t(slot);
    list_->BeginQuery(occ_heap_, D3D12_QUERY_TYPE_OCCLUSION, UINT(slot));
    occ_open_ = h;
    occ_open_in_list_ = true;
    ++stats_.occlusion_queries;
    return h;
  }
  void EndOcclusionQuery(OcclusionHandle h) override {
    if (!h || h != occ_open_) return;
    if (occ_open_in_list_ && list_open_) CloseOcclusionPart();
    occ_open_in_list_ = false;
    occ_queries_[h].ended = true;
    occ_open_ = 0;
  }
  bool GetOcclusionResult(OcclusionHandle h, uint64_t* samples) override {
    if (!h || h >= occ_queries_.size() || !occ_mapped_) return false;
    const OcclusionQuery& q = occ_queries_[h];
    if (!q.in_use || !q.ended || q.broken) return false;
    const uint64_t done = CompletedFence();
    uint64_t sum = 0;
    for (uint32_t i = 0; i < q.parts; ++i) {
      const uint64_t f = occ_slot_fence_[q.slot[i]];
      if (!f || f > done) return false;
      sum += occ_mapped_[q.slot[i]];
    }
    if (samples) *samples = sum;
    return true;
  }
  void ReleaseOcclusionQuery(OcclusionHandle h) override {
    if (!h || h >= occ_queries_.size() || !occ_queries_[h].in_use) return;
    if (h == occ_open_) EndOcclusionQuery(h);
    OcclusionQuery& q = occ_queries_[h];
    for (uint32_t i = 0; i < q.parts; ++i) {
      const uint32_t s = q.slot[i];
      // A slot still in the open list is free only after that list's submission (next_fence_).
      if (!occ_slot_fence_[s]) occ_slot_fence_[s] = next_fence_;
      occ_slot_held_[s] = 0;
    }
    q.in_use = false;
    occ_free_handles_.push_back(h);
  }

  void* OcclusionCompletionEvent(OcclusionHandle h) override {
    if (!h || h >= occ_queries_.size() || lost_) return nullptr;
    const OcclusionQuery& q = occ_queries_[h];
    if (!q.in_use || !q.ended || q.broken || !q.parts) return nullptr;
    uint64_t value = 0;
    for (uint32_t i = 0; i < q.parts; ++i) {
      const uint64_t f = occ_slot_fence_[q.slot[i]];
      if (!f) return nullptr;
      value = std::max(value, f);
    }
    if (!occ_event_) occ_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!occ_event_) return nullptr;
    if (value != occ_event_fence_) {
      if (FAILED(fence_->SetEventOnCompletion(value, occ_event_))) return nullptr;
      occ_event_fence_ = value;
    }
    return occ_event_;
  }

  // ---- readback ----
  ReadbackHandle ReadbackTexture(TextureHandle h, const Rect& region) override {
    if (!Recording() || !ValidTexture(h) || region.empty()) return 0;
    Texture& t = textures_[h];
    if (IsDepthFormat(t.desc.format) || t.desc.samples > 1) return 0;
    const uint32_t w = uint32_t(region.x1 - region.x0), hgt = uint32_t(region.y1 - region.y0);
    const uint32_t row = (w * BytesPerTexel(t.desc.format) + 255) & ~255u;
    const uint64_t size = uint64_t(row) * hgt;
    ReadbackHandle rh = 0;
    for (size_t i = 1; i < readbacks_.size(); ++i) {
      if (!readbacks_[i].in_use && readbacks_[i].size >= size && (!rh || readbacks_[i].size < readbacks_[rh].size))
        rh = ReadbackHandle(i);
    }
    if (!rh) {
      Readback r;
      std::string error;
      if (!CreateBuffer(std::max<uint64_t>(size, 65536), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_COPY_DEST, &r.buffer, error)) {
        REXLOG_ERROR("gow2_renderer: readback buffer: {}", error);
        return 0;
      }
      r.size = std::max<uint64_t>(size, 65536);
      if (FAILED(r.buffer->Map(0, nullptr, reinterpret_cast<void**>(&r.mapped)))) {
        r.buffer->Release();
        return 0;
      }
      readbacks_.push_back(r);
      rh = ReadbackHandle(readbacks_.size() - 1);
    }
    Readback& r = readbacks_[rh];
    r.in_use = true;
    r.row_pitch = row;
    r.fence = 0;
    Transition(t, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = r.buffer;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint = {ResourceFormat(t.desc.format), w, hgt, 1, row};
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = t.resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_BOX box{UINT(region.x0), UINT(region.y0), 0, UINT(region.x1), UINT(region.y1), 1};
    TsCat();
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    pending_readbacks_.push_back(rh);
    rt_dirty_ = true;
    return rh;
  }

  // ---- patch 0021: scratch buffer for GPU loads into BCn textures (Canary loads every texture on the GPU) ----
  bool PrepareScratchFor(TextureHandle h, uint32_t mip, uint32_t layer, uint32_t& row_pitch, uint32_t& rows) override {
    row_pitch = rows = 0;
    const bool batch = options_.batch_texture_loads;
    if (!(batch ? OpenList() : Recording()) || !ValidTexture(h)) return false;
    Texture& t = textures_[h];
    D3D12_RESOURCE_DESC rd = t.resource->GetDesc();
    if (mip >= rd.MipLevels || layer >= rd.DepthOrArraySize) return false;
    const UINT sub = mip + layer * rd.MipLevels;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT num_rows = 0;
    UINT64 row_size = 0, total = 0;
    device_->GetCopyableFootprints(&rd, sub, 1, 0, &fp, &num_rows, &row_size, &total);
    if (!total || fp.Footprint.Depth != 1) return false;
    // Batched texture loads: this subresource's region follows the queued ones; when it doesn't fit, they are copied
    // out first and the buffer starts over (the next load's transition to UAV waits for those copies).
    UINT64 at = 0;
    if (batch) {
      at = (scratch_head_ + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
      if (scratch_ && at + total > scratch_size_) {
        FlushScratchCopies();
        at = 0;
      }
    }
    if (total > scratch_size_) {
      if (scratch_) deferred_releases_.push_back({scratch_, next_fence_});  // the open list may still use it
      scratch_ = nullptr;
      scratch_size_ = 0;
      // Batched: room for a burst of loads (a level start loads hundreds of BC textures in one frame).
      const UINT64 size = std::max<UINT64>((total + 0xFFFFF) & ~UINT64(0xFFFFF), batch ? 32ull << 20 : 4ull << 20);
      std::string error;
      if (!CreateBuffer(size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COMMON, &scratch_, error)) {
        REXLOG_ERROR("gow2_renderer: texture scratch buffer ({} MB): {}", size >> 20, error);
        scratch_ = nullptr;
        return false;
      }
      scratch_size_ = size;
      scratch_state_ = D3D12_RESOURCE_STATE_COMMON;
      if (scratch_uav_ < 0) scratch_uav_ = view_heap_.Alloc();
      if (scratch_uav_ < 0) return false;
      D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
      u.Format = DXGI_FORMAT_R32_TYPELESS;
      u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      u.Buffer.NumElements = UINT(size / 4);
      u.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
      device_->CreateUnorderedAccessView(scratch_, nullptr, &u, view_heap_.Handle(scratch_uav_));
    }
    scratch_fp_ = fp;
    scratch_fp_.Offset = at;
    scratch_sub_ = sub;
    scratch_head_ = at + total;
    row_pitch = fp.Footprint.RowPitch;
    rows = num_rows;
    return true;
  }
  void CopyScratchToTexture(TextureHandle h, uint32_t mip, uint32_t layer) override {
    const bool batch = options_.batch_texture_loads;
    if (!(batch ? OpenList() : Recording()) || !ValidTexture(h) || !scratch_) return;
    Texture& t = textures_[h];
    D3D12_RESOURCE_DESC rd = t.resource->GetDesc();
    const UINT sub = mip + layer * rd.MipLevels;
    if (sub != scratch_sub_) return;  // PrepareScratchFor was for another subresource
    if (batch) {  // queued: recorded with the others by FlushScratchCopies
      scratch_copies_.push_back({h, t.serial, sub, scratch_fp_});
      t.scratch_pending = true;
      return;
    }
    TransitionScratch(D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(t, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{};
    dl.pResource = t.resource;
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dl.SubresourceIndex = sub;
    D3D12_TEXTURE_COPY_LOCATION sl{};
    sl.pResource = scratch_;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sl.PlacedFootprint = scratch_fp_;
    TsCat();
    list_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    ++stats_.scratch_texture_copies;
  }
  // Batched texture loads: records the queued scratch copies - the scratch buffer's one transition, every target's
  // transition (consecutive barriers: the proxy sends them as one batch), then the copies.
  void FlushScratchCopies() {
    if (scratch_copies_.empty()) return;
    std::vector<ScratchCopy> copies;
    copies.swap(scratch_copies_);
    scratch_head_ = 0;
    for (const ScratchCopy& c : copies)  // first: Transition below must not flush again
      if (ValidTexture(c.texture) && textures_[c.texture].serial == c.serial) textures_[c.texture].scratch_pending = false;
    if (list_open_ && !lost_ && scratch_) {
      const uint8_t category = gpu_cat_pending_;  // job 177: timed as texture loads, whatever comes next
      gpu_cat_pending_ = 3;
      TransitionScratch(D3D12_RESOURCE_STATE_COPY_SOURCE);
      for (const ScratchCopy& c : copies)
        if (ValidTexture(c.texture) && textures_[c.texture].serial == c.serial)
          Transition(textures_[c.texture], D3D12_RESOURCE_STATE_COPY_DEST);
      for (const ScratchCopy& c : copies) {
        if (!ValidTexture(c.texture) || textures_[c.texture].serial != c.serial) continue;
        D3D12_TEXTURE_COPY_LOCATION dl{};
        dl.pResource = textures_[c.texture].resource;
        dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dl.SubresourceIndex = c.subresource;
        D3D12_TEXTURE_COPY_LOCATION sl{};
        sl.pResource = scratch_;
        sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sl.PlacedFootprint = c.footprint;
        TsCat();
        list_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
        ++stats_.scratch_texture_copies;
      }
      ++stats_.scratch_batches;
      gpu_cat_pending_ = category;
    }
    copies.clear();
    if (scratch_copies_.empty()) scratch_copies_.swap(copies);  // keep the capacity
  }
  // Batched texture loads: a mip written as a UAV with its barrier left out gets it before it is written again.
  void UavBarrierIfWritten(Texture& t, uint32_t mip) {
    if (!(t.uav_unbarriered & (1ull << std::min<uint32_t>(mip, 63)))) return;
    D3D12_RESOURCE_BARRIER u{};
    u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    u.UAV.pResource = t.resource;
    list_->ResourceBarrier(1, &u);
    t.uav_unbarriered = 0;  // a UAV barrier covers the whole resource
  }
  void TransitionScratch(D3D12_RESOURCE_STATES state) {
    if (!scratch_ || scratch_state_ == state) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = scratch_;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = scratch_state_;
    b.Transition.StateAfter = state;
    list_->ResourceBarrier(1, &b);
    scratch_state_ = state;
  }

  ReadbackHandle ReadbackGuest(uint32_t address, uint32_t size) override {
    if (!Recording() || !guest_ || !size || uint64_t(address) + size > options_.guest_memory_bytes) return 0;
    ReadbackHandle rh = 0;
    for (size_t i = 1; i < readbacks_.size(); ++i) {
      if (!readbacks_[i].in_use && readbacks_[i].size >= size && (!rh || readbacks_[i].size < readbacks_[rh].size))
        rh = ReadbackHandle(i);
    }
    if (!rh) {
      Readback r;
      std::string error;
      if (!CreateBuffer(std::max<uint64_t>(size, 65536), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_COPY_DEST, &r.buffer, error)) {
        REXLOG_ERROR("gow2_renderer: guest readback buffer: {}", error);
        return 0;
      }
      r.size = std::max<uint64_t>(size, 65536);
      if (FAILED(r.buffer->Map(0, nullptr, reinterpret_cast<void**>(&r.mapped)))) {
        r.buffer->Release();
        return 0;
      }
      readbacks_.push_back(r);
      rh = ReadbackHandle(readbacks_.size() - 1);
    }
    Readback& r = readbacks_[rh];
    r.in_use = true;
    r.row_pitch = size;
    r.fence = 0;
    TransitionGuest(D3D12_RESOURCE_STATE_COPY_SOURCE);
    TsCat();
    list_->CopyBufferRegion(r.buffer, 0, guest_, address, size);
    pending_readbacks_.push_back(rh);
    return rh;
  }

  bool IsReadbackReady(ReadbackHandle h) override {
    if (!h || h >= readbacks_.size() || !readbacks_[h].in_use || !readbacks_[h].fence) return false;
    return CompletedFence() >= readbacks_[h].fence;
  }
  const uint8_t* ReadbackData(ReadbackHandle h, uint32_t& row_pitch) override {
    if (!IsReadbackReady(h)) return nullptr;
    row_pitch = readbacks_[h].row_pitch;
    return readbacks_[h].mapped;
  }
  void ReleaseReadback(ReadbackHandle h) override {
    if (h && h < readbacks_.size()) {
      // A readback released before its copy completed must not be reused until then.
      if (readbacks_[h].fence && CompletedFence() < readbacks_[h].fence) WaitAt(readbacks_[h].fence, 1);
      readbacks_[h].in_use = false;
      readbacks_[h].fence = 0;
    }
  }

  // ---- submission ----
  void FlushCopies() override { copies_.Flush(); }

  uint64_t Submit() override { return SubmitImpl(true); }
  uint64_t SubmitEarly() override {  // patch 0138
    ++stats_.early_submits;
    return SubmitImpl(false);
  }
  uint64_t SubmitImpl(bool drain) {
    stats_.barrier_calls = proxy_.barrier_calls;  // job 045
    stats_.barriers = proxy_.barriers;
    stats_.barrier_batches = proxy_.barrier_batches;  // job 046
    stats_.barriers_merged = proxy_.barriers_merged;
    copies_.Flush();  // job 171: the GPU reads these ring bytes once the list runs
    if (!ready_ || lost_) return last_submitted_;
    stats_.pipelines_loaded_async = loaded_async_.load(std::memory_order_relaxed);  // patch 0010
    stats_.pipelines_loaded += stats_.pipelines_loaded_async - loaded_async_counted_;
    loaded_async_counted_ = stats_.pipelines_loaded_async;
    stats_.pipeline_library_saves += background_saves_.exchange(0, std::memory_order_relaxed);
    RefreshWorkerStats();  // patch 0057 / 0061
    if (pipeline_library_dirty_.load(std::memory_order_relaxed)) {  // saved pipelines: at most every ~15 s (0056)
      const auto now = std::chrono::steady_clock::now();
      if (now - pipeline_library_saved_at_ > std::chrono::seconds(std::max(5, options_.pipeline_save_seconds))) {
        if (!options_.pipeline_save_background) {
          SavePipelineLibrary();
        } else if (!saving_.load(std::memory_order_acquire)) {  // patch 0010: off the recording thread
          pipeline_library_saved_at_ = now;
          if (saver_.joinable()) saver_.join();
          saving_.store(true, std::memory_order_release);
          saver_ = std::thread([this] {
            if (SavePipelineLibrary(false)) background_saves_.fetch_add(1, std::memory_order_relaxed);
            saving_.store(false, std::memory_order_release);
          });
        }
      }
    }
    if (!list_open_) return last_submitted_;
    FlushScratchCopies();  // batched texture loads: they belong to this list
    if (occ_open_ && occ_open_in_list_) {  // upl-s4c: a query spanning this Submit continues in the next list
      CloseOcclusionPart();
      occ_open_in_list_ = false;
    }
    queries::ResolveEndedRuns(occ_slots_in_list_, [this](uint32_t first, uint32_t count) {
      list_->ResolveQueryData(occ_heap_, D3D12_QUERY_TYPE_OCCLUSION, first, count, occ_readback_, UINT64(first) * 8);
    });
    if (ts_open_ && ts_cur_.count) {
      TsEmit(0xFE);  // end mark
      const uint32_t first = ts_cur_.first, n = ts_cur_.count;
      const uint32_t a = std::min(n, kTsCount - first);
      list_->ResolveQueryData(ts_heap_, D3D12_QUERY_TYPE_TIMESTAMP, first, a, ts_readback_, UINT64(first) * 8);
      if (n > a) list_->ResolveQueryData(ts_heap_, D3D12_QUERY_TYPE_TIMESTAMP, 0, n - a, ts_readback_, 0);
    }
    HRESULT hr = list_->Close();
    list_open_ = false;
    if (FAILED(hr)) {
      lost_ = true;
      lost_reason_ = Hr("ID3D12GraphicsCommandList::Close", hr);
      REXLOG_ERROR("gow2_renderer: {}", lost_reason_);
      DrainInfoQueue();
      return last_submitted_;
    }
    const uint64_t value = next_fence_++;
    if (ts_open_ && ts_cur_.count >= 2) {
      ts_cur_.fence = value;
      ts_pending_.push_back(std::move(ts_cur_));
    } else {
      ts_used_ -= ts_cur_.count;  // nothing to read back (no GPU work in this list)
    }
    ts_cur_ = TsList{};
    ts_open_ = false;
    list_->ExecuteAndSignal(fence_, value);
    // Threaded recording: wait until the recording thread has submitted it (presentation and fence waits rely on our
    // work being in the queue when Submit returns), then pick up any failure it saw. Patch 0138: an early (mid-frame)
    // submit does not wait - the recording thread submits lists in order, and the frame's last Submit drains.
    if (drain) list_->Drain(); else list_->Kick();
    stats_.recording_wait_ms = proxy_.recording_wait_ms;
    if (FAILED(list_->Error()) && !lost_) {
      lost_ = true;
      lost_reason_ = Hr(list_->ErrorWhat() ? list_->ErrorWhat() : "command recording", list_->Error());
      REXLOG_ERROR("gow2_renderer: {}", lost_reason_);
    }
    last_submitted_ = value;
    allocators_.push_back({current_allocator_, value});
    current_allocator_ = nullptr;
    upload_markers_.push_back({value, upload_head_});
    ring_markers_.push_back({value, ring_head_});
    for (ReadbackHandle r : pending_readbacks_) readbacks_[r].fence = value;
    for (uint32_t s : occ_slots_in_list_) occ_slot_fence_[s] = value;  // upl-s4c
    occ_slots_in_list_.clear();
    for (const auto& [p, t] : tables_in_open_list_) {
      sampler_pools_[p].tables[t].fence = value;
      sampler_pools_[p].tables[t].in_open_list = false;
    }
    for (SamplerPool& pool : sampler_pools_)  // patch 0034: every heap bound in this list
      if (pool.in_list) {
        pool.fence = value;
        pool.in_list = false;
      }
    Pool().fence = value;
    tables_in_open_list_.clear();
    pending_readbacks_.clear();
    ++stats_.submits;
    DrainInfoQueue();
    Retire();
    return value;
  }

  uint64_t CompletedFence() override { return fence_ ? fence_->GetCompletedValue() : 0; }

  void WaitAt(uint64_t value, uint32_t site) {
    const uint32_t saved = wait_site_;
    wait_site_ = site;
    WaitForFence(value);
    wait_site_ = saved;
  }
  void WaitForFence(uint64_t value) override {
    if (!fence_ || lost_) return;
    if (value >= next_fence_) value = Submit();  // waiting on unsubmitted work: submit it first
    if (fence_->GetCompletedValue() >= value) return;
    fence_->SetEventOnCompletion(value, fence_event_);
    const auto wait_start = std::chrono::steady_clock::now();
    const DWORD wait_result = WaitForSingleObject(fence_event_, 10000);
    ++stats_.fence_waits;
    const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_start).count();
    stats_.fence_wait_ms += waited;
    ++stats_.wait_count[wait_site_ % 6];
    stats_.wait_ms[wait_site_ % 6] += waited;
    if (wait_result != WAIT_OBJECT_0) {
      lost_ = true;
      std::string why;
      IsLost(&why);
      lost_reason_ = "GPU timeout (10 s) " + why;
      REXLOG_ERROR("gow2_renderer: {}", lost_reason_);
    }
    Retire();
  }

  void BeginEvent(const char* name) override {
    if (Recording() && name) list_->BeginEvent(1, name, UINT(std::strlen(name) + 1));  // 1 = ANSI string (PIX)
  }
  void EndEvent() override {
    if (list_open_) list_->EndEvent();
  }

  const BackendStats& Stats() const override { return stats_; }

 private:
  bool stencil_ref_export_ = false;
  uint32_t wait_site_ = 0;  // which call site the current WaitForFence belongs to (0 = called by the renderer)
  struct Shader {
    ShaderStage stage = ShaderStage::kVertex;
    std::shared_ptr<const std::vector<uint8_t>> code;  // patch 0058: shared with pipeline jobs
    uint8_t prefetch_status = 0;  // recording-thread only: 0 unchecked, 1 unavailable, 2 cached
  };
  struct FencedAllocator {
    ID3D12CommandAllocator* allocator;
    uint64_t fence;
  };
  struct Marker {
    uint64_t fence;
    uint64_t position;
  };
  struct DeferredRelease {
    ID3D12Resource* resource;
    uint64_t fence;
    int32_t heap = -1;  // patch 0017: a placed texture's heap range goes back to the heap with it
    uint64_t heap_offset = 0, heap_size = 0;
  };
  // Patch 0017: shared heaps for placed textures, each with its free ranges (offset -> size, coalesced).
  struct TextureHeap {
    ID3D12Heap* heap = nullptr;
    std::map<uint64_t, uint64_t> free;
  };
  static constexpr uint64_t kTextureHeapBytes = 64ull << 20;
  static constexpr uint64_t kTexturePlacedMax = 16ull << 20;
  static constexpr uint64_t kTexturePlacedAlign = 64ull << 10;
  bool AllocTextureRange(uint64_t size, int32_t& heap, uint64_t& offset) {
    for (size_t h = 0; h < tex_heaps_.size(); ++h) {
      auto& f = tex_heaps_[h].free;
      for (auto it = f.begin(); it != f.end(); ++it) {
        if (it->second < size) continue;
        offset = it->first;
        const uint64_t rest = it->second - size;
        f.erase(it);
        if (rest) f.emplace(offset + size, rest);
        heap = int32_t(h);
        return true;
      }
    }
    if (tex_heaps_.size() >= 64) return false;  // 4 GB of heaps: committed from here on
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = kTextureHeapBytes;
    hd.Properties = D3D12_HEAP_PROPERTIES{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
    hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
    TextureHeap th;
    const HRESULT hr = device_->CreateHeap(&hd, IID_PPV_ARGS(&th.heap));
    if (FAILED(hr)) {
      REXLOG_WARN("gow2_renderer: {} - placed textures off from here on", Hr("CreateHeap(textures)", hr));
      options_.placed_textures = false;
      return false;
    }
    th.heap->SetName(L"gow2 texture heap");
    th.free.emplace(size, kTextureHeapBytes - size);
    tex_heaps_.push_back(std::move(th));
    ++stats_.texture_heaps;
    uint64_t free_bytes = 0, largest_free = 0;
    for (size_t hi = 0; hi + 1 < tex_heaps_.size(); ++hi)  // the heaps before this new one
      for (const auto& [o, n] : tex_heaps_[hi].free) {
        free_bytes += n;
        largest_free = std::max(largest_free, n);
      }
    REXLOG_INFO("gow2_renderer: texture heap {} created ({} MB, patch 0017) for a {} KB texture | live placed textures {} "
                "({:.1f} MB), free in the other heaps {:.1f} MB (largest range {} KB)",
                tex_heaps_.size(), kTextureHeapBytes >> 20, size >> 10, placed_live_count_,
                double(placed_live_bytes_) / 1048576.0, double(free_bytes) / 1048576.0,
                largest_free >> 10);
    heap = int32_t(tex_heaps_.size() - 1);
    offset = 0;
    return true;
  }
  void FreeTextureRange(int32_t heap, uint64_t offset, uint64_t size) {
    if (heap < 0 || size_t(heap) >= tex_heaps_.size() || !size) return;
    auto& f = tex_heaps_[size_t(heap)].free;
    auto next = f.lower_bound(offset);
    if (next != f.end() && offset + size == next->first) {  // merge with the following range
      size += next->second;
      next = f.erase(next);
    }
    if (next != f.begin()) {
      auto prev = std::prev(next);
      if (prev->first + prev->second == offset) {  // merge with the preceding range
        prev->second += size;
        return;
      }
    }
    f.emplace(offset, size);
  }
  void ReleaseDeferred(DeferredRelease& d) {
    SafeRelease(d.resource);
    if (d.heap >= 0 && d.heap_size) {  // job 306
      placed_live_bytes_ -= std::min(placed_live_bytes_, d.heap_size);
      if (placed_live_count_) --placed_live_count_;
    }
    FreeTextureRange(d.heap, d.heap_offset, d.heap_size);
  }

  void Shutdown() {
    if (fence_ && queue_ && !lost_) {
      if (list_open_) Submit();
      WaitForFence(last_submitted_);
    }
    proxy_.Stop();  // the recording thread is idle after the last Submit; nothing may reach it after this
    copies_.Stop();
    for (auto& d : deferred_releases_) ReleaseDeferred(d);
    deferred_releases_.clear();
    for (Texture& t : textures_) SafeRelease(t.resource);
    SafeRelease(adapter3_);  // patch 0032
    for (TextureHeap& th : tex_heaps_) SafeRelease(th.heap);  // patch 0017: after every texture placed in them
    tex_heaps_.clear();
    StopPipelineWorkers();
    if (saver_.joinable()) saver_.join();  // patch 0010
    if (pipeline_library_) {
      if (pipeline_library_dirty_.load()) SavePipelineLibrary();
      SafeRelease(pipeline_library_);
    }
    if (warm_pipeline_library_) {
      if (warm_library_dirty_.load()) SavePipelineLibrary(false, true);
      SafeRelease(warm_pipeline_library_);
    }
    for (Pipeline& p : pipelines_) SafeRelease(p.pso);
    for (IndexBuffer& b : index_buffers_) SafeRelease(b.resource);
    for (Readback& r : readbacks_) SafeRelease(r.buffer);
    SafeRelease(ts_heap_);  // job 176
    SafeRelease(ts_readback_);
    ts_mapped_ = nullptr;
    SafeRelease(occ_heap_);  // upl-s4c
    SafeRelease(occ_readback_);
    occ_mapped_ = nullptr;
    if (occ_event_) { CloseHandle(occ_event_); occ_event_ = nullptr; }
    for (auto& a : allocators_) SafeRelease(a.allocator);
    allocators_.clear();
    SafeRelease(current_allocator_);
    for (auto*& t : default_textures_) SafeRelease(t);
    SafeRelease(real_list_);
    SafeRelease(guest_);
    SafeRelease(scratch_);  // patch 0021
    SafeRelease(upload_);
    SafeRelease(zero_cb_);
    SafeRelease(shader_heap_);
    for (SamplerPool& pool : sampler_pools_) SafeRelease(pool.heap);
    rtv_heap_.Shutdown();
    dsv_heap_.Shutdown();
    view_heap_.Shutdown();
    default_table_heap_.Shutdown();
    SafeRelease(graphics_root_);
    SafeRelease(compute_root_);
    SafeRelease(info_queue_);
    SafeRelease(fence_);
    SafeRelease(queue_);
    SafeRelease(device_);
    if (fence_event_) {
      CloseHandle(fence_event_);
      fence_event_ = nullptr;
    }
    ready_ = false;
  }

  bool ValidTexture(TextureHandle h) const { return h && h < textures_.size() && textures_[h].resource; }

  // Opens the command list if needed (binding heaps and root signatures). False when the device is unusable.
  // Batched texture loads: every recording call but the texture-load ones (OpenList) first records the queued scratch
  // copies, so nothing can read a texture before its copy.
  bool Recording() {
    if (!OpenList()) return false;
    if (!scratch_copies_.empty()) FlushScratchCopies();
    return true;
  }
  bool OpenList() {
    if (!ready_ || lost_) return false;
    if (list_open_) return true;
    // An allocator whose submission has completed, or a new one.
    const uint64_t done = CompletedFence();
    for (size_t i = 0; i < allocators_.size(); ++i) {
      if (allocators_[i].fence <= done) {
        current_allocator_ = allocators_[i].allocator;
        allocators_.erase(allocators_.begin() + ptrdiff_t(i));
        break;
      }
    }
    HRESULT hr;
    if (!current_allocator_) {
      if (FAILED(hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&current_allocator_)))) {
        lost_ = true;
        lost_reason_ = Hr("CreateCommandAllocator", hr);
        return false;
      }
    }
    if (!real_list_) {
      // First list: created open on the allocator, before the proxy (and its recording thread) starts.
      if (FAILED(hr = current_allocator_->Reset()) ||
          FAILED(hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, current_allocator_, nullptr,
                                                 IID_PPV_ARGS(&real_list_)))) {
        lost_ = true;
        lost_reason_ = Hr("CreateCommandList", hr);
        return false;
      }
      real_list_->SetName(L"gow2 renderer list");  // patch 0006: names the DRED breadcrumb output
      proxy_.Start(real_list_, queue_, threaded_recording_);
      if (threaded_recording_) REXLOG_INFO("gow2_renderer: command recording on its own thread (threaded_recording)");
    } else if (FAILED(hr = list_->Reset(current_allocator_, nullptr))) {  // also resets the allocator
      lost_ = true;
      lost_reason_ = Hr("ID3D12GraphicsCommandList::Reset", hr);
      return false;
    }
    list_open_ = true;
    // Jobs 176-177: GPU time per list and category - a timestamp when GPU work of a new category starts (TsCat), one
    // at the end (Submit), resolved into a readback ring and summed when the list's fence passes (Retire).
    ts_open_ = EnsureTimestamps();
    ts_cur_ = TsList{0, ts_head_, 0, {}};
    gpu_cat_emitted_ = 0xFF;
    ID3D12DescriptorHeap* heaps[] = {shader_heap_, Pool().heap};
    list_->SetDescriptorHeaps(2, heaps);
    Pool().in_list = true;  // patch 0034
    list_->SetGraphicsRootSignature(graphics_root_);
    list_->SetGraphicsRootShaderResourceView(kRootGuestSrv, guest_->GetGPUVirtualAddress());
    list_->SetGraphicsRootShaderResourceView(kRootUploadSrv, upload_->GetGPUVirtualAddress());
    list_->SetGraphicsRootUnorderedAccessView(kRootGuestUav, guest_->GetGPUVirtualAddress());
    list_->SetGraphicsRootDescriptorTable(kRootSrvTable, shader_heap_->GetGPUDescriptorHandleForHeapStart());
    list_->SetGraphicsRootDescriptorTable(kRootSamplerTable, Pool().heap->GetGPUDescriptorHandleForHeapStart());
    // Everything recorded state-wise must be re-applied in the new list.
    rt_dirty_ = viewport_dirty_ = scissor_dirty_ = stencil_dirty_ = pipeline_dirty_ = blend_dirty_ = true;
    positions_dirty_ = positions_count_ != 0;  // patch 0089: a new list starts on the standard pattern
    // Buffers decay to COMMON when a command list finishes executing (D3D12 implicit state rules), so the guest-memory
    // buffer starts every list in COMMON.
    guest_state_ = D3D12_RESOURCE_STATE_COMMON;
    scratch_state_ = D3D12_RESOURCE_STATE_COMMON;  // patch 0021: the same rule
    root_cbv_dirty_ = 0xFF;
    cbv_reupload_ = true;
    if (textures_custom_) textures_dirty_ = true;
    if (occ_open_ && !occ_open_in_list_) ContinueOcclusionQuery();  // upl-s4c
    return true;
  }

  // ---- Vendor SDK access (Gears 2 notes/64, Gears 3 job 052): see Backend::BeginNative ----
  bool BeginNative(void** device, void** command_list) override {
    if (native_open_ || !Recording()) return false;
    proxy_.Drain();  // queued barriers emitted, everything encoded so far is on the real list, the recorder is idle
    native_open_ = true;
    if (device) *device = device_;
    if (command_list) *command_list = real_list_;
    return true;
  }
  void* NativeTexture(TextureHandle h, uint32_t state) override {
    if (!native_open_ || !ValidTexture(h)) return nullptr;
    Texture& t = textures_[h];
    const D3D12_RESOURCE_STATES want = D3D12_RESOURCE_STATES(state);
    if (t.state != want) {  // straight into the real list: nothing may go through the proxy between Begin and End
      D3D12_RESOURCE_BARRIER b{};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = t.resource;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      b.Transition.StateBefore = t.state;
      b.Transition.StateAfter = want;
      real_list_->ResourceBarrier(1, &b);
      t.state = want;
    }
    return t.resource;
  }
  void EndNative() override {
    if (!native_open_) return;
    native_open_ = false;
    // The SDK may have set its own heaps, root signatures, pipeline, render targets and views: re-apply ours (through the
    // proxy, after the SDK's calls) and mark every recorded binding dirty, as for a new list.
    ID3D12DescriptorHeap* heaps[] = {shader_heap_, Pool().heap};
    list_->SetDescriptorHeaps(2, heaps);
    list_->SetGraphicsRootSignature(graphics_root_);
    list_->SetGraphicsRootShaderResourceView(kRootGuestSrv, guest_->GetGPUVirtualAddress());
    list_->SetGraphicsRootShaderResourceView(kRootUploadSrv, upload_->GetGPUVirtualAddress());
    list_->SetGraphicsRootUnorderedAccessView(kRootGuestUav, guest_->GetGPUVirtualAddress());
    list_->SetGraphicsRootDescriptorTable(kRootSrvTable, shader_heap_->GetGPUDescriptorHandleForHeapStart());
    list_->SetGraphicsRootDescriptorTable(kRootSamplerTable, Pool().heap->GetGPUDescriptorHandleForHeapStart());
    rt_dirty_ = viewport_dirty_ = scissor_dirty_ = stencil_dirty_ = pipeline_dirty_ = blend_dirty_ = true;
    root_cbv_dirty_ = 0xFF;
    cbv_reupload_ = true;
    textures_dirty_ = true;
  }
  bool native_open_ = false;

  // ---- GPU timestamps (jobs 176-177) ----
  void SetGpuCategory(uint8_t category) override { gpu_cat_pending_ = category; }
  void SetGpuTag(uint32_t tag) override { gpu_tag_pending_ = tag; }  // patch 0101
  void TakeGpuTagTimes(std::vector<GpuTagTime>& out) override {
    out.swap(tag_times_);
    tag_times_.clear();
  }
  bool TextureInfo(TextureHandle h, TextureDesc& out) const override {  // patch 0124
    if (!ValidTexture(h)) return false;
    out = textures_[h].desc;
    return true;
  }
  std::string DescribeTexture(TextureHandle h) const override {  // patch 0101
    if (!ValidTexture(h)) return "none";
    const Texture& t = textures_[h];
    const D3D12_RESOURCE_DESC rd = t.resource->GetDesc();
    char b[256];
    std::snprintf(b, sizeof b, "%ux%ux%u m%u s%u DXGI %d flags 0x%X%s%s %s state 0x%X", t.desc.width, t.desc.height,
                  t.desc.depth_or_layers, t.desc.mips, t.desc.samples, int(rd.Format), unsigned(rd.Flags),
                  t.typeless ? " typeless" : "", t.bits ? " bits" : "", t.heap >= 0 ? "placed" : "committed",
                  unsigned(t.state));
    return b;
  }
  void TsCat() {  // before each piece of GPU work
    if (!options_.gpu_timestamp_split) {  // patch 0109: one timestamp at the list's first work, one at its end
      if (ts_open_ && ts_cur_.count == 0) TsEmit(0);
      return;
    }
    if (ts_open_ && (gpu_cat_pending_ != gpu_cat_emitted_ || gpu_tag_pending_ != gpu_tag_emitted_)) TsEmit(gpu_cat_pending_);
  }
  void TsEmit(uint8_t cat) {
    if (ts_cur_.count >= kTsPerList - 1 && cat != 0xFE) return;  // keep one slot for the end mark
    if (ts_used_ + 1 >= kTsCount) return;                        // the ring is full of lists not read back yet
    list_->EndQuery(ts_heap_, D3D12_QUERY_TYPE_TIMESTAMP, ts_head_);
    ts_head_ = (ts_head_ + 1) % kTsCount;
    ++ts_cur_.count;
    ++ts_used_;
    ts_cur_.cats.push_back(cat);
    ts_cur_.tags.push_back(gpu_tag_pending_);  // patch 0101
    gpu_cat_emitted_ = cat;
    gpu_tag_emitted_ = gpu_tag_pending_;
  }
  bool EnsureTimestamps() {
    if (ts_heap_) return true;
    if (ts_failed_ || !device_ || !queue_) return false;
    ts_failed_ = true;  // until it works
    if (FAILED(queue_->GetTimestampFrequency(&ts_freq_)) || !ts_freq_) return false;
    D3D12_QUERY_HEAP_DESC desc{};
    desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    desc.Count = kTsCount;
    std::string error;
    void* mapped = nullptr;
    if (FAILED(device_->CreateQueryHeap(&desc, IID_PPV_ARGS(&ts_heap_))) ||
        !CreateBuffer(UINT64(kTsCount) * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                      D3D12_RESOURCE_STATE_COPY_DEST, &ts_readback_, error) ||
        FAILED(ts_readback_->Map(0, nullptr, &mapped))) {
      REXLOG_WARN("gow2_renderer: GPU timestamps unavailable {}", error);
      SafeRelease(ts_heap_);
      SafeRelease(ts_readback_);
      return false;
    }
    ts_mapped_ = static_cast<const uint64_t*>(mapped);
    ts_failed_ = false;
    return true;
  }

  // ---- occlusion query helpers (upl-s4c) ----
  bool EnsureOcclusion() {
    if (occ_heap_) return true;
    if (occ_failed_ || !device_) return false;
    D3D12_QUERY_HEAP_DESC desc{};
    desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    desc.Count = kOcclusionSlots;
    HRESULT hr = device_->CreateQueryHeap(&desc, IID_PPV_ARGS(&occ_heap_));
    std::string error;
    void* mapped = nullptr;
    if (FAILED(hr) || !CreateBuffer(UINT64(kOcclusionSlots) * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                    D3D12_RESOURCE_STATE_COPY_DEST, &occ_readback_, error) ||
        FAILED(occ_readback_->Map(0, nullptr, &mapped))) {
      REXLOG_ERROR("gow2_renderer: occlusion queries unavailable ({})", FAILED(hr) ? Hr("CreateQueryHeap", hr) : error);
      SafeRelease(occ_heap_);
      SafeRelease(occ_readback_);
      occ_mapped_ = nullptr;
      occ_failed_ = true;
      return false;
    }
    occ_mapped_ = static_cast<const uint64_t*>(mapped);
    occ_slot_fence_.assign(kOcclusionSlots, 0);
    occ_slot_held_.assign(kOcclusionSlots, 0);
    occ_queries_.assign(1, OcclusionQuery{});  // handle 0 = none
    return true;
  }
  // A slot nobody holds whose last use has completed on our GPU; -1 when none (never waits).
  int64_t AcquireOcclusionSlot() {
    const uint64_t done = CompletedFence();
    for (uint32_t i = 0; i < kOcclusionSlots; ++i) {
      const uint32_t s = (occ_cursor_ + i) % kOcclusionSlots;
      if (!occ_slot_held_[s] && occ_slot_fence_[s] <= done) {
        occ_slot_held_[s] = 1;
        occ_slot_fence_[s] = 0;
        occ_cursor_ = (s + 1) % kOcclusionSlots;
        return s;
      }
    }
    return -1;
  }
  void CloseOcclusionPart() {
    OcclusionQuery& q = occ_queries_[occ_open_];
    const uint32_t s = q.slot[q.parts - 1];
    list_->EndQuery(occ_heap_, D3D12_QUERY_TYPE_OCCLUSION, s);
    occ_slots_in_list_.push_back(s);
  }
  void ContinueOcclusionQuery() {
    OcclusionQuery& q = occ_queries_[occ_open_];
    const int64_t s = q.parts < kOcclusionMaxParts ? AcquireOcclusionSlot() : -1;
    if (s < 0) {
      q.broken = true;  // no result for this one; the caller answers some other way
      q.ended = true;
      occ_open_ = 0;
      return;
    }
    q.slot[q.parts++] = uint32_t(s);
    list_->BeginQuery(occ_heap_, D3D12_QUERY_TYPE_OCCLUSION, UINT(s));
    occ_open_in_list_ = true;
    ++stats_.occlusion_segments;
  }

  bool PrepareDraw() {
    using Clock = TscClock;
    const auto t0 = Clock::now();
    const bool ok = PrepareDrawInner();
    stats_.prepare_draw_ms += TscMs(t0);
    return ok;
  }
  bool PrepareDrawInner() {
    using Clock = TscClock;
    if (!Recording()) return false;
    if (textures_dirty_) {
      const auto t0 = Clock::now();
      bool ok = true;
      for (int guard = 0; guard < 4 && textures_dirty_ && ok; ++guard) {
        if (!Recording()) return false;  // patch 0029 (Gears 3 bug hunt C1): a wait inside closed the list - reopen
        const uint64_t submits = stats_.submits;
        ok = ApplyTextures();
        if (stats_.submits != submits) continue;  // a new list was opened: tables must be rebuilt in it
      }
      stats_.apply_textures_ms += TscMs(t0);
      if (!ok) return false;
    }
    if (cbv_pending_ || cbv_reupload_) {
      const uint64_t submits = stats_.submits;
      const auto t0 = Clock::now();
      bool ok = UploadConstants();
      // Patch 0029 (Gears 3 bug hunt C2): a ring-full submit inside reopened a list with the static texture tables
      // bound - rebuild this draw's textures and constants in the new list.
      if (ok && stats_.submits != submits) {
        if (!Recording()) return false;
        textures_dirty_ = true;
        for (int guard = 0; guard < 4 && textures_dirty_ && ok; ++guard) {
          if (!Recording()) return false;
          ok = ApplyTextures();
        }
        if (ok) ok = UploadConstants();
        ++stats_.redone_after_submit;
      }
      stats_.upload_constants_ms += TscMs(t0);
      if (!ok) return false;
    }
    if (!pipeline_ || pipeline_ >= pipelines_.size() || pipelines_[pipeline_].compute ||
        !pipelines_[pipeline_].ready.load(std::memory_order_acquire) || !pipelines_[pipeline_].pso)
      return false;
    if (guest_writable_) {
      // Memexport draw (notes/11): the buffer becomes a UAV; a draw right after another exporting draw gets a UAV
      // barrier so it sees the earlier writes. Leaving UAV state (next ordinary draw, copy or dispatch) is a transition,
      // which also orders the writes before the readers.
      if (guest_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = guest_;
        list_->ResourceBarrier(1, &b);
      } else {
        TransitionGuest(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      }
      ++stats_.memexport_draws;
    } else if (guest_state_ != (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) {
      TransitionGuest(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    if (rt_dirty_) {
      D3D12_CPU_DESCRIPTOR_HANDLE rtvs[kMaxColorTargets];
      UINT n = 0;
      for (uint32_t i = 0; i < bound_color_count_; ++i) {
        if (!ValidTexture(bound_colors_[i]) || textures_[bound_colors_[i]].rtv < 0) {
          if (null_rtv_ < 0) break;
          rtvs[n++] = rtv_heap_.Handle(null_rtv_);  // the pipeline has DXGI_FORMAT_UNKNOWN there
          continue;
        }
        Transition(textures_[bound_colors_[i]], D3D12_RESOURCE_STATE_RENDER_TARGET);
        const Texture& ct = textures_[bound_colors_[i]];
        rtvs[n++] = rtv_heap_.Handle(bound_bits_ && ct.rtv_bits >= 0 ? ct.rtv_bits : ct.rtv);
      }
      // Trailing null slots are just not bound.
      while (n && (bound_colors_[n - 1] == 0 || !ValidTexture(bound_colors_[n - 1]))) --n;
      D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
      const bool has_depth = ValidTexture(bound_depth_) && textures_[bound_depth_].dsv >= 0;
      if (has_depth) {
        Transition(textures_[bound_depth_], D3D12_RESOURCE_STATE_DEPTH_WRITE);
        dsv = dsv_heap_.Handle(textures_[bound_depth_].dsv);
      }
      list_->OMSetRenderTargets(n, n ? rtvs : nullptr, FALSE, has_depth ? &dsv : nullptr);
      rt_dirty_ = false;
    }
    if (pipeline_dirty_) {
      list_->SetPipelineState(pipelines_[pipeline_].pso);
      list_->IASetPrimitiveTopology(pipelines_[pipeline_].topology);
      pipeline_dirty_ = false;
    }
    if (viewport_dirty_) {
      D3D12_VIEWPORT v{viewport_.x, viewport_.y, viewport_.width, viewport_.height, viewport_.min_depth,
                       viewport_.max_depth};
      list_->RSSetViewports(1, &v);
      viewport_dirty_ = false;
    }
    if (scissor_dirty_) {
      D3D12_RECT r{scissor_.x0, scissor_.y0, scissor_.x1, scissor_.y1};
      list_->RSSetScissorRects(1, &r);
      scissor_dirty_ = false;
    }
    if (stencil_dirty_) {
      list_->OMSetStencilRef(stencil_ref_);
      stencil_dirty_ = false;
    }
    ApplySamplePositions();  // patch 0089
    if (blend_dirty_) {
      list_->OMSetBlendFactor(blend_constant_);
      blend_dirty_ = false;
    }
    for (UINT i = 0; i < 8; ++i) {
      // A slot never set reads the zero buffer, so a missing SetConstants is never a GPU fault.
      if (root_cbv_dirty_ & (1u << i)) list_->SetGraphicsRootConstantBufferView(i, root_cbv_[i] ? root_cbv_[i] : zero_cb_gpu_);
    }
    root_cbv_dirty_ = 0;
    return true;
  }

  // Constants are kept in a CPU shadow and copied into the upload ring by the draw that uses them. The ring is only
  // protected up to the submission that was open when data was written, so after a new command list is opened every
  // slot is copied again: a forced submission in the middle of a draw (ring full) never leaves the draw reading reused
  // ring memory.
  bool UploadConstants() {
    for (int guard = 0; guard < 4 && (cbv_pending_ || cbv_reupload_); ++guard) {
      if (cbv_reupload_) cbv_pending_ = 0xFF;
      cbv_reupload_ = false;
      const uint64_t submits = stats_.submits;
      for (UINT i = 0; i < 8; ++i) {
        if (i >= kRootPsCbv && cbv_shared_[i - kRootPsCbv]) {  // job 177: follows the vertex slot (set below)
          cbv_pending_ &= ~(1u << i);
          continue;
        }
        if (!(cbv_pending_ & (1u << i)) || cbv_shadow_[i].empty()) continue;
        uint64_t off;
        uint8_t* dst = AllocUpload(cbv_shadow_[i].size(), 256, off);
        if (!dst) return false;
        if (stats_.submits != submits) break;  // a new list was opened meanwhile: start again
        std::memcpy(dst, cbv_shadow_[i].data(), cbv_shadow_[i].size());
        root_cbv_[i] = upload_gpu_ + off;
        root_cbv_dirty_ |= 1u << i;
        cbv_pending_ &= ~(1u << i);
        if (i < kRootPsCbv && cbv_shared_[i]) {
          root_cbv_[i + kRootPsCbv] = root_cbv_[i];
          root_cbv_dirty_ |= 1u << (i + kRootPsCbv);
        }
      }
    }
    return !cbv_reupload_;
  }

  // UAV of one mip of a 2D storage texture (mip 0: the one made at creation).
  int32_t UavSlot(Texture& t, uint32_t mip) {
    if (t.uav < 0) return -1;
    if (mip == 0) return t.uav;
    const bool is_3d = t.desc.dimension == TextureDimension::k3D;  // patch 0026: 3D mips too
    if ((t.desc.dimension != TextureDimension::k2D && !is_3d) || mip >= t.desc.mips) return -1;
    if (t.uav_mips.size() < mip) t.uav_mips.resize(mip, -1);
    int32_t& slot = t.uav_mips[mip - 1];
    if (slot < 0) {
      slot = view_heap_.Alloc();
      if (slot < 0) return -1;
      D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
      u.Format = ResourceFormat(t.desc.format);
      if (is_3d) {
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        u.Texture3D.MipSlice = mip;
        u.Texture3D.FirstWSlice = 0;
        u.Texture3D.WSize = UINT(-1);
      } else {
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        u.Texture2D.MipSlice = mip;
      }
      device_->CreateUnorderedAccessView(t.resource, nullptr, &u, view_heap_.Handle(slot));
    }
    return slot;
  }

  // CPU descriptor of a texture view (created on first use, cached on the texture).
  int32_t ViewSlot(Texture& t, const TextureView& v) {
    const uint64_t key = uint64_t(v.components[0]) | uint64_t(v.components[1]) << 4 | uint64_t(v.components[2]) << 8 |
                         uint64_t(v.components[3]) << 12 | uint64_t(v.signed_view) << 16 | uint64_t(v.dimension) << 17 |
                         uint64_t(v.plane) << 20 | uint64_t(v.bits_view && t.bits) << 24;
    for (const auto& [k, slot] : t.views)
      if (k == key) return slot;
    const int32_t slot = view_heap_.Alloc();
    if (slot < 0) return -1;
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = v.signed_view && SignedFormat(t.desc.format) != DXGI_FORMAT_UNKNOWN ? SignedFormat(t.desc.format)
                                                                                    : ResourceFormat(t.desc.format);
    if (v.bits_view && t.bits) d.Format = BitsFormat(t.desc.format);
    auto src = [](uint8_t c) {
      return c < 4 ? D3D12_SHADER_COMPONENT_MAPPING(c)
                   : c == 4 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0 : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
    };
    d.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(src(v.components[0]), src(v.components[1]),
                                                                        src(v.components[2]), src(v.components[3]));
    const UINT mips = t.resource->GetDesc().MipLevels;
    if (v.dimension == ViewDimension::kCube) {
      d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
      d.TextureCube.MipLevels = mips;
    } else if (v.dimension == ViewDimension::k3D) {
      d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
      d.Texture3D.MipLevels = mips;
    } else {
      d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      d.Texture2D.MipLevels = mips;
    }
    if (IsDepthFormat(t.desc.format)) {  // one plane of a depth/stencil texture (EDRAM transfers)
      d.Format = DepthPlaneFormat(t.desc.format, v.plane);
      d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      d.Texture2D.MipLevels = 1;
      d.Texture2D.PlaneSlice = v.plane;
    }
    device_->CreateShaderResourceView(t.resource, &d, view_heap_.Handle(slot));
    t.views.push_back({key, slot});
    return slot;
  }

  static uint64_t HashSamplers(const SamplerDesc* s, uint32_t count) {
    uint64_t h = 1469598103934665603ull ^ count;
    for (uint32_t i = 0; i < count; ++i) {
      const SamplerDesc& d = s[i];
      uint32_t f[3];
      std::memcpy(&f[0], &d.lod_bias, 4);
      std::memcpy(&f[1], &d.min_lod, 4);
      std::memcpy(&f[2], &d.max_lod, 4);
      const uint64_t a = uint64_t(d.mag_linear) | uint64_t(d.min_linear) << 2 | uint64_t(d.mip_linear) << 4 |
                         uint64_t(d.max_anisotropy) << 8 | uint64_t(d.address[0]) << 16 | uint64_t(d.address[1]) << 20 |
                         uint64_t(d.address[2]) << 24 | uint64_t(d.border) << 28;
      for (uint64_t v : {a, uint64_t(f[0]), uint64_t(f[1]), uint64_t(f[2])}) {
        h ^= v;
        h *= 1099511628211ull;
      }
    }
    return h;
  }

  // A sampler table holding the first `count` of these samplers, from the smallest size class that fits (a larger one
  // when that class is full); UINT32_MAX if none could be freed. Content-hashed per heap, so a set seen before in this
  // heap is reused without writing; a free or retired table is taken next, waiting on our GPU only when every table
  // that fits is in use by this frame's lists (upl-n109).
  // Patch 0034: move the open list to another sampler heap the GPU has finished with (not bound in this list), so a full
  // heap does not mean waiting. The caller binds its tables right after (ApplyTextures sets the SRV and sampler tables;
  // after SetDescriptorHeaps every table must be set again, and the draw-time ones are - static tables included).
  bool SwitchSamplerHeap() {
    const uint64_t done = CompletedFence();
    for (uint32_t k = 1; k < kSamplerPools; ++k) {
      const uint32_t p = (cur_pool_ + k) % kSamplerPools;
      SamplerPool& pool = sampler_pools_[p];
      if (pool.in_list || pool.fence > done) continue;
      cur_pool_ = p;
      for (SamplerTable& t : pool.tables) t.in_open_list = false;
      ID3D12DescriptorHeap* heaps[] = {shader_heap_, pool.heap};
      list_->SetDescriptorHeaps(2, heaps);
      pool.in_list = true;
      ++stats_.sampler_heap_switches;
      return true;
    }
    return false;
  }
  // Patch 0034: this heap, then each other heap the GPU is done with (a set seen before in that heap is found there
  // too), and only then the old wait.
  uint32_t SamplerTableFor(const SamplerDesc* s, uint32_t count) {
    for (uint32_t k = 0; k < kSamplerPools; ++k) {
      const uint32_t t = SamplerTableIn(s, count, false);
      if (t != UINT32_MAX) return t;
      if (!SwitchSamplerHeap()) break;
    }
    return SamplerTableIn(s, count, true);
  }
  uint32_t SamplerTableIn(const SamplerDesc* s, uint32_t count, bool allow_wait) {
    SamplerPool& pool = Pool();
    uint32_t cls = 0;
    while (cls + 1 < kSamplerClasses && kSamplerClassSize[cls] < count) ++cls;
    for (uint32_t c = cls; c < kSamplerClasses; ++c) {
      const uint64_t key = HashSamplers(s, kSamplerClassSize[c]);
      if (auto it = pool.map.find(key); it != pool.map.end()) {
        SamplerTable& t = pool.tables[it->second];
        if (t.used && t.size == kSamplerClassSize[c] && std::equal(t.descs.begin(), t.descs.end(), s)) {
          ++stats_.sampler_table_hits;
          return it->second;
        }
      }
      if (c == cls) continue;  // larger classes: only reuse an existing table here, allocation follows below
    }
    uint32_t pick = UINT32_MAX, pick_class = cls;
    for (int attempt = 0; attempt < 2 && pick == UINT32_MAX; ++attempt) {
      const uint64_t done = CompletedFence();
      for (uint32_t c = cls; c < kSamplerClasses && pick == UINT32_MAX; ++c) {
        uint32_t first = 1;
        for (uint32_t k = 0; k < c; ++k) first += kSamplerClassTables[k];
        const uint32_t last = first + kSamplerClassTables[c];
        uint64_t oldest = UINT64_MAX;
        for (uint32_t i = first; i < last; ++i) {
          SamplerTable& t = pool.tables[i];
          if (!t.used) {
            pick = i;
            break;
          }
          if (!t.in_open_list && t.fence <= done && t.fence < oldest) {
            oldest = t.fence;
            pick = i;
          }
        }
        if (pick != UINT32_MAX) pick_class = c;
      }
      if (pick == UINT32_MAX && !allow_wait) break;  // patch 0034: the caller tries another heap first
      if (pick == UINT32_MAX) {
        ++stats_.sampler_table_waits;
        WaitAt(Submit(), 2);
      }
    }
    if (pick == UINT32_MAX) return UINT32_MAX;
    count = kSamplerClassSize[pick_class];
    const uint64_t h = HashSamplers(s, count);
    if (pool.tables[pick].used) {
      auto it = pool.map.find(pool.tables[pick].hash);
      if (it != pool.map.end() && it->second == pick) pool.map.erase(it);
    }
    pool.map[h] = pick;
    SamplerTable& t = pool.tables[pick];
    t.used = true;
    t.hash = h;
    t.descs.assign(s, s + count);
    ++stats_.sampler_tables_written;
    D3D12_CPU_DESCRIPTOR_HANDLE base = pool.heap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < count; ++i) {
      const SamplerDesc& d = s[i];
      D3D12_SAMPLER_DESC sd{};
      if (d.max_anisotropy > 1) {
        sd.Filter = D3D12_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = std::min<UINT>(d.max_anisotropy, 16);
      } else {
        sd.Filter = D3D12_ENCODE_BASIC_FILTER(d.min_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                              d.mag_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                              d.mip_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                              D3D12_FILTER_REDUCTION_TYPE_STANDARD);
        sd.MaxAnisotropy = 1;
      }
      auto addr = [](SamplerAddress a) {
        switch (a) {
          case SamplerAddress::kMirror: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
          case SamplerAddress::kClamp: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
          case SamplerAddress::kMirrorOnce: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
          case SamplerAddress::kBorder: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
          default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        }
      };
      sd.AddressU = addr(d.address[0]);
      sd.AddressV = addr(d.address[1]);
      sd.AddressW = addr(d.address[2]);
      sd.MipLODBias = std::clamp(d.lod_bias, -16.0f, 15.99f);
      sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
      const float b = d.border == 1 ? 1.0f : 0.0f, a = d.border == 0 ? 0.0f : 1.0f;
      sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = b;
      sd.BorderColor[3] = a;
      sd.MinLOD = d.min_lod;
      sd.MaxLOD = d.max_lod;
      device_->CreateSampler(&sd, {base.ptr + SIZE_T(t.offset + i) * sampler_increment_});
    }
    return pick;
  }

  // Builds and binds the texture and sampler tables for bound_views_/bound_samplers_ (or the static ones).
  bool ApplyTextures() {
    textures_dirty_ = false;
    if (!textures_custom_) {
      list_->SetGraphicsRootDescriptorTable(kRootSrvTable, shader_heap_->GetGPUDescriptorHandleForHeapStart());
      list_->SetGraphicsRootDescriptorTable(kRootSamplerTable, Pool().heap->GetGPUDescriptorHandleForHeapStart());
      return true;
    }
    const uint64_t submits = stats_.submits;
    const uint32_t table = SamplerTableFor(bound_samplers_, bound_sampler_count_);
    if (stats_.submits != submits) {
      textures_dirty_ = true;  // a new list: build again in it
      return true;
    }
    const uint32_t count = std::clamp<uint32_t>(bound_view_count_, 1u, kTextureSrvCount);
    if (table == UINT32_MAX) return false;
    // Texture-table cache (upl-n109): the same views (by texture serial) reuse the descriptors written for them earlier,
    // while the ring has not come round to them again (half the ring: far more than our GPU is ever behind).
    const D3D12_RESOURCE_STATES kSrv =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    uint64_t key = 1469598103934665603ull ^ count;
    for (uint32_t i = 0; i < count; ++i) {
      const TextureView& v = bound_views_[i];
      const uint64_t serial = ValidTexture(v.texture) ? textures_[v.texture].serial : 0;
      const uint64_t view = uint64_t(v.components[0]) | uint64_t(v.components[1]) << 4 | uint64_t(v.components[2]) << 8 |
                            uint64_t(v.components[3]) << 12 | uint64_t(v.signed_view) << 16 |
                            uint64_t(v.dimension) << 17 | uint64_t(v.plane) << 20 | uint64_t(v.bits_view) << 24 |
                            uint64_t(v.zero) << 25;
      for (uint64_t x : {serial, view}) {
        key ^= x;
        key *= 1099511628211ull;
      }
    }
    constexpr uint64_t kRingReuse = (kShaderHeapSize - kShaderHeapRingStart) / 2;
    if (auto it = texture_tables_.find(key); it != texture_tables_.end() && it->second.count == count &&
                                             ring_alloc_total_ - it->second.at < kRingReuse) {
      bool same = true;
      for (uint32_t i = 0; i < count && same; ++i) {
        const TextureView& v = bound_views_[i];
        same = it->second.views[i] == v &&
               it->second.serials[i] == (ValidTexture(v.texture) ? textures_[v.texture].serial : 0);
      }
      if (same) {
        for (uint32_t i = 0; i < count; ++i)
          if (ValidTexture(bound_views_[i].texture)) Transition(textures_[bound_views_[i].texture], kSrv);
        ++stats_.texture_table_hits;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = shader_heap_->GetGPUDescriptorHandleForHeapStart();
        list_->SetGraphicsRootDescriptorTable(kRootSrvTable, {gpu.ptr + UINT64(it->second.base) * view_increment_});
        BindSamplerTable(table);
        return true;
      }
    }
    const uint32_t base = AllocShaderDescriptors(count);
    stats_.descriptors_allocated += count;
    if (base == UINT32_MAX) return false;
    if (stats_.submits != submits) {
      textures_dirty_ = true;
      return true;
    }
    ++stats_.texture_table_misses;
    if (texture_tables_.size() > 65536) texture_tables_.clear();
    TextureTable& entry = texture_tables_[key];
    entry.base = base;
    entry.count = count;
    entry.at = ring_alloc_total_;
    entry.views.assign(bound_views_, bound_views_ + count);
    entry.serials.resize(count);
    for (uint32_t i = 0; i < count; ++i)
      entry.serials[i] = ValidTexture(bound_views_[i].texture) ? textures_[bound_views_[i].texture].serial : 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = shader_heap_->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE dst{cpu.ptr + SIZE_T(base) * view_increment_};
    device_->CopyDescriptorsSimple(count, dst, default_table_heap_.Handle(0), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (uint32_t i = 0; i < count; ++i) {
      const TextureView& v = bound_views_[i];
      if (!ValidTexture(v.texture)) {
        if (v.zero && i < 96 && null_views_[i / 32] >= 0)
          device_->CopyDescriptorsSimple(1, {dst.ptr + SIZE_T(i) * view_increment_}, view_heap_.Handle(null_views_[i / 32]),
                                         D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        continue;
      }
      Texture& t = textures_[v.texture];
      const int32_t slot = ViewSlot(t, v);
      if (slot < 0) continue;
      Transition(t, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      device_->CopyDescriptorsSimple(1, {dst.ptr + SIZE_T(i) * view_increment_}, view_heap_.Handle(slot),
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = shader_heap_->GetGPUDescriptorHandleForHeapStart();
    list_->SetGraphicsRootDescriptorTable(kRootSrvTable, {gpu.ptr + UINT64(base) * view_increment_});
    BindSamplerTable(table);
    return true;
  }

  void BindSamplerTable(uint32_t table) {
    D3D12_GPU_DESCRIPTOR_HANDLE sg = Pool().heap->GetGPUDescriptorHandleForHeapStart();
    list_->SetGraphicsRootDescriptorTable(kRootSamplerTable,
                                          {sg.ptr + UINT64(Pool().tables[table].offset) * sampler_increment_});
    if (!Pool().tables[table].in_open_list) {
      Pool().tables[table].in_open_list = true;
      tables_in_open_list_.push_back({cur_pool_, table});
    }
  }

  void Transition(Texture& t, D3D12_RESOURCE_STATES state) {
    if (t.scratch_pending) FlushScratchCopies();  // batched texture loads: its queued copy goes first
    if (t.state == state) return;
    t.uav_unbarriered = 0;  // a transition out of UAV state waits for those writes
    // Patch 0089 (Canary d3d12_render_target_cache.cc): MSAA depth drawn with programmable sample positions is
    // decompressed in place before anything but depth writing reads it.
    if (positions_ever_ && t.dsv >= 0 && t.desc.samples > 1 && t.state == D3D12_RESOURCE_STATE_DEPTH_WRITE &&
        state != D3D12_RESOURCE_STATE_DEPTH_READ) {
      D3D12_RESOURCE_BARRIER rb{};
      rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      rb.Transition.pResource = t.resource;
      rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      rb.Transition.StateBefore = t.state;
      rb.Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
      list_->ResourceBarrier(1, &rb);
      t.state = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
      list_->DecompressDepth(t.resource, ViewFormat(t.desc.format));
      ++stats_.depth_decompressions;
      if (t.state == state) return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t.resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = t.state;
    b.Transition.StateAfter = state;
    list_->ResourceBarrier(1, &b);
    t.state = state;
    if (state != D3D12_RESOURCE_STATE_RENDER_TARGET && state != D3D12_RESOURCE_STATE_DEPTH_WRITE) rt_dirty_ = true;
  }
  void TransitionGuest(D3D12_RESOURCE_STATES state) {
    if (guest_state_ == state) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = guest_;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = guest_state_;
    b.Transition.StateAfter = state;
    list_->ResourceBarrier(1, &b);
    guest_state_ = state;
  }

  // Upload ring: [tail, head) is in flight. Markers record the head at each submission.
  uint8_t* AllocUpload(uint64_t size, uint64_t align, uint64_t& offset) {
    if (size > upload_size_ / 2) {
      REXLOG_ERROR("gow2_renderer: upload of {} bytes exceeds half the {} MB ring", size, upload_size_ >> 20);
      return nullptr;
    }
    bool polled = false;
    for (int attempt = 0; attempt < 64; ++attempt) {
      Retire(polled);
      uint64_t start = (upload_head_ + align - 1) & ~(align - 1);
      const bool empty = upload_markers_.empty() && upload_tail_ == upload_head_;
      if (start + size > upload_size_) start = 0;  // wrap
      // Free space: if head >= tail, [head, size) and [0, tail); else [head, tail).
      bool fits;
      if (empty) {
        fits = true;
      } else if (upload_head_ >= upload_tail_) {
        fits = start >= upload_head_ ? true : start + size < upload_tail_;
      } else {
        fits = start >= upload_head_ && start + size < upload_tail_;
      }
      if (fits) {
        offset = start;
        upload_head_ = start + size;
        stats_.upload_bytes += size;
        if (empty) upload_tail_ = start;
        return upload_mapped_ + start;
      }
      if (!polled) {  // read the fence before giving up on the space we have
        polled = true;
        continue;
      }
      // Out of space: submit what we have and wait for the oldest marker.
      ++stats_.ring_waits;
      const bool reopen = list_open_;
      if (list_open_) Submit();
      if (!upload_markers_.empty()) WaitAt(upload_markers_.front().fence, 3);
      if (reopen && !Recording()) return nullptr;
    }
    return nullptr;
  }

  uint32_t AllocShaderDescriptors(uint32_t count) {
    bool polled = false;
    for (int attempt = 0; attempt < 64; ++attempt) {
      Retire(polled);
      uint32_t start = ring_head_;
      if (start + count > kShaderHeapSize) start = kShaderHeapRingStart;
      const bool empty = ring_markers_.empty() && ring_tail_ == ring_head_;
      bool fits;
      if (empty) fits = true;
      else if (ring_head_ >= ring_tail_) fits = start >= ring_head_ ? true : start + count < ring_tail_;
      else fits = start >= ring_head_ && start + count < ring_tail_;
      if (fits) {
        ring_alloc_total_ += (start == ring_head_ ? 0u : kShaderHeapSize - ring_head_) + count;
        ring_head_ = start + count;
        if (empty) ring_tail_ = start;
        return start;
      }
      if (!polled) {
        polled = true;
        continue;
      }
      const bool reopen = list_open_;
      ++stats_.descriptor_ring_waits;
      if (list_open_) Submit();
      if (!ring_markers_.empty()) WaitAt(ring_markers_.front().fence, 4);
      if (reopen && !Recording()) return UINT32_MAX;
    }
    return UINT32_MAX;
  }

  // poll = false: use the fence value read last time (ID3D12Fence::GetCompletedValue goes into the driver each time; job
  // 156 had it per allocation). Stale is safe: it only frees less. The allocators poll before they would wait.
  void Retire(bool poll = true) {
    if (!fence_) return;
    if (poll) completed_ = fence_->GetCompletedValue();
    const uint64_t done = completed_;
    while (!upload_markers_.empty() && upload_markers_.front().fence <= done) {
      upload_tail_ = upload_markers_.front().position;
      upload_markers_.pop_front();
    }
    if (upload_markers_.empty() && !list_open_) upload_tail_ = upload_head_;
    while (!ring_markers_.empty() && ring_markers_.front().fence <= done) {
      ring_tail_ = uint32_t(ring_markers_.front().position);
      ring_markers_.pop_front();
    }
    if (ring_markers_.empty() && !list_open_) ring_tail_ = ring_head_;
    while (!ts_pending_.empty() && ts_pending_.front().fence <= done) {  // jobs 176-177
      const TsList& l = ts_pending_.front();
      const double k = 1000.0 / double(ts_freq_);
      uint64_t prev = ts_mapped_[l.first];
      const uint64_t t0 = prev;
      for (uint32_t i = 1; i < l.count; ++i) {
        const uint64_t t = ts_mapped_[(l.first + i) % kTsCount];
        const uint8_t c = l.cats[i - 1];
        if (t >= prev && i - 1 < l.tags.size() && l.tags[i - 1])  // patch 0101
          tag_times_.push_back({l.tags[i - 1], c, double(t - prev) * k});
        if (t >= prev && c < 8) stats_.gpu_cat_ms[c] += double(t - prev) * k;
        if (t >= prev && c >= 0x20 && c < 0x40) {  // job 234: draws into one render-target slot
          stats_.gpu_cat_ms[0] += double(t - prev) * k;
          stats_.gpu_target_ms[c - 0x20] += double(t - prev) * k;
        }
        prev = t;
      }
      if (prev > t0) stats_.gpu_ms += double(prev - t0) * k;
      ++stats_.gpu_lists_timed;
      ts_used_ -= l.count;
      ts_pending_.pop_front();
    }
    for (size_t i = 0; i < deferred_releases_.size();) {
      if (deferred_releases_[i].fence <= done) {
        ReleaseDeferred(deferred_releases_[i]);
        deferred_releases_[i] = deferred_releases_.back();
        deferred_releases_.pop_back();
      } else {
        ++i;
      }
    }
  }

  void DrainInfoQueue() {
    if (!info_queue_) return;
    const UINT64 n = info_queue_->GetNumStoredMessages();
    for (UINT64 i = 0; i < n && info_messages_logged_ < 500; ++i) {
      SIZE_T len = 0;
      if (FAILED(info_queue_->GetMessage(i, nullptr, &len)) || !len) continue;
      std::vector<uint8_t> buf(len);
      auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
      if (SUCCEEDED(info_queue_->GetMessage(i, m, &len)) && m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
        REXLOG_WARN("gow2_renderer: D3D12 debug: {}", std::string(m->pDescription, m->DescriptionByteLength ? m->DescriptionByteLength - 1 : 0));
        ++info_messages_logged_;
      }
    }
    info_queue_->ClearStoredMessages();
  }

  bool CreateBuffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                    ID3D12Resource** out, std::string& error) {
    D3D12_HEAP_PROPERTIES hp{type, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    // Buffers are always created in COMMON (the debug layer ignores any other InitialState, job 097); upload buffers are
    // then readable and readback buffers writable by copies through implicit promotion.
    (void)state;
    const HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                        nullptr, IID_PPV_ARGS(out));
    if (FAILED(hr)) {
      error = Hr("CreateCommittedResource(buffer)", hr);
      return false;
    }
    return true;
  }

  bool CreateDefaultTextures(std::string& error) {
    // 1x1 opaque white 2D, cube (6 faces) and 3D, filled by a copy from the upload ring.
    for (uint32_t kind = 0; kind < 3; ++kind) {
      D3D12_RESOURCE_DESC rd{};
      rd.Dimension = kind == 2 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      rd.Width = 1;
      rd.Height = 1;
      rd.DepthOrArraySize = kind == 1 ? 6 : 1;
      rd.MipLevels = 1;
      rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      rd.SampleDesc.Count = 1;
      D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
      const HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                          nullptr, IID_PPV_ARGS(&default_textures_[kind]));
      if (FAILED(hr)) {
        error = Hr("CreateCommittedResource(default texture)", hr);
        return false;
      }
    }
    ready_ = true;  // Recording() needs it
    if (!Recording()) {
      error = "could not open a command list";
      return false;
    }
    for (uint32_t kind = 0; kind < 3; ++kind) {
      const UINT subs = kind == 1 ? 6 : 1;
      for (UINT s = 0; s < subs; ++s) {
        uint64_t off;
        uint8_t* p = AllocUpload(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, off);
        if (!p) {
          error = "upload ring";
          return false;
        }
        std::memset(p, 0xFF, 4);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = default_textures_[kind];
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = s;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = upload_;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = off;
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
        TsCat();
        list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      }
      D3D12_RESOURCE_BARRIER b{};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = default_textures_[kind];
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      list_->ResourceBarrier(1, &b);
    }
    WaitForFence(Submit());
    ready_ = false;  // Init sets it again once everything else exists
    return !lost_;
  }

  bool CreateRootSignatures(std::string& error) {
    {
      D3D12_ROOT_PARAMETER params[kRootGraphicsCount] = {};
      for (UINT i = 0; i < 8; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[i].Descriptor.ShaderRegister = i & 3;
        params[i].Descriptor.RegisterSpace = 0;
        params[i].ShaderVisibility = i < 4 ? D3D12_SHADER_VISIBILITY_VERTEX : D3D12_SHADER_VISIBILITY_PIXEL;
      }
      D3D12_DESCRIPTOR_RANGE srv{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kTextureSrvCount, 0, 0, 0};
      params[kRootGuestSrv].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
      params[kRootGuestSrv].Descriptor.ShaderRegister = 96;
      params[kRootGuestSrv].Descriptor.RegisterSpace = 0;
      params[kRootGuestSrv].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      params[kRootUploadSrv].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
      params[kRootUploadSrv].Descriptor.ShaderRegister = 97;
      params[kRootUploadSrv].Descriptor.RegisterSpace = 0;
      params[kRootUploadSrv].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      params[kRootGuestUav].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
      params[kRootGuestUav].Descriptor.ShaderRegister = 0;
      params[kRootGuestUav].Descriptor.RegisterSpace = 0;
      params[kRootGuestUav].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      params[kRootSrvTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params[kRootSrvTable].DescriptorTable = {1, &srv};
      params[kRootSrvTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      D3D12_DESCRIPTOR_RANGE smp{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, kSamplerCount, 0, 0, 0};
      params[kRootSamplerTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params[kRootSamplerTable].DescriptorTable = {1, &smp};
      params[kRootSamplerTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      D3D12_ROOT_SIGNATURE_DESC rs{kRootGraphicsCount, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
      if (!SerializeRoot(rs, &graphics_root_, error)) return false;
    }
    {
      D3D12_ROOT_PARAMETER params[3] = {};
      params[kRootCsConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      params[kRootCsConstants].Constants = {0, 0, 16};
      params[kRootCsConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      D3D12_DESCRIPTOR_RANGE srv{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kComputeSrvCount, 0, 0, 0};
      params[kRootCsSrv].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params[kRootCsSrv].DescriptorTable = {1, &srv};
      params[kRootCsSrv].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      D3D12_DESCRIPTOR_RANGE uav{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 0};  // u0, u1 (job 047)
      params[kRootCsUav].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params[kRootCsUav].DescriptorTable = {1, &uav};
      params[kRootCsUav].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      D3D12_ROOT_SIGNATURE_DESC rs{3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
      if (!SerializeRoot(rs, &compute_root_, error)) return false;
    }
    return true;
  }

  bool SerializeRoot(const D3D12_ROOT_SIGNATURE_DESC& rs, ID3D12RootSignature** out, std::string& error) {
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) {
      error = Hr("D3D12SerializeRootSignature", hr);
      if (err) {
        error += std::string(": ") + static_cast<const char*>(err->GetBufferPointer());
        err->Release();
      }
      return false;
    }
    if (out == &graphics_root_) graphics_root_hash_ = XXH3_64bits(blob->GetBufferPointer(), blob->GetBufferSize());
    hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(out));
    blob->Release();
    if (FAILED(hr)) {
      error = Hr("CreateRootSignature", hr);
      return false;
    }
    return true;
  }

  BackendOptions options_;
  std::string description_;
  bool ready_ = false, lost_ = false;
  bool loss_reported_ = false;  // patch 0006
  std::string lost_reason_;
  ID3D12Device* device_ = nullptr;
  IDXGIAdapter3* adapter3_ = nullptr;  // patch 0032
  ID3D12InfoQueue* info_queue_ = nullptr;
  uint32_t info_messages_logged_ = 0;
  ID3D12CommandQueue* queue_ = nullptr;
  ID3D12Fence* fence_ = nullptr;
  HANDLE fence_event_ = nullptr;
  HANDLE occ_event_ = nullptr;
  uint64_t occ_event_fence_ = 0;
  uint64_t next_fence_ = 1, last_submitted_ = 0;
  ID3D12GraphicsCommandList* real_list_ = nullptr;
  CommandListProxy proxy_;
  CommandListProxy* const list_ = &proxy_;  // every command-list call goes through the proxy (immediate or threaded)
  bool threaded_recording_ = false;
  CopyPool copies_;  // job 171
  // Occlusion queries (upl-s4c): one heap of slots, results resolved into a persistently mapped readback buffer.
  static constexpr uint32_t kOcclusionSlots = 16384, kOcclusionMaxParts = 16;
  struct OcclusionQuery {
    bool in_use = false, ended = false, broken = false;
    uint32_t parts = 0;
    uint32_t slot[kOcclusionMaxParts] = {};
  };
  static constexpr uint32_t kTsCount = 32768, kTsPerList = 4096;
  ID3D12QueryHeap* ts_heap_ = nullptr;
  ID3D12Resource* ts_readback_ = nullptr;
  const uint64_t* ts_mapped_ = nullptr;
  UINT64 ts_freq_ = 0;
  uint32_t ts_head_ = 0, ts_used_ = 0;
  bool ts_open_ = false, ts_failed_ = false;
  uint8_t gpu_cat_pending_ = 0, gpu_cat_emitted_ = 0xFF;
  uint32_t gpu_tag_pending_ = 0, gpu_tag_emitted_ = 0;  // patch 0101
  std::vector<GpuTagTime> tag_times_;
  struct TsList {
    uint64_t fence = 0;
    uint32_t first = 0, count = 0;
    std::vector<uint8_t> cats;  // category of the time from mark i to mark i + 1
    std::vector<uint32_t> tags;  // patch 0101: the SetGpuTag value of each stretch
  };
  TsList ts_cur_;
  std::deque<TsList> ts_pending_;
  ID3D12QueryHeap* occ_heap_ = nullptr;
  ID3D12Resource* occ_readback_ = nullptr;
  const uint64_t* occ_mapped_ = nullptr;
  bool occ_failed_ = false;
  std::vector<uint64_t> occ_slot_fence_;  // submission that last resolved the slot (0 = not submitted yet)
  std::vector<uint8_t> occ_slot_held_;    // held by a live query
  uint32_t occ_cursor_ = 0;
  std::vector<OcclusionQuery> occ_queries_;
  std::vector<OcclusionHandle> occ_free_handles_;
  OcclusionHandle occ_open_ = 0;
  bool occ_open_in_list_ = false;
  std::vector<uint32_t> occ_slots_in_list_;  // slots resolved in the open list (fenced at Submit)
  bool list_open_ = false;
  ID3D12CommandAllocator* current_allocator_ = nullptr;
  std::vector<FencedAllocator> allocators_;
  ID3D12RootSignature* graphics_root_ = nullptr;
  ID3D12RootSignature* compute_root_ = nullptr;
  ID3D12DescriptorHeap* shader_heap_ = nullptr;

  UINT view_increment_ = 0;
  uint32_t ring_head_ = 0, ring_tail_ = 0;
  std::deque<Marker> ring_markers_;
  CpuHeap rtv_heap_, dsv_heap_, view_heap_, default_table_heap_;
  UINT sampler_increment_ = 0;
  struct SamplerTable {
    bool used = false;
    uint64_t hash = 0;
    std::vector<SamplerDesc> descs;
    uint64_t fence = 0;  // last submission that used it (0 = only the open list or never)
    uint32_t offset = 0, size = 32;  // first heap slot, samplers written
    bool in_open_list = false;
  };
  struct SamplerPool {
    ID3D12DescriptorHeap* heap = nullptr;
    std::vector<SamplerTable> tables;            // [0] = the static default table
    std::unordered_map<uint64_t, uint32_t> map;  // content hash -> table
    uint64_t fence = 0;                          // last submission that used this heap
    bool in_list = false;                        // patch 0034: bound in the open command list
  };
  SamplerPool sampler_pools_[kSamplerPools];
  struct TextureTable {
    uint32_t base = 0, count = 0;
    uint64_t at = 0;  // ring_alloc_total_ when written
    std::vector<TextureView> views;
    std::vector<uint64_t> serials;
  };
  std::unordered_map<uint64_t, TextureTable> texture_tables_;
  uint64_t ring_alloc_total_ = 0, texture_serial_ = 0;
  uint32_t cur_pool_ = 0;
  SamplerPool& Pool() { return sampler_pools_[cur_pool_]; }
  struct IndexBuffer {
    ID3D12Resource* resource = nullptr;
    uint32_t count = 0;
  };
  std::vector<IndexBuffer> index_buffers_;
  std::vector<uint32_t> free_index_buffers_;

  // Current texture bindings (null = static tables), re-applied lazily (PrepareDraw) and after a new command list.
  bool textures_custom_ = false, textures_dirty_ = false;
  TextureView bound_views_[96];
  uint32_t bound_view_count_ = 96;
  SamplerDesc bound_samplers_[32];
  uint32_t bound_sampler_count_ = 32;
  bool bound_bits_ = false;
  std::vector<std::pair<uint32_t, uint32_t>> tables_in_open_list_;  // (pool, table); patch 0034: pools change mid-list
  ID3D12Resource* upload_ = nullptr;
  uint8_t* upload_mapped_ = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS upload_gpu_ = 0;
  uint64_t upload_size_ = 0, upload_head_ = 0, upload_tail_ = 0;
  std::deque<Marker> upload_markers_;
  ID3D12Resource* guest_ = nullptr;
  int32_t guest_uav_ = -1;  // T2: view_heap_ slot of the guest buffer's raw UAV
  // Patch 0021: scratch buffer for GPU loads into BCn textures, its raw UAV slot, state and the pending copy layout.
  ID3D12Resource* scratch_ = nullptr;
  UINT64 scratch_size_ = 0;
  int32_t scratch_uav_ = -1;
  D3D12_RESOURCE_STATES scratch_state_ = D3D12_RESOURCE_STATE_COMMON;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT scratch_fp_{};
  UINT scratch_sub_ = UINT_MAX;
  // Batched texture loads: the next free byte of the scratch buffer and the copies queued out of it.
  struct ScratchCopy {
    TextureHandle texture = 0;
    uint64_t serial = 0;
    UINT subresource = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  };
  UINT64 scratch_head_ = 0;
  std::vector<ScratchCopy> scratch_copies_;
  int32_t null_views_[3] = {-1, -1, -1};
  uint64_t lost_checked_ms_ = 0;  // IsLost: last GetDeviceRemovedReason
  uint64_t completed_ = 0;        // Retire: last fence value read  // view_heap_ slots of null 2D / cube / 3D views (TextureView::zero)
  D3D12_RESOURCE_STATES guest_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  bool guest_writable_ = false;  // the next draws memexport into the guest-memory buffer (SetGuestWritable)
  ID3D12Resource* default_textures_[3] = {};
  std::vector<Texture> textures_;
  std::vector<Shader> shaders_;
  std::deque<Pipeline> pipelines_;  // deque: workers fill slots while the recording thread appends
  // Asynchronous pipeline creation (upl-n108).
  bool StartPipelineWorkers() {
    if (!workers_.empty()) return true;
    // Patch 0056: Canary's count (75% of the logical processors) unless set; the old count was min(4, a quarter).
    const uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
    const int want = options_.pipeline_threads;
    const uint32_t n = want < 0    ? std::max(1u, hw * 3 / 4)
                       : want == 0 ? std::max(1u, std::min(4u, hw / 4))
                                   : std::clamp(uint32_t(want), 1u, hw);
    // Patch 0162: below normal priority by default, so a burst of builds can't starve the game's own threads or the audio.
    const int prio = std::clamp(options_.pipeline_thread_priority, -2, 0);
    for (uint32_t i = 0; i < n; ++i)
      workers_.emplace_back([this, prio] {
        if (prio != 0) SetThreadPriority(GetCurrentThread(), prio == -1 ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_LOWEST);
        PipelineWorker();
      });
    REXLOG_INFO("gow2_renderer: pipeline workers: {} thread(s) ({} logical processors), priority {}", n, hw,
                prio == 0 ? "normal" : prio == -1 ? "below normal" : "lowest");  // patch 0056 / 0162
    return !workers_.empty();
  }
  void PipelineWorker() {
    for (;;) {
      std::unique_ptr<PipelineJob> job;
      {
        std::unique_lock<std::mutex> lock(job_mutex_);
        job_cv_.wait(lock, [this] { return stop_workers_ || JobsQueued(); });
        if (stop_workers_ && !JobsQueued()) return;
        for (uint32_t p = kPipelinePriorities; p-- > 0;) {  // patch 0057: highest priority first, FIFO within one
          // Patch 0068: warm-only jobs after every live request, before the start-up backlog.
          // Patch 0069: and only while fewer than warm_workers_ workers are on warm jobs.
          if (p == kPipelineClasses - 1 && !warm_jobs_.empty() && warm_busy_ < warm_workers_) {
            job = std::move(warm_jobs_.front());
            warm_jobs_.pop_front();
            break;
          }
          if (jobs_[p].empty()) continue;
          job = std::move(jobs_[p].front());
          jobs_[p].pop_front();
          break;
        }
        // Patch 0061: under job_mutex_, so PipelineJobsLeft never sees a job in neither place (0068: warm jobs apart).
        ++(job->target ? jobs_busy_ : warm_busy_);
      }
      struct BusyDone {  // patch 0061: whatever path the job leaves by
        D3D12Backend* b;
        uint64_t* counter;
        ~BusyDone() {
          {
            std::lock_guard<std::mutex> l(b->job_mutex_);
            --*counter;
          }
          if (counter == &b->warm_busy_) { b->job_cv_.notify_one(); b->warm_capacity_cv_.notify_all(); }  // patch 0069: a warm slot is free again
        }
      } busy_done{this, job->target ? &jobs_busy_ : &warm_busy_};
      if (!job->target) {  // warm-only: load or create the stage pipeline, persist it, then release the PSO
        // Prebuild v2: a speculative build runs at its own (lower) priority, so it takes CPU time only the game, the
        // renderer, the audio and the draws' own pipelines leave free; the worker's priority is restored after.
        const int base_prio = std::clamp(options_.pipeline_thread_priority, -2, 0);
        const int warm_prio = warm_startup_.load(std::memory_order_relaxed) ? base_prio
                              : std::clamp(options_.warm_thread_priority, -2, 0);
        struct PrioScope {
          int from, to;
          static int Win(int p) { return p == 0 ? THREAD_PRIORITY_NORMAL : p == -1 ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_LOWEST; }
          PrioScope(int f, int t) : from(f), to(t) { if (to != from) SetThreadPriority(GetCurrentThread(), Win(to)); }
          ~PrioScope() { if (to != from) SetThreadPriority(GetCurrentThread(), Win(from)); }
        } prio_scope{base_prio, warm_prio};
        if (job->stage_prefetch) {
          SeedWarmStages(*job);
          continue;
        }
        const auto w0 = std::chrono::steady_clock::now();
        ID3D12PipelineState* warm = nullptr;
        HRESULT whr = E_FAIL;
        const std::wstring& name = job->library_name;
        whr = LoadLibraryPipeline(name, job->desc, &warm, true);
        bool persisted = SUCCEEDED(whr);
        if (SUCCEEDED(whr)) warm_loaded_.fetch_add(1, std::memory_order_relaxed);
        if (FAILED(whr)) {
          whr = device_->CreateGraphicsPipelineState(&job->desc, IID_PPV_ARGS(&warm));
          if (SUCCEEDED(whr)) {
            ID3DBlob* blob = nullptr;
            if (SUCCEEDED(warm->GetCachedBlob(&blob))) {
              persisted = warm_stages_.Put(name, blob->GetBufferPointer(), blob->GetBufferSize());
              blob->Release();
            }
            if (!persisted && warm_save_failed_.fetch_add(1) < 4)
              REXLOG_WARN("gow2_renderer: could not save compiled stage; startup will retry it next launch");
          }
        }
        warm_us_.fetch_add(uint64_t(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - w0).count()),
                           std::memory_order_relaxed);
        if (SUCCEEDED(whr)) {
          warm->Release();
          {
            std::lock_guard<std::mutex> lock(job_mutex_);
            warm_success_names_.insert(name);
            if (auto it = warm_source_pending_.find(name); it != warm_source_pending_.end()) {
              if (persisted) for (const auto& key : it->second) warm_sources_.Remember(key, name);
              warm_source_pending_.erase(it);
            }
          }
          warmed_.fetch_add(1, std::memory_order_relaxed);
        } else if (warm_failed_.fetch_add(1, std::memory_order_relaxed) < 4) {
          REXLOG_WARN("gow2_renderer: {} (warm-only pipeline, patch 0068)", Hr("CreateGraphicsPipelineState", whr));
        }
        continue;
      }
      ID3D12PipelineState* pso = nullptr;
      Pipeline& p = *job->target;
      // Patch 0010: a pipeline already saved loads from the library here, off the recording thread. Each pipeline has
      // one job, so no two threads load the same one (the one case the library does not synchronise itself).
      if (pipeline_library_ && options_.pipeline_load_async && !job->library_name.empty() &&
          SUCCEEDED(LoadLibraryPipeline(job->library_name, job->desc, &pso))) {
        p.pso = pso;
        loaded_async_.fetch_add(1, std::memory_order_relaxed);
        p.ready.store(true, std::memory_order_release);
        continue;
      }
      SeedWarmStages(*job);
      const auto t0 = std::chrono::steady_clock::now();
      const HRESULT hr = device_->CreateGraphicsPipelineState(&job->desc, IID_PPV_ARGS(&pso));
      {  // patch 0057: how long the driver takes per new pipeline, and for how much bytecode
        const uint64_t us = uint64_t(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
        built_async_.fetch_add(1, std::memory_order_relaxed);
        async_us_.fetch_add(us, std::memory_order_relaxed);
        uint64_t bytes = 0;
        for (const auto& c : job->code) bytes += c ? c->size() : 0;
        async_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        built_by_priority_[job->priority % kPipelineClasses].fetch_add(1, std::memory_order_relaxed);
        uint64_t m = async_max_us_.load(std::memory_order_relaxed);
        while (us > m && !async_max_us_.compare_exchange_weak(m, us, std::memory_order_relaxed)) {}
      }
      if (SUCCEEDED(hr)) {
        p.pso = pso;
        StorePipeline(job->library_name, pso);
      } else {
        p.failed.store(true);
        REXLOG_ERROR("gow2_renderer: {} (asynchronous)", Hr("CreateGraphicsPipelineState", hr));
      }
      p.ready.store(true, std::memory_order_release);
    }
  }
  void StopPipelineWorkers() {
    {
      std::lock_guard<std::mutex> lock(job_mutex_);
      stop_workers_ = true;
      warm_jobs_.clear();  // patch 0068: warm-only work is dropped at shutdown, not finished
      // Patch 0149 (deep-dive review): so is everything still queued - the workers used to build the whole start-up
      // backlog before quitting (seconds to minutes when the game closed soon after start). Running jobs finish.
      for (auto& q : jobs_) q.clear();
    }
    job_cv_.notify_all();
    for (std::thread& t : workers_) t.join();
    workers_.clear();
  }
  // ---- Saved pipelines (notes/64): an ID3D12PipelineLibrary1 in options_.pipeline_library_path ----
  // The file must stay in memory while the library lives (the library reads from it), so it is kept in
  // pipeline_library_blob_. Names: XXH3-128 of the root signature, the shader bytecode and the fixed-function state of
  // the description (pointers zeroed), so the same pipeline gets the same name in every run of the same build.
  void OpenPipelineLibrary() {
    LoadPipelineLibrary(std::filesystem::path(options_.pipeline_library_path), pipeline_library_, pipeline_library_blob_);
    pipeline_library_saved_at_ = std::chrono::steady_clock::now();
  }
  void LoadPipelineLibrary(const std::filesystem::path& path, ID3D12PipelineLibrary1*& library,
                           std::vector<uint8_t>& blob, cogbox::io::MappedFile* mapped = nullptr,
                           std::atomic<bool>* existing_valid = nullptr) {
    ID3D12Device1* device1 = nullptr;
    if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&device1)))) {
      REXLOG_WARN("gow2_renderer: saved pipelines: the device has no ID3D12Device1; off");
      return;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const void* data = nullptr;
    size_t had = 0;
    if (mapped && mapped->Open(path)) { data = mapped->Data(); had = mapped->Size(); }
    else if (!mapped) {
      std::ifstream f(path, std::ios::binary | std::ios::ate);
      if (f && f.tellg() > 0) {
        blob.resize(size_t(f.tellg())); f.seekg(0);
        if (f.read(reinterpret_cast<char*>(blob.data()), std::streamsize(blob.size()))) {
          data = blob.data(); had = blob.size();
        } else blob.clear();
      }
    }
    HRESULT hr = had ? device1->CreatePipelineLibrary(data, had, IID_PPV_ARGS(&library)) : E_FAIL;
    if (existing_valid) *existing_valid = had && SUCCEEDED(hr);
    if (FAILED(hr)) {  // no file yet, another driver / GPU (DRIVER_VERSION_MISMATCH, ADAPTER_NOT_FOUND) or damaged
      if (had) REXLOG_INFO("gow2_renderer: saved pipelines: {} not usable ({:08X}) - starting a new library",
                           path.string(), uint32_t(hr));
      blob.clear();
      if (mapped) mapped->Reset();
      hr = device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library));
    }
    device1->Release();
    if (FAILED(hr)) {
      REXLOG_WARN("gow2_renderer: saved pipelines: CreatePipelineLibrary failed ({:08X}); off", uint32_t(hr));
      library = nullptr;
      return;
    }
    REXLOG_INFO("gow2_renderer: saved pipelines: {} ({} KB loaded)", path.string(),
                SUCCEEDED(hr) ? had / 1024 : 0);
  }
  std::wstring PipelineLibraryName(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& pd) const {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC fixed = pd;  // value copy; pointer members cleared below (padding stays zero:
    fixed.pRootSignature = nullptr;                 // every description is value-initialised before it is filled)
    fixed.VS = fixed.PS = fixed.GS = fixed.DS = fixed.HS = D3D12_SHADER_BYTECODE{};
    fixed.StreamOutput = D3D12_STREAM_OUTPUT_DESC{};
    fixed.InputLayout = D3D12_INPUT_LAYOUT_DESC{};
    fixed.CachedPSO = D3D12_CACHED_PIPELINE_STATE{};
    XXH3_state_t* st = XXH3_createState();
    XXH3_128bits_reset(st);
    XXH3_128bits_update(st, &graphics_root_hash_, sizeof(graphics_root_hash_));
    XXH3_128bits_update(st, &fixed, sizeof(fixed));
    for (const D3D12_SHADER_BYTECODE* b : {&pd.VS, &pd.PS, &pd.GS}) {
      const uint64_t n = b->BytecodeLength;
      XXH3_128bits_update(st, &n, sizeof(n));
      if (n) XXH3_128bits_update(st, b->pShaderBytecode, size_t(n));
    }
    const XXH128_hash_t h = XXH3_128bits_digest(st);
    XXH3_freeState(st);
    wchar_t name[40];
    swprintf(name, 40, L"gp%016llx%016llx", (unsigned long long)h.high64, (unsigned long long)h.low64);
    return name;
  }
  void SeedWarmStages(const PipelineJob& job) {
    if (!warm_ready_.load(std::memory_order_acquire) || !warm_sources_.Size()) return;
    {
      std::lock_guard<std::mutex> lock(warm_lifecycle_mutex_);
      if (!warm_pipeline_library_ && warm_library_valid_)
        LoadPipelineLibrary(WarmLibraryPath(), warm_pipeline_library_, warm_library_blob_, &warm_library_map_);
    }
    if (!warm_pipeline_library_) return;
    for (bool vertex : {true, false}) {
      const auto& code = job.code[vertex ? 0 : 1];
      if (!code || code->empty()) continue;
      auto pd = warm_pd_;
      if (vertex) {
        pd.VS = {code->data(), code->size()}; pd.PS = {}; pd.NumRenderTargets = 0;
        for (auto& format : pd.RTVFormats) format = DXGI_FORMAT_UNKNOWN;
      } else pd.PS = {code->data(), code->size()};
      const auto name = PipelineLibraryName(pd);
      if (!warm_sources_.HasPipeline(name)) continue;
      // Another live pipeline may need this same stage. Wait off the render
      // thread until its restore finishes instead of racing into a cold compile.
      std::lock_guard<std::mutex> seed_lock(warm_seed_mutexes_[std::hash<std::wstring>{}(name) % warm_seed_mutexes_.size()]);
      {
        std::lock_guard<std::mutex> lock(job_mutex_);
        if (live_seeded_.count(name)) continue;
      }
      ID3D12PipelineState* stage = nullptr;
      const auto restored_at = std::chrono::steady_clock::now();
      if (SUCCEEDED(LoadLibraryPipeline(name, pd, &stage, true))) {
        stage->Release();
        stage_restores_.fetch_add(1, std::memory_order_relaxed);
        stage_restore_us_.fetch_add(uint64_t(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now() - restored_at).count()), std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(job_mutex_);
        live_seeded_.insert(name);
      }
    }
  }
  HRESULT LoadLibraryPipeline(const std::wstring& name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                              ID3D12PipelineState** out, bool warm = false) {
    if (warm) {
      std::vector<uint8_t> bytes;
      if (warm_stages_.Get(name, bytes)) {
        auto cached = desc;
        cached.CachedPSO = {bytes.data(), bytes.size()};
        const HRESULT hr = device_->CreateGraphicsPipelineState(&cached, IID_PPV_ARGS(out));
        if (SUCCEEDED(hr)) return hr;
      }
    }
    ID3D12PipelineLibrary1* library = warm ? warm_pipeline_library_ : pipeline_library_;
    if (!library || name.empty()) return E_FAIL;
    // D3D12 requires synchronization for loads of the same PSO. Striped locks retain parallel
    // loading for unrelated keys and never make a live lookup wait for cache serialization.
    std::lock_guard<std::mutex> lock(pipeline_load_mutexes_[std::hash<std::wstring>{}(name) % pipeline_load_mutexes_.size()]);
    return library->LoadGraphicsPipeline(name.c_str(), &desc, IID_PPV_ARGS(out));
  }
  void StorePipeline(const std::wstring& name, ID3D12PipelineState* pso, bool warm = false) {
    ID3D12PipelineLibrary1* library = warm ? warm_pipeline_library_ : pipeline_library_;
    if (!library || name.empty() || !pso) return;
    std::lock_guard<std::mutex> lock(warm ? warm_library_mutex_ : pipeline_library_mutex_);
    const HRESULT hr = library->StorePipeline(name.c_str(), pso);
    if (SUCCEEDED(hr)) {
      if (!warm) stored_pipelines_.fetch_add(1, std::memory_order_relaxed);
      else warm_library_valid_ = false;
      (warm ? warm_library_dirty_ : pipeline_library_dirty_).store(true, std::memory_order_relaxed);
    }
  }
  // Patch 0010: also called on the background saver thread (on_recording_thread = false): then it leaves the stats and
  // the save time to the recording thread and reports success by its result.
  bool SavePipelineLibrary(bool on_recording_thread = true, bool warm = false) {
    ID3D12PipelineLibrary1* library = warm ? warm_pipeline_library_ : pipeline_library_;
    auto& dirty = warm ? warm_library_dirty_ : pipeline_library_dirty_;
    std::vector<uint8_t> out;
    {
      // Only the snapshot is taken under the lock (workers store new pipelines meanwhile); the file is written after.
      std::lock_guard<std::mutex> lock(warm ? warm_library_mutex_ : pipeline_library_mutex_);
      if (on_recording_thread) pipeline_library_saved_at_ = std::chrono::steady_clock::now();
      if (!library || !dirty.exchange(false)) return false;
      const SIZE_T size = library->GetSerializedSize();
      out.resize(size);
      if (!size || FAILED(library->Serialize(out.data(), size))) {
        REXLOG_WARN("gow2_renderer: saved pipelines: Serialize failed ({} bytes)", size_t(size));
        dirty.store(true);
        return false;
      }
    }
    if (warm) {
      SafeRelease(warm_pipeline_library_);
      warm_library_map_.Reset();
      std::vector<uint8_t>().swap(warm_library_blob_);
    }
    const std::filesystem::path path = warm ? WarmLibraryPath() : std::filesystem::path(options_.pipeline_library_path);
    std::filesystem::path tmp = path;
    tmp += L".tmp";
    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
      if (!f) { dirty.store(true); REXLOG_WARN("gow2_renderer: could not write shader pipeline cache {}", tmp.string()); return false; }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);  // whole file or nothing: a crash mid-write leaves the old one
    if (ec) {
      REXLOG_WARN("gow2_renderer: could not replace shader pipeline cache {}: {}", path.string(), ec.message());
      std::filesystem::remove(tmp, ec);
      dirty.store(true);
      return false;
    }
    if (on_recording_thread) ++stats_.pipeline_library_saves;
    if (warm) warm_library_valid_ = true;
    return true;
  }
  cogbox::shader_cache::WarmSourceCache warm_sources_;
  cogbox::shader_cache::WarmStageStore warm_stages_;
  bool WarmSourceStamp(cogbox::shader_cache::LibraryStamp& stamp) const {
    return cogbox::shader_cache::WarmSourceCache::Stamp(WarmLibraryPath(), warm_source_identity_, stamp) &&
           warm_stages_.Stamp(stamp);
  }
  std::string warm_source_identity_;
  std::atomic<bool> warm_library_valid_{false};
  std::atomic<uint64_t> warm_source_hits_{0};
  std::atomic<uint64_t> warm_save_failed_{0};
  std::atomic<uint64_t> stage_prefetches_{0}, stage_restores_{0}, stage_restore_us_{0};
  std::mutex warm_lifecycle_mutex_;
  std::unordered_set<std::wstring> warm_success_names_, live_seeded_;
  std::unordered_set<std::wstring> stage_prefetch_seen_;
  std::unordered_map<std::wstring, std::vector<cogbox::shader_cache::SourceKey>> warm_source_pending_;
  std::filesystem::path WarmLibraryPath() const { return options_.pipeline_library_path + ".prebuild"; }
  ID3D12PipelineLibrary1* warm_pipeline_library_ = nullptr;
  cogbox::io::MappedFile warm_library_map_;
  std::vector<uint8_t> warm_library_blob_;  // retained for the library's entire lifetime
  std::mutex warm_library_mutex_;
  std::atomic<bool> warm_library_dirty_{false};
  std::array<std::mutex, 64> pipeline_load_mutexes_;
  GraphicsPipelineDesc last_pipeline_desc_;  // GetGraphicsPipeline's one-entry memo (renderer thread only)
  PipelineHandle last_pipeline_ = 0;
  std::array<std::mutex, 64> warm_seed_mutexes_;
  ID3D12PipelineLibrary1* pipeline_library_ = nullptr;
  std::vector<uint8_t> pipeline_library_blob_;
  std::mutex pipeline_library_mutex_;
  std::atomic<bool> pipeline_library_dirty_{false};
  std::atomic<uint64_t> stored_pipelines_{0};
  std::chrono::steady_clock::time_point pipeline_library_saved_at_{};
  // Patch 0010: background saves and asynchronous library loads.
  std::thread saver_;
  std::atomic<bool> saving_{false};
  std::atomic<uint64_t> background_saves_{0};
  std::atomic<uint64_t> loaded_async_{0};
  // Patch 0057: worker creation timing (Stats::pipelines_built_async ...).
  std::atomic<uint64_t> built_async_{0}, async_us_{0}, async_max_us_{0}, async_bytes_{0};
  std::atomic<uint64_t> built_by_priority_[kPipelineClasses] = {};
  // Patch 0058.
  uint64_t pipeline_misses_ = 0;
  bool pipeline_backlog_ = false;
  uint64_t PipelineMisses() const override { return pipeline_misses_; }
  uint64_t jobs_busy_ = 0;  // patch 0061: jobs taken by a worker and not finished (under job_mutex_)
  uint64_t PipelineJobsLeft() override {  // patch 0061
    std::lock_guard<std::mutex> lock(job_mutex_);
    uint64_t q = jobs_busy_;
    for (const auto& d : jobs_) q += d.size();
    return q;
  }
  void RefreshWorkerStats() {  // patch 0057 (moved here by 0061: also for the prepare-only report)
    stats_.cached_stage_prefetches = stage_prefetches_.load(std::memory_order_relaxed);
    stats_.cached_stage_restores = stage_restores_.load(std::memory_order_relaxed);
    stats_.cached_stage_restore_ms = double(stage_restore_us_.load(std::memory_order_relaxed)) / 1000.0;
    stats_.pipelines_stored = stored_pipelines_.load(std::memory_order_relaxed);
    stats_.pipelines_loaded_async = loaded_async_.load(std::memory_order_relaxed);
    stats_.pipelines_built_async = built_async_.load(std::memory_order_relaxed);
    stats_.pipeline_async_ms = double(async_us_.load(std::memory_order_relaxed)) / 1000.0;
    stats_.pipeline_async_max_ms = double(async_max_us_.load(std::memory_order_relaxed)) / 1000.0;
    stats_.pipeline_async_bytes = async_bytes_.load(std::memory_order_relaxed);
    stats_.pipelines_warmed = warmed_.load(std::memory_order_relaxed);  // patch 0068
    stats_.pipelines_warm_loaded = warm_loaded_.load(std::memory_order_relaxed);
    stats_.pipelines_warm_failed = warm_failed_.load(std::memory_order_relaxed);
    stats_.pipeline_warm_ms = double(warm_us_.load(std::memory_order_relaxed)) / 1000.0;
    for (uint32_t i = 0; i < kPipelineClasses; ++i)
      stats_.pipelines_built_by_priority[i] = built_by_priority_[i].load(std::memory_order_relaxed);
    if (!workers_.empty()) {
      std::lock_guard<std::mutex> lock(job_mutex_);
      uint64_t q = 0;
      for (const auto& d : jobs_) q += d.size();
      stats_.pipeline_jobs_queued = q;
    }
  }
  void SavePipelinesNow() override {  // patch 0061
    RefreshWorkerStats();
    if (saver_.joinable()) saver_.join();
    if (pipeline_library_ && pipeline_library_dirty_.load()) SavePipelineLibrary();
    if (warm_pipeline_library_ && warm_library_dirty_.load()) SavePipelineLibrary(false, true);
    if (warm_library_valid_ && !warm_source_identity_.empty()) {
      cogbox::shader_cache::LibraryStamp stamp;
      if (WarmSourceStamp(stamp) &&
          !warm_sources_.Save(WarmLibraryPath().wstring() + L".sources", stamp))
        REXLOG_WARN("gow2_renderer: could not save the compiled source inventory");
    }
  }
  void SetPipelineBacklog(bool on) override { pipeline_backlog_ = on; }
  bool PipelinePending(const GraphicsPipelineDesc& d) override {  // patch 0063 (the renderer's load hold)
    auto it = pipeline_cache_.find(HashPipelineDesc(d));
    if (it == pipeline_cache_.end()) return false;
    for (const auto& [desc, handle] : it->second) {
      if (!(desc == d)) continue;
      if (!handle) return false;
      Pipeline& p = pipelines_[handle];
      if (p.ready.load(std::memory_order_acquire)) return false;
      PromoteBacklogJob(handle, p);
      return true;
    }
    return false;
  }
  // Patch 0062 (Shagg: "There doesn't need to be a prepare shaders button"): the start-up backlog holds every
  // pipeline recorded on this PC, all chapters, in the order they were first seen. When a draw needs one of them before
  // the workers got to it, that job moves from its backlog queue to the live queue of its class (the same place a new
  // pipeline would go), so the area being played comes first and the rest keeps building behind it. Once per pipeline.
  // Patch 0132: wait up to max_ms for a pipeline already queued or being built by a worker - never builds it here.
  bool WaitGraphicsPipeline(const GraphicsPipelineDesc& d, double max_ms) override {
    if (!ready_ || lost_ || max_ms <= 0.0) return false;
    auto it = pipeline_cache_.find(HashPipelineDesc(d));
    if (it == pipeline_cache_.end()) return false;
    for (const auto& [desc, handle] : it->second) {
      if (!(desc == d) || !handle) continue;
      Pipeline& p = pipelines_[handle];
      if (!pipeline_backlog_) PromoteBacklogJob(handle, p);
      const auto t0 = std::chrono::steady_clock::now();
      while (!p.ready.load(std::memory_order_acquire)) {
        if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() >= max_ms) return false;
        std::this_thread::yield();
      }
      return !p.failed.load();
    }
    return false;
  }
  void PromoteBacklogJob(PipelineHandle handle, Pipeline& p) {
    const uint8_t c = p.backlog_class.exchange(0xFF);
    if (c >= kPipelineClasses) return;  // not a waiting backlog job (or already moved)
    std::lock_guard<std::mutex> lock(job_mutex_);
    auto& from = jobs_[c];
    for (auto it = from.begin(); it != from.end(); ++it) {
      if ((*it)->handle != handle) continue;
      std::unique_ptr<PipelineJob> job = std::move(*it);
      from.erase(it);
      job->priority = uint8_t(c + kPipelineClasses);
      jobs_[job->priority].push_back(std::move(job));
      ++stats_.pipelines_promoted;
      return;
    }
    // Not found: a worker already took it (it is being built now).
  }
  uint64_t loaded_async_counted_ = 0;
  uint64_t graphics_root_hash_ = 0;

  std::vector<std::thread> workers_;
  std::deque<std::unique_ptr<PipelineJob>> jobs_[kPipelinePriorities];  // patch 0057: by PipelinePriority
  bool JobsQueued() const {  // under job_mutex_
    for (const auto& q : jobs_)
      if (!q.empty()) return true;
    return !warm_jobs_.empty() && warm_busy_ < warm_workers_;  // patch 0068 (0069: only while a warm slot is free)
  }
  // Patch 0068 (gpu.h SetWarmTemplate / WarmPixelShader): warm-only pipelines.
  std::deque<std::unique_ptr<PipelineJob>> warm_jobs_;  // under job_mutex_
  std::unordered_set<std::wstring> warm_seen_;  // identical final bytecode/state needs only one driver build
  std::condition_variable warm_capacity_cv_;
  std::atomic<bool> warm_startup_{false};
  uint64_t warm_busy_ = 0;                              // under job_mutex_
  uint64_t warm_workers_ = 3;                           // patch 0069 (SetWarmTemplate); prebuild v2: SetWarmWorkers
  bool warm_workers_set_ = false;                       // prebuild v2: SetWarmWorkers was called (under job_mutex_)
  D3D12_GRAPHICS_PIPELINE_STATE_DESC warm_pd_{};        // written once, before warm_ready_
  std::shared_ptr<const std::vector<uint8_t>> warm_vs_;
  std::atomic<bool> warm_ready_{false};
  std::atomic<uint64_t> warmed_{0}, warm_failed_{0}, warm_us_{0}, warm_loaded_{0};
  bool SetWarmTemplate(const GraphicsPipelineDesc& d) override {
    if (warm_ready_.load(std::memory_order_acquire)) return true;
    {
      std::lock_guard<std::mutex> lock(job_mutex_);
      if (!warm_workers_set_) warm_workers_ = std::max<uint64_t>(1, options_.warm_workers);  // patch 0069 (v2: unless set)
    }
    if (!ready_ || lost_ || !d.vs || d.vs >= shaders_.size() || !shaders_[d.vs].code || d.gs) return false;
    if (!options_.pipeline_library_path.empty())
      LoadPipelineLibrary(WarmLibraryPath(), warm_pipeline_library_, warm_library_blob_, &warm_library_map_, &warm_library_valid_);
    if (!StartPipelineWorkers()) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    GraphicsPipelineDesc t = d;
    t.ps = 0;
    BuildPipelineDesc(t, pd, topology);
    warm_vs_ = shaders_[d.vs].code;
    pd.VS = {warm_vs_->data(), warm_vs_->size()};
    pd.PS = {};
    warm_pd_ = pd;
    warm_ready_.store(true, std::memory_order_release);
    REXLOG_INFO("gow2_renderer: warm-only pipelines ready (patch 0068): vertex shader {}, {} colour target(s)", d.vs,
                d.color_count);
    return true;
  }
  bool WarmPixelShader(std::vector<uint8_t>&& ps_bytecode, uint64_t source_hash = 0) override {
    if (!warm_ready_.load(std::memory_order_acquire) || ps_bytecode.empty()) return false;
    auto job = std::make_unique<PipelineJob>();
    job->desc = warm_pd_;
    job->code[0] = warm_vs_;
    job->code[1] = std::make_shared<const std::vector<uint8_t>>(std::move(ps_bytecode));
    job->desc.PS = {job->code[1]->data(), job->code[1]->size()};
    return QueueWarmPipeline(std::move(job), source_hash, false);
  }
  bool PrefetchShaderStage(ShaderHandle shader) override {
    if (!warm_ready_.load(std::memory_order_acquire) || !shader || shader >= shaders_.size() || !shaders_[shader].code)
      return false;
    auto& source = shaders_[shader];
    if (source.prefetch_status) return source.prefetch_status == 2;
    source.prefetch_status = 1;
    const bool vertex = shaders_[shader].stage == ShaderStage::kVertex;
    if (!vertex && shaders_[shader].stage != ShaderStage::kPixel) return false;
    auto job = std::make_unique<PipelineJob>();
    auto pd = warm_pd_;
    job->code[vertex ? 0 : 1] = shaders_[shader].code;
    if (vertex) {
      pd.VS = {job->code[0]->data(), job->code[0]->size()}; pd.PS = {}; pd.NumRenderTargets = 0;
      for (auto& format : pd.RTVFormats) format = DXGI_FORMAT_UNKNOWN;
    } else pd.PS = {job->code[1]->data(), job->code[1]->size()};
    const auto name = PipelineLibraryName(pd);
    if (!warm_sources_.HasPipeline(name)) return false;
    source.prefetch_status = 2;
    job->stage_prefetch = true;
    {
      std::lock_guard<std::mutex> lock(job_mutex_);
      if (stop_workers_) return false;
      if (live_seeded_.count(name) || stage_prefetch_seen_.count(name)) return true;
      // Optional prefetch is bounded. A draw still restores any stage not queued here.
      if (warm_jobs_.size() >= 256) return true;
      stage_prefetch_seen_.insert(name);
      stage_prefetches_.fetch_add(1, std::memory_order_relaxed);
      warm_jobs_.push_front(std::move(job));
    }
    job_cv_.notify_one();
    return true;
  }
  bool WarmVertexShader(std::vector<uint8_t>&& bytecode, uint64_t source_hash = 0) override {
    if (!warm_ready_.load(std::memory_order_acquire) || bytecode.empty()) return false;
    auto job = std::make_unique<PipelineJob>();
    job->desc = warm_pd_;
    job->code[0] = std::make_shared<const std::vector<uint8_t>>(std::move(bytecode));
    job->desc.VS = {job->code[0]->data(), job->code[0]->size()};
    job->desc.PS = {};
    job->desc.NumRenderTargets = 0;
    for (auto& format : job->desc.RTVFormats) format = DXGI_FORMAT_UNKNOWN;
    return QueueWarmPipeline(std::move(job), source_hash, true);
  }
  bool QueueWarmPipeline(std::unique_ptr<PipelineJob> job, uint64_t source_hash, bool vertex) {
    job->library_name = PipelineLibraryName(job->desc);
    {
      std::lock_guard<std::mutex> lock(job_mutex_);
      if (stop_workers_) return false;
      const bool saved = warm_sources_.HasPipeline(job->library_name);
      if (saved || warm_success_names_.count(job->library_name)) {
        if (source_hash && saved) warm_sources_.Remember({source_hash, vertex}, job->library_name);
        return true;
      }
      if (source_hash) warm_source_pending_[job->library_name].push_back({source_hash, vertex});
      if (!warm_seen_.insert(job->library_name).second) return true;
      warm_jobs_.push_back(std::move(job));
    }
    job_cv_.notify_one();
    return true;
  }
  uint64_t WarmJobsLeft() override {
    std::lock_guard<std::mutex> lock(job_mutex_);
    return warm_jobs_.size() + warm_busy_;
  }
  void ConfigureWarmSourceCache(const std::string& identity) override {
    if (options_.pipeline_library_path.empty()) return;
    const auto name = PipelineLibraryName(warm_pd_);
    warm_source_identity_ = identity + std::string(name.begin(), name.end());
    const bool compatible = warm_library_valid_.load();
    // Save an empty driver/adapter validation anchor before the first compile. The
    // individual stage blobs are streamed to disk and their PSOs immediately released.
    if (!compatible && warm_pipeline_library_) {
      warm_library_dirty_ = true;
      if (SavePipelineLibrary(false, true))
        LoadPipelineLibrary(WarmLibraryPath(), warm_pipeline_library_, warm_library_blob_, &warm_library_map_, &warm_library_valid_);
    }
    if (!warm_stages_.Open(WarmLibraryPath().wstring() + L".stages", compatible))
      REXLOG_WARN("gow2_renderer: compiled stage store unavailable; shaders remain available through live compilation");
    cogbox::shader_cache::LibraryStamp stamp;
    const bool loaded = warm_library_valid_ &&
        WarmSourceStamp(stamp) &&
        warm_sources_.Load(WarmLibraryPath().wstring() + L".sources", stamp);
    REXLOG_INFO("gow2_renderer: compiled source inventory: {} ({} sources)", loaded ? "valid" : "will rebuild", warm_sources_.Size());
  }
  bool WarmSourceCached(bool vertex, uint64_t hash) override {
    if (!warm_sources_.Contains({hash, vertex})) return false;
    warm_source_hits_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  uint64_t WarmSourcesReused() const override { return warm_source_hits_.load(std::memory_order_relaxed); }
  void WaitWarmCapacity(uint64_t capacity) override {
    std::unique_lock<std::mutex> lock(job_mutex_);
    warm_capacity_cv_.wait_for(lock, std::chrono::milliseconds(25), [this, capacity] {
      return stop_workers_ || warm_jobs_.size() + warm_busy_ < capacity;
    });
  }
  void ReleaseWarmupCache() override {
    // Called after the startup barrier: no job may still read the library's backing memory.
    if (WarmJobsLeft()) return;
    REXLOG_INFO("gow2_renderer: streamed stage cache: {} pipelines, {} save failures", warm_stages_.Size(), warm_save_failed_.load());
    if (warm_source_hits_.load() && warm_pipeline_library_) {
      REXLOG_INFO("gow2_renderer: compiled shader inventory reused {}; mapped driver library retained for on-demand stages",
                  warm_source_hits_.load());
      return;
    }
    SafeRelease(warm_pipeline_library_);
    warm_library_map_.Reset();
    std::vector<uint8_t>().swap(warm_library_blob_);
    REXLOG_INFO("gow2_renderer: startup driver library released before gameplay");
  }
  void SetWarmWorkers(uint32_t n) override {  // prebuild v2: the renderer's load / play setting (0 = paused)
    warm_startup_.store(n >= 1024u, std::memory_order_relaxed);
    bool more = false;
    {
      std::lock_guard<std::mutex> lock(job_mutex_);
      more = n > warm_workers_;
      warm_workers_ = n;
      warm_workers_set_ = true;
    }
    if (more) job_cv_.notify_all();  // idle workers may take the waiting warm jobs now
  }
  std::mutex job_mutex_;
  std::condition_variable job_cv_;
  bool stop_workers_ = false;
  std::unordered_map<uint64_t, std::vector<std::pair<GraphicsPipelineDesc, PipelineHandle>>> pipeline_cache_;
  std::vector<Readback> readbacks_;
  std::vector<ReadbackHandle> pending_readbacks_;
  std::vector<DeferredRelease> deferred_releases_;
  std::vector<TextureHeap> tex_heaps_;  // patch 0017
  uint64_t placed_live_bytes_ = 0, placed_live_count_ = 0;  // job 306: placed textures not yet released
  // Recorded state, re-applied lazily.
  static constexpr uint32_t kMaxColorTargets = GraphicsPipelineDesc::kMaxColor;
  TextureHandle bound_colors_[kMaxColorTargets] = {};
  int32_t null_rtv_ = -1;  // a null 2D RTV for unused colour slots below a used one
  uint32_t bound_color_count_ = 0;
  TextureHandle bound_depth_ = 0;
  Viewport viewport_;
  Rect scissor_;
  uint8_t stencil_ref_ = 0;
  PipelineHandle pipeline_ = 0;
  D3D12_GPU_VIRTUAL_ADDRESS root_cbv_[8] = {};
  std::vector<uint8_t> cbv_shadow_[8];
  bool cbv_shared_[4] = {};  // job 177: the pixel slot points at the vertex slot's upload (SetConstantsBoth)
  bool cbv_reupload_ = false;
  uint32_t cbv_pending_ = 0;
  ID3D12Resource* zero_cb_ = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS zero_cb_gpu_ = 0;
  uint32_t root_cbv_dirty_ = 0;
  bool sample_positions_ = false, positions_dirty_ = false, positions_ever_ = false;  // patch 0089
  uint32_t positions_count_ = 0;
  std::array<int8_t, 8> positions_{};
  std::array<int8_t, 8> positions_for_[5]{};
  bool have_positions_for_[5] = {};
  bool rt_dirty_ = true, viewport_dirty_ = true, scissor_dirty_ = true, stencil_dirty_ = true, pipeline_dirty_ = true,
       blend_dirty_ = true;
  float blend_constant_[4] = {};
  bool index_vram_logged_ = false;  // patch 0150
  BackendStats stats_;
};

}  // namespace

std::unique_ptr<Backend> CreateD3D12Backend() { return std::make_unique<D3D12Backend>(); }

bool EnableDeviceLossDiagnostics() {
  ID3D12DeviceRemovedExtendedDataSettings* s = nullptr;
  if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&s))) || !s) return false;
  s->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  s->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  s->Release();
  return true;
}

}  // namespace xenos::renderer::gpu

// Global scope: shadow_renderer.cpp calls it.
// 0.2.0 (#9): the renderer couldn't start - say so and close, instead of a black window with sound and input.
void XenosRendererFatal(const std::string& why, bool vulkan) {
  REXLOG_ERROR("gow2_renderer: can't start the renderer: {}", why);
  std::string m = "The game's renderer couldn't start, so the game will close instead of showing a black screen.\n\n" + why +
                  (vulkan ? "\n\nTry Graphics API: DirectX 12 on the game's Graphics tab in COGBox, and update the graphics driver."
                          : "\n\nTry Graphics API: Vulkan on the game's Graphics tab in COGBox, and update Windows and the graphics "
                            "driver.");
  std::wstring w(m.size() + 1, L'\0');
  w.resize(size_t(MultiByteToWideChar(CP_UTF8, 0, m.c_str(), -1, w.data(), int(w.size()))) - 1);
  MessageBoxW(nullptr, w.c_str(), L"COGBox - renderer", MB_ICONERROR | MB_OK | MB_TOPMOST);
  ExitProcess(0xE0C0B009u);   // COGBox explains this code too
}

#endif  // _WIN32
