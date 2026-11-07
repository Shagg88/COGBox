// Layer 0 (game-free): NVIDIA DLSS Frame Generation (DLSS-G, DLSS 4 / 4.5 multi frame generation) through NVIDIA
// Streamline. EXPERIMENTAL, D3D12 only, RTX 40 / 50 series. --gow2_frame_generation=dlss selects it;
// --gow2_dlss_fg_mode picks 2x / 3x / 4x ... / dynamic (clamped to what the card and driver report).
//
// Targets the Streamline SDK v2.14.16 (github.com/NVIDIA-RTX/Streamline, tag v2.14.16, the newest public tag on
// 2026-11-07; DLSS-G plugin for DLSS SDK 310.x / DLSS 4.5 - DLSSGOptions v5 with numFramesToGenerate and
// DLSSGMode::eDynamic, DLSSGState v4 with numFramesToGenerateMax and bIsDynamicMFGSupported). The headers this needs are
// vendored, unchanged, in src/layer0_xenos/streamline/include (NVIDIA, MIT licence: streamline/LICENSE.txt). There is no
// link-time dependency: sl.interposer.dll is loaded at run time from the exe's folder (after its NVIDIA signature is
// checked) and every sl* function comes from GetProcAddress / slGetFeatureFunction. An older plugin simply leaves the
// newer state fields at their defaults (no dynamic mode, at most 2x).
//
// How it hooks in (manual hooking, docs/ProgrammingGuideManualHooking.md): slInit(eUseManualHooking) when the hooks are
// installed (before the window and its swap chain exist); create_swap_chain (SDK patch 36) gives Streamline the native
// device (slSetD3DDevice), checks DLSS-G / Reflex support on that adapter (slIsFeatureSupported, by LUID), upgrades the
// presenter's DXGI factory to Streamline's proxy (slUpgradeInterface) and makes the swap chain through it, so the
// presenter's Present / GetBuffer / ResizeBuffers / GetCurrentBackBufferIndex calls go through Streamline;
// before_present tags depth + motion (render size) + the HUD-less image + the back buffer sub-rectangle (the generation
// rectangle) for the frame (slSetTagForFrame, eValidUntilPresent), sets the camera constants (slSetConstants) and the
// DLSS-G options (slDLSSGSetOptions). The inputs are the same ones AMD FSR frame generation takes (patches 0146-0148).
// DLSS-G needs Reflex: while this runs, our NVAPI Reflex path (latency_reflex.cpp) stays off and the latency markers
// and the Reflex sleep go through sl.reflex / sl.pcl on the same SDK patch 35 hooks.
//
// Fails safe: no DLLs, an unsigned interposer, slInit failing, a card or driver without DLSS-G (RTX 30 and older), HAGS
// off or Windows older than 10 20H1 -> the reason is logged and the usual swap chain is made (or AMD FSR frame
// generation when the build has it). Never ships NVIDIA's files: players put them next to the exe (see the launcher).
// Our code (MIT).
#include "dlss_framegen.h"

#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/frame_hooks.h>
#include <rex/logging/macros.h>

#include <string>

REXCVAR_DEFINE_STRING(gow2_dlss_fg_mode, "2x", "Gears2",
                      "NVIDIA DLSS frame generation multiplier (with --gow2_frame_generation=dlss; RTX 40/50, "
                      "experimental): 2x, 3x, 4x, 5x, 6x or dynamic. Clamped to what the card and driver report "
                      "(RTX 40: 2x; RTX 50: up to 4x, 6x and dynamic with DLSS 4.5)");

#if defined(GN_HAVE_STREAMLINE) && defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <math.h>  // sl_matrix_helpers.h uses sqrtf unqualified

#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_matrix_helpers.h>
#include <sl_reflex.h>
#include <sl_security.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <cstring>
#include <mutex>

static_assert(SL_VERSION_MAJOR == 2 && SL_VERSION_MINOR >= 14,
              "dlss_framegen.cpp is written against the vendored Streamline 2.14.16 headers");

