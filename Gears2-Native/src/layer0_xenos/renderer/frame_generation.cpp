// Layer 0 (game-free), patches 0146 + 0147: frame generation (item 6) with AMD FSR frame generation.
//
// Patch 0146 (plan step 2): AMD's frame-generation swap chain in place of the presenter's own. With
// --gow2_frame_generation=swapchain frame generation itself stays off, which proves the swap-chain swap alone: the game
// must look and behave exactly as before (resize, fullscreen toggle, exit, HDR, the frame latency waitable).
// Patch 0147 (plan step 3): --gow2_frame_generation=fsr adds the frame-generation context. For each image our renderer
// writes into the presenter it hands over that frame's depth and motion (patch 0145) through
// XenosFrameGenerationSetInputs; when the presenter is about to show that image, the before_present hook configures
// frame generation and records its Prepare pass on the same queue; AMD's swap chain then shows a generated frame between
// it and the previous one. A repaint of an image already shown is not presented (frame generation counts one frame per
// Present). Not yet: the game's HUD and our own overlays are interpolated with the scene (step 4/5), and characters'
// own movement is not in the motion vectors (step 6) - both show as smearing in the generated frames.
//
// How it hooks in (SDK patch 36, rex/frame_hooks.h): create_swap_chain makes the swap chain through
// ffxCreateContext(FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12) with the presenter's own description, factory, queue and
// window; swap_chain_change waits for the swap chain's queued presents before a resize (and drops the frame-generation
// context, whose display size changes), and destroys the contexts before the presenter drops its last reference (AMD's
// order: frame-generation context, swap-chain context, then the release); before_present is described above.
//
// Built only with the FidelityFX SDK in third_party\fidelityfx-sdk (GN_HAVE_FSR, the same as the FSR upscaler); at run
// time amd_fidelityfx_loader_dx12.dll + amd_fidelityfx_framegeneration_dx12.dll next to the exe. Never committed: AMD's
// files (MIT). Our code (MIT).
#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/frame_hooks.h>
#include <rex/logging/macros.h>

#include <string>

REXCVAR_DEFINE_STRING(gow2_frame_generation, "off", "Gears2",
                      "Patch 0146 / 0147 (frame-rate item 6): frame generation. off = the usual swap chain; swapchain = "
                      "AMD's frame-generation swap chain with frame generation itself off (a test: the picture and "
                      "timing must be unchanged); fsr = AMD FSR frame generation, one generated frame between each two "
                      "the game draws (any DirectX 12 graphics card)");
REXCVAR_DEFINE_INT32(gow2_fg_debug, 0, "Gears2",
                     "Patch 0147: AMD frame-generation debug drawing, as flags: 1 tear lines, 2 reset indicators, 4 debug "
                     "view (the generated frame shows its inputs), 16 pacing lines");
REXCVAR_DEFINE_BOOL(gow2_frame_generation_hudless, false, "Gears2",
                    "Patch 0148 (frame generation step 4, test): with --gow2_frame_generation=fsr, also hand AMD the "
                    "frame as it was just before the HUD was drawn (HUDLessColor), so the HUD is not smeared in the "
                    "generated frames. The HUD start is guessed per frame: the first blended draw after the last "
                    "full-screen opaque pass that follows the scene. Off: nothing changes");

#if defined(GN_HAVE_FSR)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <ffx_api_types.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_framegeneration.h>
#include <dx12/ffx_api_framegeneration_dx12.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

