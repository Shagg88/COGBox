// Layer 0 (game-free), patch 0135: NVIDIA Reflex low-latency mode for the shared renderer (and AMD Anti-Lag 2, 0136).
//
// What it does: Reflex holds the game's own thread for a moment at the start of each frame, so the game reads the
// controller as late as possible and the frame reaches the screen with almost no queue in front of it. On a card or
// driver without Reflex every call here does nothing.
//
// Where it hooks (SDK patch 35, rex/frame_hooks.h):
//   guest_swap    - guest thread, the end of VdSwap: frame k is handed over. Markers SIMULATION_END(k) and
//                   RENDERSUBMIT_START(k), then NvAPI_D3D_Sleep (the Reflex wait), then SIMULATION_START(k+1).
//   input_read    - guest thread, XamInputGetState: INPUT_SAMPLE for the frame being simulated (first read only).
//   present_begin / present_end - the presenter's Present: PRESENT_START / PRESENT_END.
//   XenosLatencyFrameSubmitted (our renderer's OnSwap, after Submit): RENDERSUBMIT_END for the frame it recorded.
//
// Built only when NVIDIA's NVAPI headers and library are in third_party/nvapi (never committed; CMake sets
// GN_WITH_REFLEX). Without them the functions below are empty and the gow2_reflex setting says so in the log.
#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/frame_hooks.h>
#include <rex/logging/macros.h>

#include <atomic>
#include <cstdint>

REXCVAR_DEFINE_BOOL(gow2_reflex, false, "Gears2",
                    "Patch 0135: NVIDIA Reflex low-latency mode (NVIDIA cards; ignored on others).");
REXCVAR_DEFINE_BOOL(gow2_antilag2, false, "Gears2",
                    "Patch 0136: AMD Anti-Lag 2 (AMD Radeon cards with a recent driver; ignored on others).");
REXCVAR_DEFINE_BOOL(gow2_reflex_boost, false, "Gears2",
                    "Patch 0135: Reflex 'boost' - keeps the graphics card's clocks up (lower lag, more power).");

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

// Patch 0136: AMD Anti-Lag 2 (GPUOpen AntiLag2-SDK, header-only, in third_party/antilag2-sdk; never committed). AMD's
// rule: call Update once per frame just before the game reads its input - the end of VdSwap is that point here.
#if defined(GN_WITH_ANTILAG2)
#include <ffx_antilag2_dx12.h>
namespace {
AMD::AntiLag2DX12::Context g_al2{};
std::atomic<ID3D12Device*> g_al2_device{nullptr};
std::atomic<bool> g_al2_on{false};
bool g_al2_tried = false;  // guest thread only
void AntiLagSwap() {
  if (!g_al2_tried) {
    ID3D12Device* d = g_al2_device.load(std::memory_order_acquire);
    if (!d) return;
    g_al2_tried = true;
    const HRESULT hr = AMD::AntiLag2DX12::Initialize(&g_al2, d);
    g_al2_on.store(SUCCEEDED(hr), std::memory_order_relaxed);
    REXLOG_INFO("gow2_antilag2: patch 0136: AMD Anti-Lag 2 {}", SUCCEEDED(hr) ? "on" : "not available (not an AMD card or the driver is too old)");
  }
  if (g_al2_on.load(std::memory_order_relaxed)) AMD::AntiLag2DX12::Update(&g_al2, true, 0);  // 0 = no cap of its own
}
}  // namespace
#endif

#if defined(GN_WITH_REFLEX)

#include <dxgi.h>  // nvapi.h's D3D part names IDXGISwapChain (job 447)
#include <nvapi.h>