namespace {

std::mutex g_mutex;
HMODULE g_module = nullptr;
bool g_hudless_on = false;
XenosDlssFgFallback g_fallback = nullptr;
std::atomic<bool> g_active{false};       // installed and not (yet) found unsupported
std::atomic<bool> g_latency_on{false};   // sl.reflex markers / sleep may be sent
std::atomic<bool> g_inputs_wanted{false};
std::atomic<uint32_t> g_hudless_format{0};
std::atomic<uint32_t> g_hl_rect_size{0};

// Core API (sl.interposer.dll).
PFun_slInit* p_slInit = nullptr;
PFun_slShutdown* p_slShutdown = nullptr;
PFun_slIsFeatureSupported* p_slIsFeatureSupported = nullptr;
PFun_slSetTagForFrame* p_slSetTagForFrame = nullptr;
PFun_slSetConstants* p_slSetConstants = nullptr;
PFun_slUpgradeInterface* p_slUpgradeInterface = nullptr;
PFun_slGetFeatureFunction* p_slGetFeatureFunction = nullptr;
PFun_slGetNewFrameToken* p_slGetNewFrameToken = nullptr;
PFun_slSetD3DDevice* p_slSetD3DDevice = nullptr;
PFun_slGetFeatureVersion* p_slGetFeatureVersion = nullptr;
// Feature API (after slSetD3DDevice).
PFun_slDLSSGGetState* p_slDLSSGGetState = nullptr;
PFun_slDLSSGSetOptions* p_slDLSSGSetOptions = nullptr;
PFun_slReflexSetOptions* p_slReflexSetOptions = nullptr;
PFun_slReflexSleep* p_slReflexSleep = nullptr;
PFun_slPCLSetMarker* p_slPCLSetMarker = nullptr;

bool g_device_set = false;
IDXGISwapChain1* g_swap_chain = nullptr;  // Streamline's proxy; the presenter holds the reference
DXGI_SWAP_CHAIN_DESC1 g_desc{};
const sl::ViewportHandle g_viewport{0u};

// Options.
uint32_t g_requested_frames = 1;  // frames to generate per rendered frame (2x = 1)
bool g_requested_dynamic = false;
uint32_t g_max_frames = 0;        // from slDLSSGGetState (0 = not known yet)
bool g_dynamic_supported = false;
bool g_state_known = false;
sl::DLSSGMode g_mode_set = sl::DLSSGMode::eOff;
uint32_t g_frames_set = 0;
bool g_options_failed = false;

// Frames (presenter side) and latency markers.
std::atomic<uint64_t> g_sim_frame{1};
std::atomic<uint64_t> g_input_frame{0};
std::atomic<uint64_t> g_submitted{0};
std::atomic<uint64_t> g_present_frame{0};
uint64_t g_last_tagged = 0;

// Inputs per presenter image (as frame_generation.cpp).
struct Inputs {
  void* resource = nullptr;
  XenosFgFrame frame{};
  bool has = false;
  uint64_t serial = 0;
};
std::array<Inputs, 4> g_inputs{};
uint64_t g_serial = 0, g_last_presented = 0;
bool g_gen_on = false;
uint64_t g_last_shown_frame = 0;
XenosFgFrame g_last_frame{};
bool g_have_last_frame = false;
uint32_t g_no_scene_streak = 0;
constexpr uint32_t kNoSceneFramesBeforeOff = 30;

// Camera history for clipToPrevClip.
sl::float4x4 g_prev_view_to_world{};
sl::float4x4 g_prev_view_to_clip{};
bool g_have_prev_camera = false;

// Log window.
uint64_t g_presents = 0, g_generated = 0, g_paused = 0, g_repaints_skipped = 0, g_hl_used = 0, g_hl_mismatch = 0;
constexpr uint64_t kLogAt = 600;

const char* ResultName(sl::Result r) {
  switch (r) {
    case sl::Result::eOk: return "ok";
    case sl::Result::eErrorIO: return "eErrorIO (a plugin file is missing or unreadable)";
    case sl::Result::eErrorDriverOutOfDate: return "eErrorDriverOutOfDate (update the NVIDIA driver)";
    case sl::Result::eErrorOSOutOfDate: return "eErrorOSOutOfDate (needs Windows 10 20H1 or newer)";
    case sl::Result::eErrorOSDisabledHWS: return "eErrorOSDisabledHWS (turn on hardware-accelerated GPU scheduling)";
    case sl::Result::eErrorDeviceNotCreated: return "eErrorDeviceNotCreated";
    case sl::Result::eErrorNoSupportedAdapterFound: return "eErrorNoSupportedAdapterFound (needs an RTX 40 / 50 card)";
    case sl::Result::eErrorAdapterNotSupported: return "eErrorAdapterNotSupported (needs an RTX 40 / 50 card)";
    case sl::Result::eErrorNoPlugins: return "eErrorNoPlugins (the sl.*.dll plugins are not next to the exe)";
    case sl::Result::eErrorNGXFailed: return "eErrorNGXFailed (nvngx_dlssg.dll missing or refused)";
    case sl::Result::eErrorMissingProxy: return "eErrorMissingProxy";
    case sl::Result::eErrorInitNotCalled: return "eErrorInitNotCalled";
    case sl::Result::eErrorFeatureMissing: return "eErrorFeatureMissing (plugin not loaded)";
    case sl::Result::eErrorFeatureNotSupported: return "eErrorFeatureNotSupported (this card / driver has no DLSS-G)";
    case sl::Result::eErrorFeatureFailedToLoad: return "eErrorFeatureFailedToLoad";
    case sl::Result::eErrorFeatureMissingDependency: return "eErrorFeatureMissingDependency (sl.reflex / sl.pcl missing)";
    default: return "error";
  }
}

void SlLog(sl::LogType type, const char* msg) {
  std::string m = msg ? msg : "";
  while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
  if (type == sl::LogType::eError) REXLOG_WARN("gow2_frame_generation: Streamline: {}", m);
  else if (type == sl::LogType::eWarn) REXLOG_INFO("gow2_frame_generation: Streamline: {}", m);
  // eInfo is very chatty: left out
}

void SlApiError(const sl::APIError& e) {
  static uint32_t logged = 0;
  if (logged < 8) {
    ++logged;
    REXLOG_WARN("gow2_frame_generation: DLSS-G: DXGI / D3D error 0x{:08X}", uint32_t(e.hres));
  }
}

template <typename T>
bool Proc(T*& out, const char* name) {
  out = reinterpret_cast<T*>(reinterpret_cast<void*>(GetProcAddress(g_module, name)));
  return out != nullptr;
}

template <typename T>
bool FeatureProc(T*& out, sl::Feature f, const char* name) {
  void* fn = nullptr;
  if (!p_slGetFeatureFunction || p_slGetFeatureFunction(f, name, fn) != sl::Result::eOk || !fn) return false;
  out = reinterpret_cast<T*>(fn);
  return true;
}

std::wstring ExeDir() {
  wchar_t path[MAX_PATH * 2] = {};
  const DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  if (!n || n >= std::size(path)) return {};
  std::wstring s(path, n);
  const size_t slash = s.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : s.substr(0, slash + 1);
}

std::string Narrow(const std::wstring& w) {
  std::string s;
  for (wchar_t c : w) s += c < 128 ? char(c) : '?';
  return s;
}

sl::FrameToken* Token(uint64_t index) {
  if (!p_slGetNewFrameToken) return nullptr;
  sl::FrameToken* t = nullptr;
  const uint32_t i = uint32_t(index);
  if (p_slGetNewFrameToken(t, &i) != sl::Result::eOk) return nullptr;
  return t;
}

void Marker(sl::PCLMarker m, uint64_t frame) {
  if (!g_latency_on.load(std::memory_order_acquire) || !p_slPCLSetMarker || !frame) return;
  if (sl::FrameToken* t = Token(frame)) p_slPCLSetMarker(m, *t);
}

// Called with the lock held: DLSS-G will not run here - stop using Streamline and let the usual path take over.
void Disable(const char* why) {
  REXLOG_WARN("gow2_frame_generation: NVIDIA DLSS frame generation not used: {}", why);
  g_active.store(false, std::memory_order_release);
  g_latency_on.store(false, std::memory_order_release);
  g_inputs_wanted.store(false, std::memory_order_relaxed);
  g_hudless_format.store(0, std::memory_order_relaxed);
  rex::FrameHooks& h = rex::GetFrameHooks();
  h.before_present.store(nullptr, std::memory_order_release);
}

void ParseMode() {
  std::string m = REXCVAR_GET(gow2_dlss_fg_mode);
  for (char& c : m) c = char(std::tolower(static_cast<unsigned char>(c)));
  g_requested_dynamic = m == "dynamic" || m == "auto";
  g_requested_frames = 1;
  if (!g_requested_dynamic) {
    const int x = std::atoi(m.c_str());  // "3x" -> 3
    if (x >= 2 && x <= 6) g_requested_frames = uint32_t(x - 1);
    else REXLOG_WARN("gow2_dlss_fg_mode: unknown '{}' (2x .. 6x | dynamic); 2x", m);
  }
}

// What we actually ask for, given what the runtime reported.
void EffectiveMode(sl::DLSSGMode& mode, uint32_t& frames) {
  const uint32_t max = g_max_frames ? g_max_frames : 1u;
  if (g_requested_dynamic && g_dynamic_supported) {
    mode = sl::DLSSGMode::eDynamic;
    frames = max;  // the upper bound dynamic mode may use
    return;
  }
  mode = sl::DLSSGMode::eOn;
  frames = g_requested_dynamic ? max : std::min(g_requested_frames, max);
}

void QueryState(bool log) {
  if (!p_slDLSSGGetState) return;
  sl::DLSSGState st{};
  const sl::Result r = p_slDLSSGGetState(g_viewport, st, nullptr);
  if (r != sl::Result::eOk) {
    if (log) REXLOG_WARN("gow2_frame_generation: slDLSSGGetState failed: {}", ResultName(r));
    return;
  }
  g_state_known = true;
  g_max_frames = st.numFramesToGenerateMax ? st.numFramesToGenerateMax : 1u;
  g_dynamic_supported = st.bIsDynamicMFGSupported == sl::Boolean::eTrue;
  if (log) {
    REXLOG_INFO("gow2_frame_generation: DLSS-G state: status 0x{:X}, up to {}x ({} generated per frame), dynamic {}, "
                "{} frames presented since the last check, min size {}, VRAM {} MB", uint32_t(st.status),
                g_max_frames + 1, g_max_frames, g_dynamic_supported ? "supported" : "not supported",
                st.numFramesActuallyPresented, st.minWidthOrHeight, st.estimatedVRAMUsageInBytes >> 20);
    if (st.status != sl::DLSSGStatus::eOk)
      REXLOG_WARN("gow2_frame_generation: DLSS-G reports a problem (status 0x{:X}: 1 resolution too low, 2 Reflex not "
                  "seen, 4 HDR format, 8 constants invalid, 16 GetCurrentBackBufferIndex not called)",
                  uint32_t(st.status));
  }
}

bool SetOptions(bool on, const XenosFgFrame* f) {
  if (!p_slDLSSGSetOptions || g_options_failed) return false;
  sl::DLSSGMode mode = sl::DLSSGMode::eOff;
  uint32_t frames = 1;
  if (on) EffectiveMode(mode, frames);
  if (mode == g_mode_set && (mode == sl::DLSSGMode::eOff || frames == g_frames_set)) return on;
  sl::DLSSGOptions o{};
  o.mode = mode;
  o.numFramesToGenerate = frames;
  o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
  o.numBackBuffers = g_desc.BufferCount;
  o.colorWidth = g_desc.Width;
  o.colorHeight = g_desc.Height;
  o.colorBufferFormat = uint32_t(g_desc.Format);
  if (f) {
    o.mvecDepthWidth = f->width;
    o.mvecDepthHeight = f->height;
    o.mvecBufferFormat = uint32_t(DXGI_FORMAT_R16G16_FLOAT);
    o.depthBufferFormat = uint32_t(DXGI_FORMAT_R32_FLOAT);
  }
  o.onErrorCallback = &SlApiError;
  const sl::Result r = p_slDLSSGSetOptions(g_viewport, o);
  if (r != sl::Result::eOk) {
    REXLOG_WARN("gow2_frame_generation: slDLSSGSetOptions failed: {}; frames shown as drawn", ResultName(r));
    if (on) {
      g_options_failed = true;  // not tried again (no flapping)
      sl::DLSSGOptions off{};
      off.mode = sl::DLSSGMode::eOff;
      p_slDLSSGSetOptions(g_viewport, off);
    }
    g_mode_set = sl::DLSSGMode::eOff;
    return false;
  }
  REXLOG_INFO("gow2_frame_generation: DLSS-G {}", mode == sl::DLSSGMode::eOff ? std::string("off")
                                                  : mode == sl::DLSSGMode::eDynamic
                                                      ? "dynamic, up to " + std::to_string(frames + 1) + "x"
                                                      : std::to_string(frames + 1) + "x");
  g_mode_set = mode;
  g_frames_set = frames;
  return on;
}

sl::float4 F4(float x, float y, float z, float w) { return sl::float4(x, y, z, w); }

void Normalize(float v[3]) {
  const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (l > 1e-6f) v[0] /= l, v[1] /= l, v[2] /= l;
}

// Camera constants from our frame record. Row-major, row vectors (Streamline's convention; sl_matrix_helpers.h).
sl::Constants MakeConstants(const XenosFgFrame& f) {
  sl::Constants c{};
  const float w = float(std::max(1u, f.width)), h = float(std::max(1u, f.height));
  const float aspect = w / h;
  const float ys = 1.0f / std::tan(std::max(0.01f, f.vertical_fov) * 0.5f);
  const float xs = ys / aspect;
  const float n = std::max(1e-3f, f.camera_near);
  const float far_ = std::max(n * 2.0f, f.camera_far);
  const float q = f.infinite ? 1.0f : far_ / (far_ - n);  // depth 0 near .. 1 far (not inverted)
  c.cameraViewToClip = {F4(xs, 0, 0, 0), F4(0, ys, 0, 0), F4(0, 0, q, 1), F4(0, 0, -q * n, 0)};
  sl::matrixFullInvert(c.clipToCameraView, c.cameraViewToClip);
  c.clipToLensClip = {F4(1, 0, 0, 0), F4(0, 1, 0, 0), F4(0, 0, 1, 0), F4(0, 0, 0, 1)};
  float r[3] = {f.camera_right[0], f.camera_right[1], f.camera_right[2]};
  float u[3] = {f.camera_up[0], f.camera_up[1], f.camera_up[2]};
  float fw[3] = {f.camera_forward[0], f.camera_forward[1], f.camera_forward[2]};
  Normalize(r), Normalize(u), Normalize(fw);
  const bool basis_ok = std::fabs(r[0]) + std::fabs(r[1]) + std::fabs(r[2]) > 0.5f &&
                        std::fabs(u[0]) + std::fabs(u[1]) + std::fabs(u[2]) > 0.5f &&
                        std::fabs(fw[0]) + std::fabs(fw[1]) + std::fabs(fw[2]) > 0.5f;
  const sl::float4x4 view_to_world = {F4(r[0], r[1], r[2], 0), F4(u[0], u[1], u[2], 0), F4(fw[0], fw[1], fw[2], 0),
                                      F4(f.camera_position[0], f.camera_position[1], f.camera_position[2], 1)};
  if (basis_ok && g_have_prev_camera && !f.reset) {
    sl::float4x4 view_to_prev_view, clip_to_prev_view;
    sl::calcCameraToPrevCamera(view_to_prev_view, view_to_world, g_prev_view_to_world);
    sl::matrixMul(clip_to_prev_view, c.clipToCameraView, view_to_prev_view);
    sl::matrixMul(c.clipToPrevClip, clip_to_prev_view, g_prev_view_to_clip);
  } else {
    c.clipToPrevClip = c.clipToLensClip;  // identity: no history (or no usable camera basis)
  }
  sl::matrixFullInvert(c.prevClipToClip, c.clipToPrevClip);
  g_prev_view_to_world = view_to_world;
  g_prev_view_to_clip = c.cameraViewToClip;
  g_have_prev_camera = basis_ok;
  c.jitterOffset = sl::float2(f.jitter_x, f.jitter_y);
  c.mvecScale = sl::float2(1.0f / w, 1.0f / h);  // our motion is in render pixels; Streamline wants [-1, 1]
  c.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
  c.cameraPos = sl::float3(f.camera_position[0], f.camera_position[1], f.camera_position[2]);
  c.cameraUp = sl::float3(u[0], u[1], u[2]);
  c.cameraRight = sl::float3(r[0], r[1], r[2]);
  c.cameraFwd = sl::float3(fw[0], fw[1], fw[2]);
  c.cameraNear = n;
  c.cameraFar = far_;
  c.cameraFOV = f.vertical_fov;
  c.cameraAspectRatio = aspect;
  c.motionVectorsInvalidValue = 0.0f;  // optional; never written by our motion pass
  c.depthInverted = sl::Boolean::eFalse;
  c.cameraMotionIncluded = sl::Boolean::eTrue;
  c.motionVectors3D = sl::Boolean::eFalse;
  c.reset = f.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  c.orthographicProjection = sl::Boolean::eFalse;
  c.motionVectorsDilated = sl::Boolean::eFalse;
  c.motionVectorsJittered = sl::Boolean::eFalse;
  return c;
}

// --- Swap chain (SDK patch 36) -------------------------------------------------------------------------------------

bool UseFallback(void* factory, void* queue, void* hwnd, const void* desc, void** out) {
  if (!g_fallback) return false;  // the presenter makes the usual swap chain
  REXLOG_INFO("gow2_frame_generation: falling back to AMD FSR frame generation");
  return g_fallback(factory, queue, hwnd, desc, out);
}

bool MakeSwapChain(void* factory, void* queue, void* hwnd, const void* desc, void** out) {
  std::unique_lock<std::mutex> lock(g_mutex);
  if (!g_active.load(std::memory_order_acquire)) {
    lock.unlock();
    return UseFallback(factory, queue, hwnd, desc, out);
  }
  auto fail = [&](const std::string& why) {
    Disable(why.c_str());
    lock.unlock();
    return UseFallback(factory, queue, hwnd, desc, out);
  };
  auto* q = static_cast<ID3D12CommandQueue*>(queue);
  ID3D12Device* device = nullptr;
  if (FAILED(q->GetDevice(IID_PPV_ARGS(&device))) || !device) return fail("the presenter's queue gave no device");
  const LUID luid = device->GetAdapterLuid();
  if (!g_device_set) {
    const sl::Result r = p_slSetD3DDevice(device);
    if (r != sl::Result::eOk) {
      device->Release();
      return fail(std::string("slSetD3DDevice: ") + ResultName(r));
    }
    g_device_set = true;
  }
  device->Release();  // Streamline keeps its own reference; the presenter owns the device
  sl::AdapterInfo ai{};
  ai.deviceLUID = reinterpret_cast<uint8_t*>(const_cast<LUID*>(&luid));
  ai.deviceLUIDSizeInBytes = sizeof(LUID);
  sl::Result r = p_slIsFeatureSupported(sl::kFeatureDLSS_G, ai);
  if (r != sl::Result::eOk) return fail(std::string("DLSS-G on this graphics card: ") + ResultName(r));
  r = p_slIsFeatureSupported(sl::kFeatureReflex, ai);
  if (r != sl::Result::eOk) return fail(std::string("Reflex (needed by DLSS-G) on this graphics card: ") + ResultName(r));
  if (!p_slDLSSGGetState && (!FeatureProc(p_slDLSSGGetState, sl::kFeatureDLSS_G, "slDLSSGGetState") ||
                             !FeatureProc(p_slDLSSGSetOptions, sl::kFeatureDLSS_G, "slDLSSGSetOptions")))
    return fail("sl.dlss_g has no slDLSSGGetState / slDLSSGSetOptions");
  if (!p_slReflexSetOptions && (!FeatureProc(p_slReflexSetOptions, sl::kFeatureReflex, "slReflexSetOptions") ||
                                !FeatureProc(p_slReflexSleep, sl::kFeatureReflex, "slReflexSleep") ||
                                !FeatureProc(p_slPCLSetMarker, sl::kFeaturePCL, "slPCLSetMarker")))
    return fail("sl.reflex / sl.pcl have no slReflexSetOptions / slReflexSleep / slPCLSetMarker");
  if (p_slGetFeatureVersion) {
    sl::FeatureVersion v{};
    if (p_slGetFeatureVersion(sl::kFeatureDLSS_G, v) == sl::Result::eOk)
      REXLOG_INFO("gow2_frame_generation: sl.dlss_g {} (NGX DLSS-G {}), built against Streamline {}.{}.{}",
                  v.versionSL.toStr(), v.versionNGX.toStr(), SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH);
  }
  // The presenter's factory, upgraded to Streamline's proxy (our own reference goes to the proxy).
  IDXGIFactory2* proxy = static_cast<IDXGIFactory2*>(factory);
  proxy->AddRef();
  r = p_slUpgradeInterface(reinterpret_cast<void**>(&proxy));
  if (r != sl::Result::eOk || !proxy) {
    if (proxy) proxy->Release();
    return fail(std::string("slUpgradeInterface(IDXGIFactory2): ") + ResultName(r));
  }
  g_desc = *static_cast<const DXGI_SWAP_CHAIN_DESC1*>(desc);
  IDXGISwapChain1* sc = nullptr;
  const HRESULT hr = proxy->CreateSwapChainForHwnd(q, static_cast<HWND>(hwnd), &g_desc, nullptr, nullptr, &sc);
  proxy->Release();
  if (FAILED(hr) || !sc) {
    if (sc) sc->Release();
    return fail("Streamline's swap chain was not made (HRESULT " + std::to_string(uint32_t(hr)) + ")");
  }
  g_swap_chain = sc;
  // Reflex on (DLSS-G requires it); markers are sent from now on.
  sl::ReflexOptions ro{};
  ro.mode = sl::ReflexMode::eLowLatency;
  ro.useMarkersToOptimize = true;
  r = p_slReflexSetOptions(ro);
  if (r != sl::Result::eOk)
    REXLOG_WARN("gow2_frame_generation: slReflexSetOptions failed: {} (DLSS-G may stay off)", ResultName(r));
  g_latency_on.store(true, std::memory_order_release);
  QueryState(true);
  g_mode_set = sl::DLSSGMode::eOff;
  g_frames_set = 0;
  g_options_failed = false;
  g_have_prev_camera = false;
  // HUD-less image: the renderer makes it at the generation rectangle's size; R10G10B10A2 for the HDR10 swap chain,
  // otherwise plain R8G8B8A8 (Streamline is told its format; no B8G8R8A8 byte swap as for AMD).
  g_hudless_format.store(!g_hudless_on ? 0u : g_desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ? 2u : 1u,
                         std::memory_order_relaxed);
  g_inputs_wanted.store(true, std::memory_order_relaxed);
  *out = sc;  // the creation's reference goes to the presenter
  REXLOG_INFO("gow2_frame_generation: Streamline swap chain made ({}x{}, format {}, {} buffers); NVIDIA DLSS frame "
              "generation on with the first scene frame", g_desc.Width, g_desc.Height, uint32_t(g_desc.Format),
              g_desc.BufferCount);
  return true;
}

bool SameObject(void* a, IDXGISwapChain1* b) {
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

void SwapChainChange(void* swap_chain, bool destroying) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_swap_chain || !SameObject(swap_chain, g_swap_chain)) return;
  // Off before a resize (made again with the next scene frame, at the new size) or before the swap chain goes.
  SetOptions(false, nullptr);
  g_gen_on = false;
  g_have_last_frame = false;
  g_have_prev_camera = false;
  if (!destroying) return;
  g_swap_chain = nullptr;
  g_latency_on.store(false, std::memory_order_release);  // until a new swap chain is made
  REXLOG_INFO("gow2_frame_generation: Streamline swap chain released by the presenter");
}