namespace {

std::mutex g_mutex;  // every hook and SetInputs; never held inside an AMD callback
HMODULE g_module = nullptr;
ffxFunctions g_fn{};
bool g_load_failed = false;
bool g_generate = false;  // --gow2_frame_generation=fsr
std::atomic<bool> g_inputs_wanted{false};  // the renderer hands over inputs (off again once AMD's files fail)

// The swap chain (0146).
ffxContext g_swap_chain_ctx = nullptr;
IDXGISwapChain4* g_swap_chain = nullptr;  // not owned: the presenter holds the reference
DXGI_SWAP_CHAIN_DESC1 g_desc{};           // "pointers passed in desc must remain live until ffxDestroyContext"

// Frame generation (0147).
ffxContext g_fg_ctx = nullptr;
bool g_fg_failed = false;                  // creation failed once: not tried again until the swap chain changes
uint32_t g_fg_display_w = 0, g_fg_display_h = 0, g_fg_render_w = 0, g_fg_render_h = 0;
bool g_fg_infinite = false;
bool g_gen_on = false;                     // generation was on at the last Present
uint64_t g_last_shown_frame = 0;           // the renderer frame of the last image presented with inputs
XenosFgFrame g_last_frame{};               // its inputs (re-sent with reset while a few frames lack a scene)
bool g_have_last_frame = false;
uint32_t g_no_scene_streak = 0;
constexpr uint32_t kNoSceneFramesBeforeOff = 30;  // review: no on/off flapping for a frame or two without a scene
uint32_t g_transfer = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
ID3D12Device* g_device = nullptr;          // from the queue; one reference held
ID3D12CommandQueue* g_queue = nullptr;     // not owned
struct PrepareList {
  ID3D12CommandAllocator* allocator = nullptr;
  uint64_t fence_value = 0;
};
std::array<PrepareList, 3> g_lists{};
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12Fence* g_fence = nullptr;
HANDLE g_fence_event = nullptr;
uint64_t g_fence_value = 0;
uint64_t g_list_index = 0;
// Patch 0148: the HUD-less image at the back buffer's size and format (AMD: "same format and resolution as the
// backbuffer"); the renderer's image is copied in at the generation rectangle. Rests in NON_PIXEL_SHADER_RESOURCE.
bool g_hudless_on = false;                  // --gow2_frame_generation_hudless
std::atomic<uint32_t> g_hudless_format{0};  // for the renderer: 0 none, 1 R8G8B8A8_UNORM, 2 R10G10B10A2_UNORM
ID3D12Resource* g_hl_tex = nullptr;
DXGI_FORMAT g_hl_format = DXGI_FORMAT_UNKNOWN;
uint32_t g_hl_w = 0, g_hl_h = 0;
uint64_t g_hl_used = 0, g_hl_missing = 0, g_hl_mismatch = 0;  // log window

// The inputs per presenter image, newest first served (written by our renderer thread, read at present time).
struct Inputs {
  void* resource = nullptr;  // the presenter's guest-output texture
  XenosFgFrame frame{};
  bool has = false;          // false: no 3D scene that frame (frame generation pauses)
  uint64_t serial = 0;       // which hand-over this was (one per image written)
};
std::array<Inputs, 4> g_inputs{};
uint64_t g_serial = 0, g_last_presented = 0, g_frame_id = 0;
// Log window.
uint64_t g_presents = 0, g_repaints_skipped = 0, g_generated = 0, g_paused = 0, g_resets = 0, g_prepare_failed = 0;
constexpr uint64_t g_log_at = 600;

void FfxMessage(uint32_t type, const wchar_t* message) {
  std::string m;
  for (const wchar_t* p = message; p && *p; ++p) m += *p < 128 ? char(*p) : '?';
  if (type == FFX_API_MESSAGE_TYPE_ERROR) REXLOG_WARN("gow2_frame_generation: AMD: {}", m);
  else REXLOG_INFO("gow2_frame_generation: AMD: {}", m);
}

bool Load() {
  if (g_fn.CreateContext) return true;
  if (g_load_failed) return false;
  g_module = LoadLibraryW(L"amd_fidelityfx_loader_dx12.dll");
  if (!g_module) {
    g_load_failed = true;
    g_inputs_wanted.store(false, std::memory_order_relaxed);
    REXLOG_WARN("gow2_frame_generation: amd_fidelityfx_loader_dx12.dll not found next to the exe; usual swap chain");
    return false;
  }
  ffxLoadFunctions(&g_fn, g_module);
  if (!g_fn.CreateContext || !g_fn.DestroyContext || !g_fn.Dispatch || !g_fn.Configure) {
    g_load_failed = true;
    g_inputs_wanted.store(false, std::memory_order_relaxed);
    g_fn = {};
    REXLOG_WARN("gow2_frame_generation: the FidelityFX loader has no ffx_api entry points; usual swap chain");
    return false;
  }
  return true;
}

void WaitForPresents() {
  if (!g_swap_chain_ctx) return;
  ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 w{};
  w.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
  g_fn.Dispatch(&g_swap_chain_ctx, &w.header);
}

void WaitForPrepareLists() {
  if (!g_fence || !g_fence_value) return;
  if (g_fence->GetCompletedValue() < g_fence_value) {
    g_fence->SetEventOnCompletion(g_fence_value, g_fence_event);
    WaitForSingleObject(g_fence_event, INFINITE);  // review: never reuse or free lists the GPU may still read
  }
}

// Turns frame generation off on the swap chain and destroys its context (before a resize or the swap chain's end).
void ReleaseHudless();  // patch 0148: defined below

void DestroyFrameGeneration() {
  if (!g_fg_ctx) return;
  if (g_swap_chain_ctx && g_swap_chain) {
    ffxConfigureDescFrameGeneration c{};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = g_swap_chain;
    c.frameGenerationEnabled = false;
    c.frameID = g_frame_id;
    g_fn.Configure(&g_fg_ctx, &c.header);
  }
  WaitForPresents();
  WaitForPrepareLists();
  g_fn.DestroyContext(&g_fg_ctx, nullptr);
  g_fg_ctx = nullptr;
  ReleaseHudless();  // patch 0148: sized for the old back buffer
  g_gen_on = false;
  g_have_last_frame = false;
  REXLOG_INFO("gow2_frame_generation: patch 0147: frame-generation context destroyed");
}

void ReleaseHudless() {  // callers have waited for the queue (presents and Prepare lists)
  if (g_hl_tex) g_hl_tex->Release(), g_hl_tex = nullptr;
  g_hl_w = g_hl_h = 0;
  g_hl_format = DXGI_FORMAT_UNKNOWN;
}

void ReleasePrepareObjects() {
  WaitForPrepareLists();
  ReleaseHudless();
  for (PrepareList& l : g_lists)
    if (l.allocator) l.allocator->Release(), l.allocator = nullptr, l.fence_value = 0;
  if (g_list) g_list->Release(), g_list = nullptr;
  if (g_fence) g_fence->Release(), g_fence = nullptr;
  if (g_fence_event) CloseHandle(g_fence_event), g_fence_event = nullptr;
  if (g_device) g_device->Release(), g_device = nullptr;
  g_fence_value = 0;
  g_queue = nullptr;
}

bool EnsurePrepareObjects(ID3D12CommandQueue* queue) {
  if (g_list && g_queue == queue) return true;
  ReleasePrepareObjects();
  if (FAILED(queue->GetDevice(IID_PPV_ARGS(&g_device)))) return false;
  for (PrepareList& l : g_lists)
    if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&l.allocator))))
      return false;
  if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_lists[0].allocator, nullptr,
                                         IID_PPV_ARGS(&g_list))))
    return false;
  g_list->Close();
  if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
  g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!g_fence_event) {
    ReleasePrepareObjects();
    return false;
  }
  g_queue = queue;
  return true;
}