namespace {

std::atomic<IUnknown*> g_device{nullptr};
std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_sim_frame{1};       // the frame the guest is simulating now
std::atomic<uint64_t> g_input_frame{0};     // the last frame an INPUT_SAMPLE was sent for
std::atomic<uint64_t> g_submitted{0};       // frames our renderer has submitted
std::atomic<uint64_t> g_present_frame{0};   // frame id used for the present markers
bool g_tried = false;                       // guest thread only

void Marker(uint64_t frame, NV_LATENCY_MARKER_TYPE type) {
  IUnknown* dev = g_device.load(std::memory_order_acquire);
  if (!dev || !g_on.load(std::memory_order_relaxed)) return;
  NV_LATENCY_MARKER_PARAMS p{};
  p.version = NV_LATENCY_MARKER_PARAMS_VER;
  p.frameID = frame;
  p.markerType = type;
  NvAPI_D3D_SetLatencyMarker(dev, &p);
}

// Guest thread, first swap after the device is known: switch Reflex on (once).
void TurnOn() {
  g_tried = true;
  IUnknown* dev = g_device.load(std::memory_order_acquire);
  if (!dev) { g_tried = false; return; }  // the renderer is not up yet; try at the next swap
  NvAPI_Status s = NvAPI_Initialize();
  if (s != NVAPI_OK) {
    REXLOG_INFO("gow2_reflex: patch 0135: Reflex not used - NVAPI did not start ({}; not an NVIDIA card or driver)", int(s));
    return;
  }
  NV_SET_SLEEP_MODE_PARAMS p{};
  p.version = NV_SET_SLEEP_MODE_PARAMS_VER;
  p.bLowLatencyMode = true;
  p.bLowLatencyBoost = REXCVAR_GET(gow2_reflex_boost);
  p.minimumIntervalUs = 0;          // the game's own frame cap does the limiting
  p.bUseMarkersToOptimize = true;
  s = NvAPI_D3D_SetSleepMode(dev, &p);
  if (s != NVAPI_OK) {
    REXLOG_INFO("gow2_reflex: patch 0135: Reflex not used - the driver refused low-latency mode ({})", int(s));
    return;
  }
  g_on.store(true, std::memory_order_relaxed);
  REXLOG_INFO("gow2_reflex: patch 0135: NVIDIA Reflex on{}", p.bLowLatencyBoost ? " with boost" : "");
}

void OnGuestSwap() {
  if (!g_tried) TurnOn();
  if (!g_on.load(std::memory_order_relaxed)) return;
  const uint64_t k = g_sim_frame.load(std::memory_order_relaxed);
  Marker(k, SIMULATION_END);
  Marker(k, RENDERSUBMIT_START);
  NvAPI_D3D_Sleep(g_device.load(std::memory_order_acquire));  // the Reflex wait: the next frame starts later, fresher
  g_sim_frame.store(k + 1, std::memory_order_relaxed);
  Marker(k + 1, SIMULATION_START);
}

void OnInputRead() {
  const uint64_t k = g_sim_frame.load(std::memory_order_relaxed);
  if (g_input_frame.exchange(k, std::memory_order_relaxed) != k) Marker(k, INPUT_SAMPLE);
}

void OnPresentBegin() {
  const uint64_t k = g_submitted.load(std::memory_order_relaxed);
  g_present_frame.store(k, std::memory_order_relaxed);
  if (k) Marker(k, PRESENT_START);
}

void OnPresentEnd() {
  const uint64_t k = g_present_frame.load(std::memory_order_relaxed);
  if (k) Marker(k, PRESENT_END);
}

}  // namespace

static void ReflexInstall() {
  rex::FrameHooks& h = rex::GetFrameHooks();
  h.guest_swap.store(&OnGuestSwap, std::memory_order_release);
  h.input_read.store(&OnInputRead, std::memory_order_release);
  h.present_begin.store(&OnPresentBegin, std::memory_order_release);
  h.present_end.store(&OnPresentEnd, std::memory_order_release);
  REXLOG_INFO("gow2_reflex: patch 0135: Reflex hooks installed; it switches on with the first frame");
}

static void ReflexSetDevice(void* d3d12_device) {
  g_device.store(static_cast<IUnknown*>(d3d12_device), std::memory_order_release);
}

static void ReflexFrameSubmitted() {
  if (!g_on.load(std::memory_order_relaxed)) return;
  const uint64_t k = g_submitted.fetch_add(1, std::memory_order_relaxed) + 1;
  Marker(k, RENDERSUBMIT_END);
}

#endif  // GN_WITH_REFLEX

// One front for both: Reflex when asked for and built in; otherwise Anti-Lag 2 when asked for and built in.
namespace {
enum class Latency { kNone, kReflex, kAntiLag2 };
Latency g_mode = Latency::kNone;
}  // namespace

void XenosLatencyInstall() {
  XenosFrameGenerationInstall();  // patch 0146: its swap-chain hooks are set before the window exists
  if (REXCVAR_GET(gow2_reflex)) {
#if defined(GN_WITH_REFLEX)
    g_mode = Latency::kReflex;
    ReflexInstall();
    return;
#else
    REXLOG_INFO("gow2_reflex: patch 0135: this build has no NVIDIA Reflex (NVAPI was not in third_party when built)");
#endif
  }
  if (REXCVAR_GET(gow2_antilag2)) {
#if defined(GN_WITH_ANTILAG2)
    g_mode = Latency::kAntiLag2;
    rex::GetFrameHooks().guest_swap.store(&AntiLagSwap, std::memory_order_release);
    REXLOG_INFO("gow2_antilag2: patch 0136: Anti-Lag 2 hook installed; it switches on with the first frame");
#else
    REXLOG_INFO("gow2_antilag2: patch 0136: this build has no AMD Anti-Lag 2 (its SDK was not in third_party when built)");
#endif
  }
}

void XenosLatencySetDevice(void* d3d12_device) {
#if defined(GN_WITH_REFLEX)
  if (g_mode == Latency::kReflex) ReflexSetDevice(d3d12_device);
#endif
#if defined(GN_WITH_ANTILAG2)
  if (g_mode == Latency::kAntiLag2) g_al2_device.store(static_cast<ID3D12Device*>(d3d12_device), std::memory_order_release);
#endif
  (void)d3d12_device;
}

void XenosLatencyFrameSubmitted() {
#if defined(GN_WITH_REFLEX)
  if (g_mode == Latency::kReflex) ReflexFrameSubmitted();
#endif
}