bool BeforePresent(void* guest_output, void* /*queue*/, const int32_t* rect) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_swap_chain || !g_active.load(std::memory_order_relaxed)) return true;
  const Inputs* in = nullptr;
  for (const Inputs& i : g_inputs)
    if (guest_output && i.resource == guest_output && i.serial && (!in || i.serial > in->serial)) in = &i;
  if (in && in->serial == g_last_presented && g_gen_on) {  // a window repaint is not a new frame
    ++g_repaints_skipped;
    return false;
  }
  const bool new_image = in && in->serial != g_last_presented;
  if (in) g_last_presented = in->serial;
  ++g_presents;
  XenosFgFrame f{};
  bool have = false;
  if (in && in->has && new_image) {
    f = in->frame;
    if (g_last_shown_frame && f.frame != g_last_shown_frame + 1) f.reset = true;
    g_last_shown_frame = f.frame;
    g_last_frame = f;
    g_have_last_frame = true;
    g_no_scene_streak = 0;
    have = true;
  } else if (g_have_last_frame && ++g_no_scene_streak <= kNoSceneFramesBeforeOff) {
    f = g_last_frame;
    f.reset = true;
    have = true;
    if (in && new_image) g_last_shown_frame = 0;
  }
  // The frame these tags belong to: the newest one our renderer submitted (the present markers use the same id).
  uint64_t index = g_submitted.load(std::memory_order_relaxed);
  if (!index) index = 1;
  if (have && index == g_last_tagged) {
    if (g_gen_on) {  // the same frame id twice would confuse DLSS-G's pacing
      ++g_repaints_skipped;
      return false;
    }
    have = false;
  }
  g_present_frame.store(index, std::memory_order_relaxed);
  if (have && (!f.depth || !f.motion || !f.width || !f.height)) have = false;
  bool generate = false;
  if (have) {
    sl::FrameToken* token = Token(index);
    if (token) {
      DXGI_SWAP_CHAIN_DESC1 sd{};
      g_swap_chain->GetDesc1(&sd);
      const uint32_t state = uint32_t(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      sl::Resource depth(sl::ResourceType::eTex2d, f.depth, state);
      sl::Resource motion(sl::ResourceType::eTex2d, f.motion, state);
      const sl::Extent render{0, 0, f.width, f.height};
      sl::Extent sub{0, 0, sd.Width, sd.Height};
      if (rect && rect[2] > 0 && rect[3] > 0 && rect[0] >= 0 && rect[1] >= 0 &&
          uint32_t(rect[0] + rect[2]) <= sd.Width && uint32_t(rect[1] + rect[3]) <= sd.Height)
        sub = sl::Extent{uint32_t(rect[1]), uint32_t(rect[0]), uint32_t(rect[2]), uint32_t(rect[3])};
      if (g_hudless_on && sub.width < 65536 && sub.height < 65536)
        g_hl_rect_size.store((sub.width << 16) | sub.height, std::memory_order_relaxed);
      std::array<sl::ResourceTag, 4> tags = {
          sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &render),
          sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &render),
          sl::ResourceTag(nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &sub),
          sl::ResourceTag(nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, nullptr)};
      uint32_t count = 3;
      sl::Resource hudless(sl::ResourceType::eTex2d, f.hudless, state);
      const sl::Extent hl_extent{0, 0, f.hudless_width, f.hudless_height};
      if (g_hudless_on && f.hudless) {
        if (f.hudless_width == sub.width && f.hudless_height == sub.height) {
          tags[3] = sl::ResourceTag(&hudless, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent,
                                    &hl_extent);
          count = 4;
          ++g_hl_used;
        } else {
          ++g_hl_mismatch;
        }
      }
      const sl::Constants consts = MakeConstants(f);
      const sl::Result rt = p_slSetTagForFrame(*token, g_viewport, tags.data(), count, nullptr);
      const sl::Result rc = rt == sl::Result::eOk ? p_slSetConstants(consts, *token, g_viewport) : rt;
      if (rc == sl::Result::eOk) {
        g_last_tagged = index;
        generate = true;
      } else {
        static uint32_t logged = 0;
        if (logged++ < 8)
          REXLOG_WARN("gow2_frame_generation: DLSS-G tags / constants refused: {}; frame not generated", ResultName(rc));
      }
    }
  }
  if (generate || g_gen_on) generate = SetOptions(generate, generate ? &f : nullptr);
  if (generate) ++g_generated;
  else ++g_paused;
  g_gen_on = generate;
  if (g_presents >= kLogAt) {
    REXLOG_INFO("gow2_frame_generation: DLSS-G: {} frames presented: generation on {}, paused {}, window repaints not "
                "presented {}{}", g_presents, g_generated, g_paused, g_repaints_skipped,
                g_hudless_on ? ", HUD-less given " + std::to_string(g_hl_used) + " (size mismatch " +
                                   std::to_string(g_hl_mismatch) + ")"
                             : std::string());
    QueryState(true);
    g_presents = g_generated = g_paused = g_repaints_skipped = g_hl_used = g_hl_mismatch = 0;
  }
  return true;
}