// AMD calls this (from its present path) to record the interpolation itself.
ffxReturnCode_t GenerateCallback(ffxDispatchDescFrameGeneration* params, void*) {
  params->backbufferTransferFunction = g_transfer;
  if (g_transfer == FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ) {
    params->minMaxLuminance[0] = 0.0001f;
    params->minMaxLuminance[1] = 1000.0f;
  }
  return g_fn.Dispatch(&g_fg_ctx, &params->header);
}

bool EnsureFrameGeneration(const XenosFgFrame& f) {
  if (!g_swap_chain || g_fg_failed) return false;
  DXGI_SWAP_CHAIN_DESC1 sd{};
  if (FAILED(g_swap_chain->GetDesc1(&sd))) return false;
  // The infinite-depth flag is fixed at creation (review: a heuristic per frame must not rebuild the context).
  if (g_fg_ctx && g_fg_display_w == sd.Width && g_fg_display_h == sd.Height && f.width <= g_fg_render_w &&
      f.height <= g_fg_render_h)
    return true;
  DestroyFrameGeneration();
  ffxCreateBackendDX12Desc be{};
  be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  be.device = g_device;
  ffxCreateContextDescFrameGenerationVersion ver{};  // "omitting this will prevent new APIs from functioning"
  ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
  ver.version = FFX_FRAMEGENERATION_VERSION;
  ver.header.pNext = &be.header;
  ffxCreateContextDescFrameGeneration c{};
  c.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
  c.header.pNext = &ver.header;
  const bool hdr = sd.Format == DXGI_FORMAT_R10G10B10A2_UNORM;  // the presenter's HDR10 swap chain (SDK patch 24)
  c.flags = (f.infinite ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE : 0u) |
            (hdr ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0u);
  c.displaySize = {sd.Width, sd.Height};
  c.maxRenderSize = {std::max(f.width, g_fg_render_w), std::max(f.height, g_fg_render_h)};
  c.backBufferFormat = ffxApiGetSurfaceFormatDX12(sd.Format);
  const ffxReturnCode_t rc = g_fn.CreateContext(&g_fg_ctx, &c.header, nullptr);
  if (rc != FFX_API_RETURN_OK || !g_fg_ctx) {
    g_fg_ctx = nullptr;
    g_fg_failed = true;
    REXLOG_WARN("gow2_frame_generation: patch 0147: the frame-generation context was not made (code {}); frames are "
                "shown as drawn", uint32_t(rc));
    return false;
  }
  g_fg_display_w = sd.Width, g_fg_display_h = sd.Height;
  g_fg_render_w = c.maxRenderSize.width, g_fg_render_h = c.maxRenderSize.height;
  g_fg_infinite = f.infinite;
  g_transfer = hdr ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ : FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
  std::string version = "?";
  ffxQueryGetProviderVersion v{};
  v.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
  if (g_fn.Query && g_fn.Query(&g_fg_ctx, &v.header) == FFX_API_RETURN_OK && v.versionName) version = v.versionName;
  REXLOG_INFO("gow2_frame_generation: patch 0147: AMD frame generation {} on: display {}x{}, scene up to {}x{}{}{}",
              version, sd.Width, sd.Height, g_fg_render_w, g_fg_render_h, hdr ? ", HDR10" : "",
              f.infinite ? ", infinite depth" : "");
  return true;
}