// --- Latency (SDK patch 35 hooks; Streamline's Reflex instead of NVAPI's) ------------------------------------------

void OnGuestSwap() {
  if (!g_latency_on.load(std::memory_order_acquire)) return;
  const uint64_t k = g_sim_frame.load(std::memory_order_relaxed);
  Marker(sl::PCLMarker::eSimulationEnd, k);
  Marker(sl::PCLMarker::eRenderSubmitStart, k);
  if (p_slReflexSleep)
    if (sl::FrameToken* t = Token(k + 1)) p_slReflexSleep(*t);
  g_sim_frame.store(k + 1, std::memory_order_relaxed);
  Marker(sl::PCLMarker::eSimulationStart, k + 1);
}

void OnInputRead() {
  const uint64_t k = g_sim_frame.load(std::memory_order_relaxed);
  if (g_input_frame.exchange(k, std::memory_order_relaxed) != k) Marker(sl::PCLMarker::eControllerInputSample, k);
}

void OnPresentBegin() {
  uint64_t k = g_present_frame.load(std::memory_order_relaxed);
  if (!k) k = g_submitted.load(std::memory_order_relaxed);
  Marker(sl::PCLMarker::ePresentStart, k);
}

void OnPresentEnd() { Marker(sl::PCLMarker::ePresentEnd, g_present_frame.load(std::memory_order_relaxed)); }

}  // namespace