// Patch 0148: the back-buffer-sized HUD-less texture (made again when the back buffer's size or format changes).
bool EnsureHudless(const DXGI_SWAP_CHAIN_DESC1& sd) {
  if (g_hl_tex && g_hl_w == sd.Width && g_hl_h == sd.Height && g_hl_format == sd.Format) return true;
  WaitForPresents();  // AMD may still read the old one
  WaitForPrepareLists();
  ReleaseHudless();
  if (!g_device) return false;
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = sd.Width;
  rd.Height = sd.Height;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = sd.Format;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  D3D12_CLEAR_VALUE* no_clear = nullptr;
  if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, no_clear,
                                               IID_PPV_ARGS(&g_hl_tex)))) {
    g_hl_tex = nullptr;
    return false;
  }
  g_hl_tex->SetName(L"gow2 frame generation HUD-less colour");
  g_hl_w = sd.Width, g_hl_h = sd.Height, g_hl_format = sd.Format;
  return true;
}

void Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  g_list->ResourceBarrier(1, &b);
}

// Records and submits the Prepare pass for this frame's inputs; false = generation paused for this frame. hl_x/hl_y:
// where f.hudless goes in the HUD-less texture (patch 0148; hl_x < 0 = no copy this frame).
bool Prepare(const XenosFgFrame& f, uint32_t flags, int32_t hl_x = -1, int32_t hl_y = -1) {
  PrepareList& l = g_lists[g_list_index % g_lists.size()];
  if (l.fence_value && g_fence->GetCompletedValue() < l.fence_value) {
    // Outside review: a bounded wait - if the GPU is lost this list never completes; skip generation, don't hang.
    if (FAILED(g_fence->SetEventOnCompletion(l.fence_value, g_fence_event)) ||
        WaitForSingleObject(g_fence_event, 2000) != WAIT_OBJECT_0)
      return false;
  }
  if (FAILED(l.allocator->Reset()) || FAILED(g_list->Reset(l.allocator, nullptr))) return false;
  if (hl_x >= 0 && g_hl_tex && f.hudless) {  // patch 0148: the renderer's HUD-less image into the back-buffer-sized one
    auto* src = static_cast<ID3D12Resource*>(f.hudless);
    Transition(src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(g_hl_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION d{}, s{};
    d.pResource = g_hl_tex;
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{0, 0, 0, f.hudless_width, f.hudless_height, 1};
    g_list->CopyTextureRegion(&d, uint32_t(hl_x), uint32_t(hl_y), 0, &s, &box);
    Transition(g_hl_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  ffxDispatchDescFrameGenerationPrepareV2 p{};
  p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
  p.frameID = g_frame_id;
  p.flags = flags;
  p.commandList = g_list;
  p.renderSize = {f.width, f.height};
  p.jitterOffset = {f.jitter_x, f.jitter_y};
  p.motionVectorScale = {1.0f, 1.0f};  // our motion is in render pixels already (as for the FSR upscaler)
  p.frameTimeDelta = f.frame_ms;
  p.reset = f.reset;
  p.cameraNear = f.camera_near;
  p.cameraFar = f.camera_far;
  p.cameraFovAngleVertical = f.vertical_fov;
  p.viewSpaceToMetersFactor = 0.01f;  // Unreal units are centimetres
  p.depth = ffxApiGetResourceDX12(static_cast<ID3D12Resource*>(f.depth), FFX_API_RESOURCE_STATE_COMPUTE_READ);
  p.motionVectors = ffxApiGetResourceDX12(static_cast<ID3D12Resource*>(f.motion), FFX_API_RESOURCE_STATE_COMPUTE_READ);
  std::memcpy(p.cameraPosition, f.camera_position, sizeof(p.cameraPosition));
  std::memcpy(p.cameraUp, f.camera_up, sizeof(p.cameraUp));
  std::memcpy(p.cameraRight, f.camera_right, sizeof(p.cameraRight));
  std::memcpy(p.cameraForward, f.camera_forward, sizeof(p.cameraForward));
  const ffxReturnCode_t rc = g_fn.Dispatch(&g_fg_ctx, &p.header);
  g_list->Close();
  if (rc != FFX_API_RETURN_OK) return false;
  ID3D12CommandList* lists[] = {g_list};
  g_queue->ExecuteCommandLists(1, lists);
  l.fence_value = ++g_fence_value;
  if (FAILED(g_queue->Signal(g_fence, l.fence_value))) {  // never wait later on a value that was never signalled
    l.fence_value = 0;
    --g_fence_value;
    return false;
  }
  ++g_list_index;
  return true;
}

bool SameObject(void* a, IDXGISwapChain4* b) {
  if (!a || !b) return false;
  if (a == b) return true;
  IUnknown* ua = nullptr;
  IUnknown* ub = nullptr;
  static_cast<IUnknown*>(a)->QueryInterface(IID_PPV_ARGS(&ua));
  b->QueryInterface(IID_PPV_ARGS(&ub));
  const bool same = ua && ua == ub;
  if (ua) ua->Release();
  if (ub) ub->Release();
  return same;
}

bool MakeSwapChain(void* factory, void* queue, void* hwnd, const void* desc, void** out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Load()) return false;
  if (g_swap_chain_ctx) {  // AMD: one swap chain per window - the old context must be gone (it is, after a destroy)
    REXLOG_WARN("gow2_frame_generation: a swap chain context is still alive; usual swap chain");
    return false;
  }
  g_desc = *static_cast<const DXGI_SWAP_CHAIN_DESC1*>(desc);
  IDXGISwapChain4* sc = nullptr;
  ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 d{};
  d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
  d.swapchain = &sc;
  d.hwnd = static_cast<HWND>(hwnd);
  d.desc = &g_desc;
  static DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs{};  // borderless window, as the presenter's own swap chain
  fs.Windowed = TRUE;
  d.fullscreenDesc = &fs;
  d.dxgiFactory = static_cast<IDXGIFactory*>(static_cast<IDXGIFactory2*>(factory));
  d.gameQueue = static_cast<ID3D12CommandQueue*>(queue);
  ffxCreateContextDescFrameGenerationSwapChainVersionDX12 v{};
  v.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
  v.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
  d.header.pNext = &v.header;
  const ffxReturnCode_t rc = g_fn.CreateContext(&g_swap_chain_ctx, &d.header, nullptr);
  if (rc != FFX_API_RETURN_OK || !sc) {
    REXLOG_WARN("gow2_frame_generation: AMD's frame-generation swap chain was not made (code {}); usual swap chain",
                uint32_t(rc));
    if (g_swap_chain_ctx) g_fn.DestroyContext(&g_swap_chain_ctx, nullptr);
    g_swap_chain_ctx = nullptr;
    if (sc) sc->Release();
    g_inputs_wanted.store(false, std::memory_order_relaxed);  // the usual swap chain: no inputs needed
    return false;
  }
  g_swap_chain = sc;
  g_fg_failed = false;
  // Patch 0148: the renderer makes its HUD-less image in the back buffer's format (a straight copy then).
  g_hudless_format.store(!g_hudless_on ? 0u
                         : g_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM    ? 1u
                         : g_desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ? 2u
                                                                          : 0u,
                         std::memory_order_relaxed);
  if (g_hudless_on && !g_hudless_format.load(std::memory_order_relaxed))
    REXLOG_WARN("gow2_frame_generation_hudless: back buffer format {} not handled; no HUD-less image",
                uint32_t(g_desc.Format));
  g_inputs_wanted.store(g_generate, std::memory_order_relaxed);
  *out = static_cast<IDXGISwapChain1*>(sc);  // the creation's reference goes to the presenter
  REXLOG_INFO("gow2_frame_generation: patch 0146: AMD frame-generation swap chain made ({}x{}, format {}, flags {:X}); "
              "frame generation {}", g_desc.Width, g_desc.Height, uint32_t(g_desc.Format), g_desc.Flags,
              g_generate ? "on its first scene frame (patch 0147)" : "off");
  return true;
}

void SwapChainChange(void* swap_chain, bool destroying) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_swap_chain_ctx || !SameObject(swap_chain, g_swap_chain)) return;
  DestroyFrameGeneration();  // a resize changes the display size; it is made again with the next scene frame
  g_fg_failed = false;
  WaitForPresents();
  if (!destroying) return;
  ReleasePrepareObjects();
  g_fn.DestroyContext(&g_swap_chain_ctx, nullptr);
  g_swap_chain_ctx = nullptr;
  g_swap_chain = nullptr;
  g_hudless_format.store(0, std::memory_order_relaxed);
  REXLOG_INFO("gow2_frame_generation: patch 0146: frame-generation swap chain context destroyed");
}

// Presenter thread, before each Present.
bool BeforePresent(void* guest_output, void* queue, const int32_t* rect) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_swap_chain_ctx || !g_generate) return true;
  const Inputs* in = nullptr;
  for (const Inputs& i : g_inputs)
    if (guest_output && i.resource == guest_output && i.serial && (!in || i.serial > in->serial)) in = &i;
  // The same image again (a window repaint) is not a new frame - skipped only while generation runs (review: a paused
  // generation must not stop resizes, the fps bar or load-hold repaints from showing).
  if (in && in->serial == g_last_presented && g_gen_on) {
    ++g_repaints_skipped;
    return false;
  }
  const bool new_image = in && in->serial != g_last_presented;
  if (in) g_last_presented = in->serial;
  ++g_frame_id;  // exactly one per Present (AMD: a step other than 1 resets generation)
  ++g_presents;
  // This Present's inputs: the image's own; a repeat or a frame without a scene re-sends the last ones with reset, for
  // up to kNoSceneFramesBeforeOff frames, before generation turns off.
  XenosFgFrame f{};
  bool have = false;
  if (in && in->has && new_image) {
    f = in->frame;
    if (g_last_shown_frame && f.frame != g_last_shown_frame + 1) f.reset = true;  // review: an image never shown
    g_last_shown_frame = f.frame;
    g_last_frame = f;
    g_have_last_frame = true;
    g_no_scene_streak = 0;
    have = true;
  } else if (g_have_last_frame && ++g_no_scene_streak <= kNoSceneFramesBeforeOff) {
    f = g_last_frame;
    f.reset = true;
    have = true;
    if (in && new_image) g_last_shown_frame = 0;  // a frame without a scene: the next one starts fresh
  }
  const int32_t debug = REXCVAR_GET(gow2_fg_debug);
  const uint32_t flags = uint32_t(debug > 0 ? debug : 0) &
                         (FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS |
                          FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_PACING_LINES);
  bool generate = have && EnsurePrepareObjects(static_cast<ID3D12CommandQueue*>(queue)) && EnsureFrameGeneration(f);
  if (!g_fg_ctx) {
    if (!generate) ++g_paused;
  } else {
    DXGI_SWAP_CHAIN_DESC1 sd{};
    g_swap_chain->GetDesc1(&sd);
    ffxConfigureDescFrameGeneration c{};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = g_swap_chain;
    c.frameGenerationCallback = &GenerateCallback;
    c.frameGenerationEnabled = generate;
    c.allowAsyncWorkloads = false;
    c.flags = flags;
    c.onlyPresentGenerated = false;
    // Where the image is in the back buffer (review: letterbox / pillarbox bars must stay out of generation).
    if (rect && rect[2] > 0 && rect[3] > 0) c.generationRect = {rect[0], rect[1], rect[2], rect[3]};
    else c.generationRect = {0, 0, int32_t(sd.Width), int32_t(sd.Height)};
    c.frameID = g_frame_id;
    // Patch 0148: the HUD-less image, when the renderer made one for this frame at exactly the generation rectangle's
    // size (the presenter shows our image 1:1 there) in the back buffer's format.
    int32_t hl_x = -1, hl_y = -1;
    if (g_hudless_on && generate) {
      const bool fits = f.hudless && int32_t(f.hudless_width) == c.generationRect.width &&
                        int32_t(f.hudless_height) == c.generationRect.height && c.generationRect.left >= 0 &&
                        c.generationRect.top >= 0 &&
                        uint32_t(c.generationRect.left + c.generationRect.width) <= sd.Width &&
                        uint32_t(c.generationRect.top + c.generationRect.height) <= sd.Height;
      if (!f.hudless) ++g_hl_missing;
      else if (!fits) ++g_hl_mismatch;
      if (fits && EnsureHudless(sd)) {
        hl_x = c.generationRect.left, hl_y = c.generationRect.top;
        c.HUDLessColor = ffxApiGetResourceDX12(g_hl_tex, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        ++g_hl_used;
      }
    }
    // Paused and already off: nothing to tell AMD (a disabling Configure waits for queued presents).
    if (!generate && !g_gen_on) {
      ++g_paused;
      return true;
    }
    if (g_fn.Configure(&g_fg_ctx, &c.header) != FFX_API_RETURN_OK) generate = false;
    if (generate && !Prepare(f, flags, hl_x, hl_y)) {
      ++g_prepare_failed;
      c.frameGenerationEnabled = false;
      g_fn.Configure(&g_fg_ctx, &c.header);
      generate = false;
    }
    if (generate) {
      ++g_generated;
      g_resets += f.reset ? 1 : 0;
    } else {
      ++g_paused;
    }
  }
  g_gen_on = generate;
  if (g_presents >= g_log_at) {
    REXLOG_INFO("gow2_frame_generation: patch 0147: {} frames presented: generation on {} ({} resets), paused {} (no "
                "scene or not ready), Prepare failed {}, window repaints not presented {}", g_presents, g_generated,
                g_resets, g_paused, g_prepare_failed, g_repaints_skipped);
    if (g_hudless_on)
      REXLOG_INFO("gow2_frame_generation_hudless: patch 0148: HUD-less image given {}, none made {}, size not the "
                  "generation rectangle's {}", g_hl_used, g_hl_missing, g_hl_mismatch);
    g_hl_used = g_hl_missing = g_hl_mismatch = 0;
    g_generated = g_paused = g_resets = g_prepare_failed = g_repaints_skipped = 0;
    g_presents = 0;
  }
  return true;
}

}  // namespace

bool XenosFrameGenerationWanted() { return g_inputs_wanted.load(std::memory_order_relaxed); }

uint32_t XenosFrameGenerationHudlessFormat() {
  return g_inputs_wanted.load(std::memory_order_relaxed) ? g_hudless_format.load(std::memory_order_relaxed) : 0u;
}

void XenosFrameGenerationSetInputs(void* guest_output_resource, const XenosFgFrame* frame) {
  if (!g_generate || !guest_output_resource) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  Inputs* slot = nullptr;
  for (Inputs& i : g_inputs)
    if (i.resource == guest_output_resource) slot = &i;
  if (!slot) {  // the oldest
    slot = &g_inputs[0];
    for (Inputs& i : g_inputs)
      if (i.serial < slot->serial) slot = &i;
  }
  slot->resource = guest_output_resource;
  slot->has = frame != nullptr;
  if (frame) slot->frame = *frame;
  slot->serial = ++g_serial;
}

void XenosFrameGenerationInstall() {
  const std::string mode = REXCVAR_GET(gow2_frame_generation);
  if (mode.empty() || mode == "off") return;
  if (mode != "swapchain" && mode != "fsr") {
    REXLOG_WARN("gow2_frame_generation: unknown '{}' (off | swapchain | fsr); off", mode);
    return;
  }
  g_generate = mode == "fsr";
  g_hudless_on = g_generate && REXCVAR_GET(gow2_frame_generation_hudless);
  g_inputs_wanted.store(g_generate, std::memory_order_relaxed);
  rex::FrameHooks& h = rex::GetFrameHooks();
  h.create_swap_chain.store(&MakeSwapChain, std::memory_order_release);
  h.swap_chain_change.store(&SwapChainChange, std::memory_order_release);
  if (g_generate) h.before_present.store(&BeforePresent, std::memory_order_release);
  REXLOG_INFO("gow2_frame_generation: patches 0146/0147: {} (SDK patch 36 hooks installed)",
              g_generate ? "AMD FSR frame generation" : "AMD's frame-generation swap chain only, generation off");
}

#else  // !GN_HAVE_FSR

bool XenosFrameGenerationWanted() { return false; }
uint32_t XenosFrameGenerationHudlessFormat() { return 0; }
void XenosFrameGenerationSetInputs(void*, const XenosFgFrame*) {}

void XenosFrameGenerationInstall() {
  const std::string mode = REXCVAR_GET(gow2_frame_generation);
  if (!mode.empty() && mode != "off")
    REXLOG_INFO("gow2_frame_generation: this build has no FidelityFX SDK (third_party\\fidelityfx-sdk was missing when "
                "built); off");
}

#endif  // GN_HAVE_FSR