bool XenosDlssFgInstall(bool hudless, XenosDlssFgFallback fallback) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_fallback = fallback;
  g_hudless_on = hudless;
  ParseMode();
  const std::wstring dir = ExeDir();
  if (dir.empty()) {
    REXLOG_WARN("gow2_frame_generation: NVIDIA DLSS frame generation not used: the exe's folder is unknown");
    return false;
  }
  // Every file Streamline needs for DLSS-G (NVIDIA's, never ours: the player copies them next to the exe).
  static const wchar_t* const kFiles[] = {L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss_g.dll",
                                          L"sl.reflex.dll",     L"sl.pcl.dll",    L"nvngx_dlssg.dll"};
  std::string missing;
  for (const wchar_t* f : kFiles)
    if (GetFileAttributesW((dir + f).c_str()) == INVALID_FILE_ATTRIBUTES) missing += (missing.empty() ? "" : ", ") + Narrow(f);
  if (!missing.empty()) {
    REXLOG_WARN("gow2_frame_generation: NVIDIA DLSS frame generation not used: not next to the exe: {}", missing);
    return false;
  }
  const std::wstring interposer = dir + L"sl.interposer.dll";
  if (!sl::security::verifyEmbeddedSignature(interposer.c_str())) {
    REXLOG_WARN("gow2_frame_generation: NVIDIA DLSS frame generation not used: sl.interposer.dll is not signed by "
                "NVIDIA");
    return false;
  }
  g_module = LoadLibraryExW(interposer.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!g_module) {
    REXLOG_WARN("gow2_frame_generation: NVIDIA DLSS frame generation not used: sl.interposer.dll did not load "
                "(error {})", uint32_t(GetLastError()));
    return false;
  }
  if (!Proc(p_slInit, "slInit") || !Proc(p_slShutdown, "slShutdown") ||
      !Proc(p_slIsFeatureSupported, "slIsFeatureSupported") || !Proc(p_slSetTagForFrame, "slSetTagForFrame") ||
      !Proc(p_slSetConstants, "slSetConstants") || !Proc(p_slUpgradeInterface, "slUpgradeInterface") ||
      !Proc(p_slGetFeatureFunction, "slGetFeatureFunction") || !Proc(p_slGetNewFrameToken, "slGetNewFrameToken") ||
      !Proc(p_slSetD3DDevice, "slSetD3DDevice")) {
    REXLOG_WARN("gow2_frame_generation: NVIDIA DLSS frame generation not used: sl.interposer.dll is too old (an "
                "entry point is missing; Streamline 2.x needed)");
    FreeLibrary(g_module), g_module = nullptr;
    return false;
  }
  Proc(p_slGetFeatureVersion, "slGetFeatureVersion");  // optional (logging)
  static const std::wstring plugin_dir = dir;
  static const wchar_t* plugin_paths[] = {plugin_dir.c_str()};
  static const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
  sl::Preferences pref{};
  pref.showConsole = false;
  pref.logLevel = sl::LogLevel::eDefault;
  pref.pathsToPlugins = plugin_paths;
  pref.numPathsToPlugins = 1;
  pref.logMessageCallback = &SlLog;
  // Manual hooking (only the swap chain goes through Streamline); no over-the-air plugin downloads.
  // eUseFrameBasedResourceTagging: required for slSetTagForFrame (found by the RTX 30 test, 7 Nov: every tag was refused)
  pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
               sl::PreferenceFlags::eUseFrameBasedResourceTagging;
  pref.featuresToLoad = features;
  pref.numFeaturesToLoad = uint32_t(std::size(features));
  pref.engine = sl::EngineType::eCustom;
  pref.engineVersion = "COGBox";
  pref.projectId = "5b7d1c1e-6f0e-4c39-9d3e-2c6a0e7f4b11";  // COGBox's own id (any stable GUID)
  pref.renderAPI = sl::RenderAPI::eD3D12;
  const sl::Result r = p_slInit(pref, sl::kSDKVersion);
  if (r != sl::Result::eOk) {
    REXLOG_WARN("gow2_frame_generation: NVIDIA DLSS frame generation not used: slInit failed: {}", ResultName(r));
    FreeLibrary(g_module), g_module = nullptr;
    p_slInit = nullptr;
    return false;
  }
  g_active.store(true, std::memory_order_release);
  rex::FrameHooks& h = rex::GetFrameHooks();
  h.create_swap_chain.store(&MakeSwapChain, std::memory_order_release);
  h.swap_chain_change.store(&SwapChainChange, std::memory_order_release);
  h.before_present.store(&BeforePresent, std::memory_order_release);
  h.guest_swap.store(&OnGuestSwap, std::memory_order_release);
  h.input_read.store(&OnInputRead, std::memory_order_release);
  h.present_begin.store(&OnPresentBegin, std::memory_order_release);
  h.present_end.store(&OnPresentEnd, std::memory_order_release);
  REXLOG_INFO("gow2_frame_generation: NVIDIA Streamline {}.{}.{} started (manual hooking); DLSS frame generation {} "
              "(experimental), Reflex through sl.reflex; support is checked when the swap chain is made",
              SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH,
              g_requested_dynamic ? std::string("dynamic") : std::to_string(g_requested_frames + 1) + "x");
  return true;
}

bool XenosDlssFgActive() { return g_active.load(std::memory_order_acquire); }
bool XenosDlssFgOwnsLatency() { return g_active.load(std::memory_order_acquire); }
bool XenosDlssFgWanted() { return g_inputs_wanted.load(std::memory_order_relaxed); }
uint32_t XenosDlssFgHudlessFormat() {
  return g_inputs_wanted.load(std::memory_order_relaxed) ? g_hudless_format.load(std::memory_order_relaxed) : 0u;
}
bool XenosDlssFgHudlessSize(uint32_t& width, uint32_t& height) {
  const uint32_t v = g_hl_rect_size.load(std::memory_order_relaxed);
  width = v >> 16, height = v & 0xFFFFu;
  return v != 0;
}

void XenosDlssFgSetInputs(void* guest_output_resource, const XenosFgFrame* frame) {
  if (!guest_output_resource || !g_inputs_wanted.load(std::memory_order_relaxed)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  Inputs* slot = nullptr;
  for (Inputs& i : g_inputs)
    if (i.resource == guest_output_resource) slot = &i;
  if (!slot) {
    slot = &g_inputs[0];
    for (Inputs& i : g_inputs)
      if (i.serial < slot->serial) slot = &i;
  }
  slot->resource = guest_output_resource;
  slot->has = frame != nullptr;
  if (frame) slot->frame = *frame;
  slot->serial = ++g_serial;
}

void XenosDlssFgFrameSubmitted() {
  if (!g_latency_on.load(std::memory_order_acquire)) {
    g_submitted.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint64_t k = g_submitted.fetch_add(1, std::memory_order_relaxed) + 1;
  Marker(sl::PCLMarker::eRenderSubmitEnd, k);
}

#else  // !GN_HAVE_STREAMLINE

bool XenosDlssFgInstall(bool, XenosDlssFgFallback) {
  REXLOG_INFO("gow2_frame_generation: this build has no NVIDIA Streamline support (GN_WITH_STREAMLINE was off); "
              "no DLSS frame generation");
  return false;
}
bool XenosDlssFgActive() { return false; }
bool XenosDlssFgOwnsLatency() { return false; }
bool XenosDlssFgWanted() { return false; }
uint32_t XenosDlssFgHudlessFormat() { return 0; }
bool XenosDlssFgHudlessSize(uint32_t&, uint32_t&) { return false; }
void XenosDlssFgSetInputs(void*, const XenosFgFrame*) {}
void XenosDlssFgFrameSubmitted() {}

#endif  // GN_HAVE_STREAMLINE
