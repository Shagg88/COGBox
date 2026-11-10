// Layer 0 (game-free): Intel XeSS frame generation (XeSS-FG, "XeFG") with Xe Low Latency (XeLL). EXPERIMENTAL,
// D3D12 only. --gow2_frame_generation=xess selects it. Plan and test list: docs\plan-049-xefg.md (COGBox 0.4.9).
//
// Targets the XeSS 3 SDK v3.0.2 (third_party\xess-sdk-3.0.2: xess_fg/xefg_swapchain*.h, xell/xell*.h; XeLL 1.3.2).
// No import library: libxess_fg.dll and libxell.dll are loaded at run time from the exe's folder (after their
// Authenticode signature is checked), so the exe starts without them and every other choice is unchanged.
// Intel's licence (Intel Simplified Software License, Oct 2022, LICENSE.txt in the SDK) lets us redistribute the
// binaries unmodified with its notice; THIRD_PARTY_NOTICES.md and the release's licenses\xess-sdk-3.0.2\ carry it.
//
// Who may run it: Intel says XeSS-FG runs on Intel Arc (discrete and integrated) and on non-Intel GPUs with Shader
// Model 6.4 (since SDK 2.1.1 the XMX requirement is gone); multi frame generation (3x / 4x) is Arc only, so COGBox asks
// for one generated frame (2x) everywhere. XeLL is required by XeSS-FG and works on non-Intel GPUs only with it.
//
// How it hooks in - the same SDK patch 36 hooks as AMD FSR frame generation (patches 0146/0147) and DLSS-G:
//  create_swap_chain: xellD3D12CreateContext + xefgSwapChainD3D12CreateContext on the presenter's device,
//    xefgSwapChainSetLatencyReduction, then xefgSwapChainD3D12InitFromSwapChainDesc with the presenter's own
//    description, factory, queue and window; the presenter gets the IDXGISwapChain1 from xefgSwapChainD3D12GetSwapChainPtr
//    (Present / GetBuffer / ResizeBuffers go through Intel's proxy).
//  before_present: per new game image, xefgSwapChainTagFrameConstants (view / projection, jitter, motion scale, reset),
//    xefgSwapChainD3D12TagFrameResource for depth, motion (render size), the back-buffer generation rectangle and the
//    HUD-less image (patch 0148, when it matches the rectangle), all valid until the next Present; then
//    xefgSwapChainSetPresentId + xefgSwapChainSetEnabled. A repaint of an image already shown is not presented.
//  swap_chain_change: generation off before a resize; on destroy, the XeFG and XeLL contexts go before the presenter
//    drops its last reference (as AMD's order).
//  guest_swap / input_read / present_begin / present_end + XenosXefgFrameSubmitted: XeLL markers and xellSleep (the
//    same places latency_reflex.cpp sends NVAPI Reflex markers), with the frame ids the tags use.
//
// Fails safe: Wine / Proton, a missing or unsigned DLL, a missing entry point, a context or init error (unsupported
// device / driver, no SM 6.4) -> the reason is logged and the fallback runs (AMD FSR frame generation when the build has
// it; otherwise the usual swap chain), exactly as DLSS-G does. Our code (MIT).
#include "xess_framegen.h"
#include "fg_shown_frames.h"  // the FPS bar's shown-frame count

#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/frame_hooks.h>
#include <rex/logging/macros.h>

#include <string>

REXCVAR_DEFINE_BOOL(gow2_xefg_transpose_matrices, false, "Gears2",
                    "Intel XeSS frame generation (XeSS-FG, experimental): hand the view / projection matrices transposed "
                    "(column-major). A test switch for Intel Arc testers: only if generated frames smear on camera turns");
REXCVAR_DEFINE_INT32(gow2_xefg_log_level, 2, "Gears2",
                     "Intel XeSS frame generation: the XeSS-FG / XeLL messages written to the log (0 debug, 1 info, "
                     "2 warnings, 3 errors)");

#if defined(GN_HAVE_XEFG) && defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <softpub.h>
#include <wintrust.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <xess_fg/xefg_swapchain.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#include <xell/xell.h>
#include <xell/xell_d3d12.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <mutex>
#include <shared_mutex>

namespace {

std::mutex g_mutex;
HMODULE g_fg_module = nullptr;
HMODULE g_ll_module = nullptr;
bool g_hudless_on = false;
XenosXefgFallback g_fallback = nullptr;
std::atomic<bool> g_active{false};
std::atomic<bool> g_latency_on{false};
std::atomic<bool> g_inputs_wanted{false};
std::atomic<uint32_t> g_hudless_format{0};
std::atomic<uint32_t> g_hl_rect_size{0};

// libxess_fg.dll
decltype(&xefgSwapChainGetVersion) p_GetVersion = nullptr;
decltype(&xefgSwapChainD3D12CreateContext) p_CreateContext = nullptr;
decltype(&xefgSwapChainD3D12InitFromSwapChainDesc) p_InitFromDesc = nullptr;
decltype(&xefgSwapChainD3D12GetSwapChainPtr) p_GetSwapChainPtr = nullptr;
decltype(&xefgSwapChainD3D12TagFrameResource) p_TagResource = nullptr;
decltype(&xefgSwapChainTagFrameConstants) p_TagConstants = nullptr;
decltype(&xefgSwapChainSetEnabled) p_SetEnabled = nullptr;
decltype(&xefgSwapChainSetPresentId) p_SetPresentId = nullptr;
decltype(&xefgSwapChainGetLastPresentStatus) p_GetLastPresentStatus = nullptr;
decltype(&xefgSwapChainSetLoggingCallback) p_SetLogging = nullptr;
decltype(&xefgSwapChainDestroy) p_Destroy = nullptr;
decltype(&xefgSwapChainSetLatencyReduction) p_SetLatencyReduction = nullptr;
decltype(&xefgSwapChainGetProperties) p_GetProperties = nullptr;
decltype(&xefgSwapChainSetUiCompositionState) p_SetUiComposition = nullptr;  // optional
// libxell.dll
decltype(&xellGetVersion) p_xellGetVersion = nullptr;
decltype(&xellD3D12CreateContext) p_xellCreate = nullptr;
decltype(&xellDestroyContext) p_xellDestroy = nullptr;
decltype(&xellSetSleepMode) p_xellSetSleepMode = nullptr;
decltype(&xellSleep) p_xellSleep = nullptr;
decltype(&xellAddMarkerData) p_xellMarker = nullptr;
decltype(&xellSetLoggingCallback) p_xellSetLogging = nullptr;

xefg_swapchain_handle_t g_fg = nullptr;
xell_context_handle_t g_ll = nullptr;
// Review 049: g_ll is read on the guest thread (OnGuestSwap -> xellSleep) and by Marker() from several threads, without
// g_mutex (xellSleep must never hold g_mutex: it would block present). Every XeLL call holds g_ll_mutex shared; creating
// and destroying g_ll holds it exclusive. Order: g_mutex, then g_ll_mutex - never the other way.
std::shared_mutex g_ll_mutex;
IDXGISwapChain1* g_swap_chain = nullptr;  // Intel's proxy; the presenter holds the reference
DXGI_SWAP_CHAIN_DESC1 g_desc{};
bool g_enabled_set = false;
bool g_enable_failed = false;

std::atomic<uint64_t> g_sim_frame{1};
std::atomic<uint64_t> g_input_frame{0};
std::atomic<uint64_t> g_submitted{0};
std::atomic<uint64_t> g_present_frame{0};
uint64_t g_last_tagged = 0;

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

uint64_t g_presents = 0, g_generated = 0, g_paused = 0, g_repaints_skipped = 0, g_hl_used = 0, g_hl_mismatch = 0;
constexpr uint64_t kLogAt = 600;

const char* FgResultName(xefg_swapchain_result_t r) {
  switch (r) {
    case XEFG_SWAPCHAIN_RESULT_SUCCESS: return "success";
    case XEFG_SWAPCHAIN_RESULT_WARNING_OLD_DRIVER: return "warning: old driver";
    case XEFG_SWAPCHAIN_RESULT_WARNING_TOO_FEW_FRAMES: return "warning: too few frames";
    case XEFG_SWAPCHAIN_RESULT_WARNING_FRAMES_ID_MISMATCH: return "warning: frame id mismatch";
    case XEFG_SWAPCHAIN_RESULT_WARNING_MISSING_PRESENT_STATUS: return "warning: missing present status";
    case XEFG_SWAPCHAIN_RESULT_WARNING_RESOURCE_SIZES_MISMATCH: return "warning: resource sizes mismatch";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE: return "unsupported device (needs Intel Arc, or Shader Model 6.4)";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DRIVER: return "unsupported driver (update the graphics driver)";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNINITIALIZED: return "uninitialized";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case XEFG_SWAPCHAIN_RESULT_ERROR_DEVICE_OUT_OF_MEMORY: return "out of video memory";
    case XEFG_SWAPCHAIN_RESULT_ERROR_DEVICE: return "device error";
    case XEFG_SWAPCHAIN_RESULT_ERROR_NOT_IMPLEMENTED: return "not implemented";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_CONTEXT: return "invalid context";
    case XEFG_SWAPCHAIN_RESULT_ERROR_OPERATION_IN_PROGRESS: return "operation in progress";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED: return "unsupported";
    case XEFG_SWAPCHAIN_RESULT_ERROR_CANT_LOAD_LIBRARY: return "can't load library";
    case XEFG_SWAPCHAIN_RESULT_ERROR_MISMATCH_INPUT_RESOURCES: return "mismatched input resources";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INCORRECT_OUTPUT_RESOURCES: return "incorrect output resources";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INCORRECT_INPUT_RESOURCES: return "incorrect input resources";
    case XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_UNSUPPORTED: return "XeLL unsupported";
    case XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_FUNCTION_MISSING: return "XeLL function missing";
    case XEFG_SWAPCHAIN_RESULT_ERROR_HRESULT_FAILURE: return "HRESULT failure";
    case XEFG_SWAPCHAIN_RESULT_ERROR_DXGI_INVALID_CALL: return "DXGI invalid call";
    case XEFG_SWAPCHAIN_RESULT_ERROR_POINTER_STILL_IN_USE: return "swap chain still in use";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_DESCRIPTOR_HEAP: return "invalid descriptor heap";
    case XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER: return "wrong call order";
    default: return "unknown error";
  }
}
bool Ok(xefg_swapchain_result_t r) { return r >= XEFG_SWAPCHAIN_RESULT_SUCCESS; }  // warnings are positive

const char* LlResultName(xell_result_t r) {
  switch (r) {
    case XELL_RESULT_SUCCESS: return "success";
    case XELL_RESULT_ERROR_UNSUPPORTED_DEVICE: return "unsupported device";
    case XELL_RESULT_ERROR_UNSUPPORTED_DRIVER: return "unsupported driver";
    case XELL_RESULT_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case XELL_RESULT_ERROR_UNSUPPORTED: return "unsupported";
    default: return "error";
  }
}

void FgLog(const char* message, xefg_swapchain_logging_level_t level, void*) {
  std::string m = message ? message : "";
  while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
  if (level >= XEFG_SWAPCHAIN_LOGGING_LEVEL_ERROR) REXLOG_WARN("gow2_frame_generation: XeSS-FG: {}", m);
  else REXLOG_INFO("gow2_frame_generation: XeSS-FG: {}", m);
}

void LlLog(const char* message, xell_logging_level_t level) {
  std::string m = message ? message : "";
  while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
  if (level >= XELL_LOGGING_LEVEL_ERROR) REXLOG_WARN("gow2_frame_generation: XeLL: {}", m);
  else REXLOG_INFO("gow2_frame_generation: XeLL: {}", m);
}

template <typename T>
bool Proc(HMODULE m, T& out, const char* name) {
  out = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
  return out != nullptr;
}

std::wstring ExeDir() {
  wchar_t path[MAX_PATH * 2] = {};
  const DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  if (!n || n >= std::size(path)) return {};
  std::wstring s(path, n);
  const size_t slash = s.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : s.substr(0, slash + 1);
}

// A valid Authenticode signature (WinVerifyTrust, no network revocation check) whose signer certificate's organisation
// is Intel Corporation (review 049: any valid signer was accepted before; the custom DLSS check requires NVIDIA the same
// way). wintrust.dll / crypt32.dll from System32, loaded here so the exe gains no import.
bool SignedFile(const std::wstring& file) {
  HMODULE wt = LoadLibraryExW(L"wintrust.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!wt) return false;
  HMODULE c32 = LoadLibraryExW(L"crypt32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  using Fn = LONG(WINAPI*)(HWND, GUID*, LPVOID);
  using ProvDataFn = CRYPT_PROVIDER_DATA*(WINAPI*)(HANDLE);
  using SignerFn = CRYPT_PROVIDER_SGNR*(WINAPI*)(CRYPT_PROVIDER_DATA*, DWORD, BOOL, DWORD);
  using CertFn = CRYPT_PROVIDER_CERT*(WINAPI*)(CRYPT_PROVIDER_SGNR*, DWORD);
  using NameFn = DWORD(WINAPI*)(PCCERT_CONTEXT, DWORD, DWORD, void*, LPWSTR, DWORD);
  auto verify = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(wt, "WinVerifyTrust")));
  auto prov = reinterpret_cast<ProvDataFn>(reinterpret_cast<void*>(GetProcAddress(wt, "WTHelperProvDataFromStateData")));
  auto signer = reinterpret_cast<SignerFn>(reinterpret_cast<void*>(GetProcAddress(wt, "WTHelperGetProvSignerFromChain")));
  auto cert = reinterpret_cast<CertFn>(reinterpret_cast<void*>(GetProcAddress(wt, "WTHelperGetProvCertFromChain")));
  auto name = c32 ? reinterpret_cast<NameFn>(reinterpret_cast<void*>(GetProcAddress(c32, "CertGetNameStringW"))) : nullptr;
  bool ok = false;
  if (verify && prov && signer && cert && name) {
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = file.c_str();
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    if (verify(nullptr, &action, &wd) == ERROR_SUCCESS) {
      CRYPT_PROVIDER_DATA* pd = prov(wd.hWVTStateData);
      CRYPT_PROVIDER_SGNR* sg = pd ? signer(pd, 0, FALSE, 0) : nullptr;
      CRYPT_PROVIDER_CERT* pc = sg ? cert(sg, 0) : nullptr;
      wchar_t org[256] = {};
      if (pc && pc->pCert)
        name(pc->pCert, CERT_NAME_ATTR_TYPE, 0, const_cast<char*>(szOID_ORGANIZATION_NAME), org, 256);
      ok = std::wstring(org) == L"Intel Corporation";
      if (!ok)
        REXLOG_WARN("gow2_frame_generation: an XeSS-FG DLL is validly signed, but not by Intel Corporation");
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    verify(nullptr, &action, &wd);
  }
  if (c32) FreeLibrary(c32);
  FreeLibrary(wt);
  return ok;
}

void Marker(xell_latency_marker_type_t m, uint64_t frame) {
  if (!g_latency_on.load(std::memory_order_acquire) || !p_xellMarker || !frame) return;
  std::shared_lock<std::shared_mutex> ll(g_ll_mutex);
  if (g_ll) p_xellMarker(g_ll, uint32_t(frame), m);
}

void DestroyContexts() {
  g_latency_on.store(false, std::memory_order_release);
  if (g_fg) {
    const xefg_swapchain_result_t r = p_Destroy(g_fg);
    if (!Ok(r)) REXLOG_WARN("gow2_frame_generation: xefgSwapChainDestroy: {}", FgResultName(r));
    g_fg = nullptr;
  }
  std::unique_lock<std::shared_mutex> ll(g_ll_mutex);  // waits for an xellSleep / marker in flight
  if (g_ll) {
    p_xellDestroy(g_ll);
    g_ll = nullptr;
  }
}

// Called with the lock held: XeFG will not run here.
void Disable(const char* why) {
  REXLOG_WARN("gow2_frame_generation: Intel XeSS frame generation not used: {}", why);
  DestroyContexts();
  g_active.store(false, std::memory_order_release);
  g_inputs_wanted.store(false, std::memory_order_relaxed);
  g_hudless_format.store(0, std::memory_order_relaxed);
  rex::FrameHooks& h = rex::GetFrameHooks();
  h.before_present.store(nullptr, std::memory_order_release);
  XenosLatencyXefgFellBack();  // review 049: Reflex / Anti-Lag 2 as asked for, now that XeLL is gone
}

void Normalize(float v[3]) {
  const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (l > 1e-6f) v[0] /= l, v[1] /= l, v[2] /= l;
}

// Row-major, row vectors (DirectXMath's XMFLOAT4X4, which Intel's samples use); --gow2_xefg_transpose_matrices flips.
xefg_swapchain_frame_constant_data_t MakeConstants(const XenosFgFrame& f) {
  xefg_swapchain_frame_constant_data_t c{};
  const float w = float(std::max(1u, f.width)), h = float(std::max(1u, f.height));
  const float ys = 1.0f / std::tan(std::max(0.01f, f.vertical_fov) * 0.5f);
  const float xs = ys / (w / h);
  const float n = std::max(1e-3f, f.camera_near);
  const float far_ = std::max(n * 2.0f, f.camera_far);
  const float q = f.infinite ? 1.0f : far_ / (far_ - n);  // depth 0 near .. 1 far (not inverted)
  const float proj[16] = {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, q, 1, 0, 0, -q * n, 0};
  float r[3] = {f.camera_right[0], f.camera_right[1], f.camera_right[2]};
  float u[3] = {f.camera_up[0], f.camera_up[1], f.camera_up[2]};
  float fw[3] = {f.camera_forward[0], f.camera_forward[1], f.camera_forward[2]};
  Normalize(r), Normalize(u), Normalize(fw);
  const float* p = f.camera_position;
  // world -> view: the inverse of the (orthonormal) camera basis plus position
  const float view[16] = {r[0], u[0], fw[0], 0, r[1], u[1], fw[1], 0, r[2], u[2], fw[2], 0,
                          -(p[0] * r[0] + p[1] * r[1] + p[2] * r[2]),
                          -(p[0] * u[0] + p[1] * u[1] + p[2] * u[2]),
                          -(p[0] * fw[0] + p[1] * fw[1] + p[2] * fw[2]), 1};
  const bool t = REXCVAR_GET(gow2_xefg_transpose_matrices);
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      c.viewMatrix[i * 4 + j] = t ? view[j * 4 + i] : view[i * 4 + j];
      c.projectionMatrix[i * 4 + j] = t ? proj[j * 4 + i] : proj[i * 4 + j];
    }
  c.jitterOffsetX = f.jitter_x;
  c.jitterOffsetY = f.jitter_y;
  c.motionVectorScaleX = 1.0f;  // our motion is current -> previous in render pixels, as XeSS-SR takes it
  c.motionVectorScaleY = 1.0f;
  c.resetHistory = f.reset ? 1u : 0u;
  c.frameRenderTime = f.frame_ms;
  return c;
}

void SetEnabled(bool on) {
  if (!g_fg || g_enable_failed) return;
  if (on == g_enabled_set) return;
  const xefg_swapchain_result_t r = p_SetEnabled(g_fg, on ? 1u : 0u);
  if (!Ok(r)) {
    REXLOG_WARN("gow2_frame_generation: xefgSwapChainSetEnabled({}): {}; frames shown as drawn", on, FgResultName(r));
    if (on) g_enable_failed = true;  // not tried again (no flapping)
    g_enabled_set = false;
    return;
  }
  REXLOG_INFO("gow2_frame_generation: Intel XeSS frame generation {}", on ? "on (2x)" : "off");
  g_enabled_set = on;
}

// --- Swap chain (SDK patch 36) -------------------------------------------------------------------------------------

bool UseFallback(void* factory, void* queue, void* hwnd, const void* desc, void** out) {
  if (!g_fallback) return false;
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
  DestroyContexts();  // a swap chain made again (fullscreen toggle, device loss): fresh contexts
  auto* q = static_cast<ID3D12CommandQueue*>(queue);
  ID3D12Device* device = nullptr;
  if (FAILED(q->GetDevice(IID_PPV_ARGS(&device))) || !device) return fail("the presenter's queue gave no device");
  xell_context_handle_t ll_new = nullptr;
  const xell_result_t lr = p_xellCreate(device, &ll_new);
  if (lr != XELL_RESULT_SUCCESS || !ll_new) {
    device->Release();
    if (ll_new) p_xellDestroy(ll_new);
    return fail(std::string("xellD3D12CreateContext (XeLL, required by XeSS-FG): ") + LlResultName(lr));
  }
  const int lvl = std::clamp(int(REXCVAR_GET(gow2_xefg_log_level)), 0, 3);
  {
    std::unique_lock<std::shared_mutex> ll(g_ll_mutex);
    g_ll = ll_new;
    p_xellSetLogging(g_ll, xell_logging_level_t(lvl), &LlLog);
  }
  xefg_swapchain_result_t r = p_CreateContext(device, &g_fg);
  device->Release();  // the contexts keep their own references; the presenter owns the device
  if (!Ok(r) || !g_fg) {
    g_fg = nullptr;
    return fail(std::string("xefgSwapChainD3D12CreateContext: ") + FgResultName(r));
  }
  p_SetLogging(g_fg, xefg_swapchain_logging_level_t(lvl), &FgLog, nullptr);
  {
    std::shared_lock<std::shared_mutex> ll(g_ll_mutex);
    r = p_SetLatencyReduction(g_fg, g_ll);
  }
  if (!Ok(r)) return fail(std::string("xefgSwapChainSetLatencyReduction: ") + FgResultName(r));
  g_desc = *static_cast<const DXGI_SWAP_CHAIN_DESC1*>(desc);
  xefg_swapchain_d3d12_init_params_t ip{};
  ip.pApplicationSwapChain = nullptr;  // made from the description
  ip.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_NONE;  // depth 0 near; low-res motion in pixels, not jittered
  ip.maxInterpolatedFrames = 1;                  // 2x: multi frame generation is Intel Arc only (not offered yet)
  ip.uiMode = XEFG_SWAPCHAIN_UI_MODE_AUTO;
  r = p_InitFromDesc(g_fg, static_cast<HWND>(hwnd), &g_desc, nullptr, q, static_cast<IDXGIFactory2*>(factory), &ip);
  if (!Ok(r)) return fail(std::string("xefgSwapChainD3D12InitFromSwapChainDesc: ") + FgResultName(r));
  IDXGISwapChain1* sc = nullptr;
  r = p_GetSwapChainPtr(g_fg, __uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc));
  if (!Ok(r) || !sc) {
    if (sc) sc->Release();
    return fail(std::string("xefgSwapChainD3D12GetSwapChainPtr: ") + FgResultName(r));
  }
  g_swap_chain = sc;
  // XeLL: low-latency mode on, no fps cap of its own (the launcher's frame-rate cap limits the drawn frames)
  xell_sleep_params_t sp{};
  sp.bLowLatencyMode = 1;
  xell_result_t sr = XELL_RESULT_SUCCESS;
  {
    std::shared_lock<std::shared_mutex> ll(g_ll_mutex);
    if (g_ll) sr = p_xellSetSleepMode(g_ll, &sp);
  }
  if (sr != XELL_RESULT_SUCCESS)
    REXLOG_WARN("gow2_frame_generation: xellSetSleepMode: {} (XeLL pacing may be off)", LlResultName(sr));
  if (p_GetProperties) {
    xefg_swapchain_properties_t pr{};
    if (Ok(p_GetProperties(g_fg, &pr)))
      REXLOG_INFO("gow2_frame_generation: XeSS-FG properties: up to {} interpolated frames per drawn frame on this card "
                  "(COGBox asks for 1), {} descriptors", pr.maxSupportedInterpolations, pr.requiredDescriptorCount);
  }
  g_enabled_set = false;
  g_enable_failed = false;
  g_latency_on.store(true, std::memory_order_release);
  // HUD-less image: the generation rectangle's size; R10G10B10A2 for the HDR10 swap chain, otherwise plain R8G8B8A8
  // (as DLSS-G: XeSS-FG samples it as colour, so no B8G8R8A8 byte swap as for AMD).
  const uint32_t hl = !g_hudless_on ? 0u : g_desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ? 2u : 1u;
  g_hudless_format.store(hl, std::memory_order_relaxed);
  g_inputs_wanted.store(true, std::memory_order_relaxed);
  *out = sc;  // GetSwapChainPtr's reference goes to the presenter
  REXLOG_INFO("gow2_frame_generation: XeSS-FG swap chain made ({}x{}, format {}, {} buffers); Intel XeSS frame generation "
              "(experimental) on with the first scene frame, XeLL low latency on", g_desc.Width, g_desc.Height,
              uint32_t(g_desc.Format), g_desc.BufferCount);
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
  SetEnabled(false);
  g_gen_on = false;
  g_have_last_frame = false;
  if (!destroying) return;
  g_swap_chain = nullptr;
  DestroyContexts();  // before the presenter drops the last reference (XeSS-FG owns the real swap chain)
  REXLOG_INFO("gow2_frame_generation: XeSS-FG swap chain released by the presenter");
}

bool BeforePresent(void* guest_output, void* /*queue*/, const int32_t* rect) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_swap_chain || !g_fg || !g_active.load(std::memory_order_relaxed)) return true;
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
  uint64_t index = g_submitted.load(std::memory_order_relaxed);
  if (!index) index = 1;
  if (have && index == g_last_tagged) {
    if (g_gen_on) {  // the same present id twice would confuse XeSS-FG's pacing
      ++g_repaints_skipped;
      return false;
    }
    have = false;
  }
  g_present_frame.store(index, std::memory_order_relaxed);
  if (have && (!f.depth || !f.motion || !f.width || !f.height)) have = false;
  bool generate = false;
  const uint32_t id = uint32_t(index);
  if (have) {
    DXGI_SWAP_CHAIN_DESC1 sd{};
    g_swap_chain->GetDesc1(&sd);
    const D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    xefg_swapchain_2d_t sub_base{0, 0}, sub_size{sd.Width, sd.Height};
    if (rect && rect[2] > 0 && rect[3] > 0 && rect[0] >= 0 && rect[1] >= 0 &&
        uint32_t(rect[0] + rect[2]) <= sd.Width && uint32_t(rect[1] + rect[3]) <= sd.Height) {
      sub_base = {uint32_t(rect[0]), uint32_t(rect[1])};
      sub_size = {uint32_t(rect[2]), uint32_t(rect[3])};
    }
    if (g_hudless_on && sub_size.x < 65536 && sub_size.y < 65536)
      g_hl_rect_size.store((sub_size.x << 16) | sub_size.y, std::memory_order_relaxed);
    auto tag = [&](xefg_swapchain_resource_type_t type, void* res, xefg_swapchain_2d_t base, xefg_swapchain_2d_t size) {
      xefg_swapchain_d3d12_resource_data_t d{};
      d.type = type;
      d.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;  // kept until the same guest output is written again
      d.resourceBase = base;
      d.resourceSize = size;
      d.pResource = static_cast<ID3D12Resource*>(res);
      d.incomingState = state;
      return p_TagResource(g_fg, nullptr, id, &d);
    };
    const xefg_swapchain_frame_constant_data_t consts = MakeConstants(f);
    xefg_swapchain_result_t r = p_TagConstants(g_fg, id, &consts);
    const xefg_swapchain_2d_t zero{0, 0}, render{f.width, f.height};
    if (Ok(r)) r = tag(XEFG_SWAPCHAIN_RES_DEPTH, f.depth, zero, render);
    if (Ok(r)) r = tag(XEFG_SWAPCHAIN_RES_MOTION_VECTOR, f.motion, zero, render);
    if (Ok(r)) r = tag(XEFG_SWAPCHAIN_RES_BACKBUFFER, nullptr, sub_base, sub_size);  // the generation rectangle only
    if (Ok(r) && g_hudless_on && f.hudless) {
      if (f.hudless_width == sub_size.x && f.hudless_height == sub_size.y) {
        r = tag(XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, f.hudless, zero, xefg_swapchain_2d_t{f.hudless_width, f.hudless_height});
        ++g_hl_used;
      } else {
        ++g_hl_mismatch;
      }
    }
    if (Ok(r)) {
      g_last_tagged = index;
      generate = true;
    } else {
      static uint32_t logged = 0;
      if (logged++ < 8)
        REXLOG_WARN("gow2_frame_generation: XeSS-FG tags / constants refused: {}; frame not generated", FgResultName(r));
    }
  }
  p_SetPresentId(g_fg, id);  // the id the next Present belongs to (the tags above and the XeLL markers use it)
  SetEnabled(generate);
  generate = generate && g_enabled_set;
  if (generate) ++g_generated;
  else ++g_paused;
  g_gen_on = generate;
  XenosFgCountShown(generate ? 2 : 1, generate);  // XeSS-FG: COGBox asks for one interpolated frame (2x)
  if (g_presents >= kLogAt) {
    xefg_swapchain_present_status_t ps{};
    const bool st = p_GetLastPresentStatus && Ok(p_GetLastPresentStatus(g_fg, &ps));
    REXLOG_INFO("gow2_frame_generation: XeSS-FG: {} frames presented: generation on {}, paused {}, window repaints not "
                "presented {}{}{}", g_presents, g_generated, g_paused, g_repaints_skipped,
                g_hudless_on ? ", HUD-less given " + std::to_string(g_hl_used) + " (size mismatch " +
                                   std::to_string(g_hl_mismatch) + ")"
                             : std::string(),
                st ? "; last Present: " + std::to_string(ps.framesPresented) + " frames shown, generation " +
                         (ps.isFrameGenEnabled ? "on" : "off") + ", result " + FgResultName(ps.frameGenResult)
                   : std::string());
    g_presents = g_generated = g_paused = g_repaints_skipped = g_hl_used = g_hl_mismatch = 0;
  }
  return true;
}

// --- Latency (XeLL markers on the SDK patch 35 hooks, where NVAPI Reflex sends its own) ----------------------------

void OnGuestSwap() {
  if (!g_latency_on.load(std::memory_order_acquire)) return;
  const uint64_t k = g_sim_frame.load(std::memory_order_relaxed);
  Marker(XELL_SIMULATION_END, k);
  Marker(XELL_RENDERSUBMIT_START, k);
  if (p_xellSleep) {
    std::shared_lock<std::shared_mutex> ll(g_ll_mutex);  // never g_mutex here: present must not wait on this sleep
    if (g_ll && g_latency_on.load(std::memory_order_acquire)) p_xellSleep(g_ll, uint32_t(k + 1));
  }
  g_sim_frame.store(k + 1, std::memory_order_relaxed);
  Marker(XELL_SIMULATION_START, k + 1);
}

void OnInputRead() {
  const uint64_t k = g_sim_frame.load(std::memory_order_relaxed);
  if (g_input_frame.exchange(k, std::memory_order_relaxed) != k) Marker(XELL_INPUT_SAMPLE, k);
}

void OnPresentBegin() {
  uint64_t k = g_present_frame.load(std::memory_order_relaxed);
  if (!k) k = g_submitted.load(std::memory_order_relaxed);
  Marker(XELL_PRESENT_START, k);
}

void OnPresentEnd() { Marker(XELL_PRESENT_END, g_present_frame.load(std::memory_order_relaxed)); }

// XeSS-FG and XeLL are untested under Wine / Proton (and DirectX 12 frame generation is off there anyway).
const char* NotWindowsReason() {
  if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"); nt && GetProcAddress(nt, "wine_get_version"))
    return "running under Wine / Proton";
  return nullptr;
}

std::string Narrow(const std::wstring& w) {
  std::string s;
  for (wchar_t c : w) s += c < 128 ? char(c) : '?';
  return s;
}

}  // namespace

bool XenosXefgInstall(bool hudless, XenosXefgFallback fallback) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_fallback = fallback;
  g_hudless_on = hudless;
  auto refuse = [](const std::string& why) {
    REXLOG_WARN("gow2_frame_generation: Intel XeSS frame generation not used: {}", why);
    return false;
  };
  if (const char* why = NotWindowsReason()) return refuse(why);
  const std::wstring dir = ExeDir();
  if (dir.empty()) return refuse("the exe's folder is unknown");
  std::string missing;
  for (const wchar_t* f : {L"libxess_fg.dll", L"libxell.dll"})
    if (GetFileAttributesW((dir + f).c_str()) == INVALID_FILE_ATTRIBUTES) missing += (missing.empty() ? "" : ", ") + Narrow(f);
  if (!missing.empty()) return refuse("not next to the exe: " + missing);
  const std::wstring fg_path = dir + L"libxess_fg.dll", ll_path = dir + L"libxell.dll";
  if (!SignedFile(fg_path) || !SignedFile(ll_path)) return refuse("libxess_fg.dll / libxell.dll have no valid signature");
  g_ll_module = LoadLibraryExW(ll_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  g_fg_module = g_ll_module ? LoadLibraryExW(fg_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) : nullptr;
  auto unload = [] {
    if (g_fg_module) FreeLibrary(g_fg_module), g_fg_module = nullptr;
    if (g_ll_module) FreeLibrary(g_ll_module), g_ll_module = nullptr;
  };
  if (!g_fg_module) {
    const DWORD e = GetLastError();
    unload();
    return refuse("libxess_fg.dll / libxell.dll did not load (error " + std::to_string(uint32_t(e)) + ")");
  }
  HMODULE fg = g_fg_module, ll = g_ll_module;
  if (!Proc(fg, p_GetVersion, "xefgSwapChainGetVersion") || !Proc(fg, p_CreateContext, "xefgSwapChainD3D12CreateContext") ||
      !Proc(fg, p_InitFromDesc, "xefgSwapChainD3D12InitFromSwapChainDesc") ||
      !Proc(fg, p_GetSwapChainPtr, "xefgSwapChainD3D12GetSwapChainPtr") ||
      !Proc(fg, p_TagResource, "xefgSwapChainD3D12TagFrameResource") ||
      !Proc(fg, p_TagConstants, "xefgSwapChainTagFrameConstants") || !Proc(fg, p_SetEnabled, "xefgSwapChainSetEnabled") ||
      !Proc(fg, p_SetPresentId, "xefgSwapChainSetPresentId") || !Proc(fg, p_SetLogging, "xefgSwapChainSetLoggingCallback") ||
      !Proc(fg, p_Destroy, "xefgSwapChainDestroy") || !Proc(fg, p_SetLatencyReduction, "xefgSwapChainSetLatencyReduction") ||
      !Proc(ll, p_xellGetVersion, "xellGetVersion") || !Proc(ll, p_xellCreate, "xellD3D12CreateContext") ||
      !Proc(ll, p_xellDestroy, "xellDestroyContext") || !Proc(ll, p_xellSetSleepMode, "xellSetSleepMode") ||
      !Proc(ll, p_xellSleep, "xellSleep") || !Proc(ll, p_xellMarker, "xellAddMarkerData") ||
      !Proc(ll, p_xellSetLogging, "xellSetLoggingCallback")) {
    unload();
    return refuse("libxess_fg.dll / libxell.dll are too old (an entry point is missing; the XeSS 2 SDK or newer is needed)");
  }
  Proc(fg, p_GetLastPresentStatus, "xefgSwapChainGetLastPresentStatus");  // optional (logging)
  Proc(fg, p_GetProperties, "xefgSwapChainGetProperties");                // optional (logging)
  xefg_swapchain_version_t fv{};
  xell_version_t lv{};
  p_GetVersion(&fv);
  p_xellGetVersion(&lv);
  g_active.store(true, std::memory_order_release);
  rex::FrameHooks& h = rex::GetFrameHooks();
  h.create_swap_chain.store(&MakeSwapChain, std::memory_order_release);
  h.swap_chain_change.store(&SwapChainChange, std::memory_order_release);
  h.before_present.store(&BeforePresent, std::memory_order_release);
  h.guest_swap.store(&OnGuestSwap, std::memory_order_release);
  h.input_read.store(&OnInputRead, std::memory_order_release);
  h.present_begin.store(&OnPresentBegin, std::memory_order_release);
  h.present_end.store(&OnPresentEnd, std::memory_order_release);
  REXLOG_INFO("gow2_frame_generation: Intel XeSS-FG {}.{}.{} and XeLL {}.{}.{} loaded; Intel XeSS frame generation 2x "
              "(experimental), XeLL low latency; support is checked when the swap chain is made",
              fv.major, fv.minor, fv.patch, lv.major, lv.minor, lv.patch);
  return true;
}

bool XenosXefgActive() { return g_active.load(std::memory_order_acquire); }
bool XenosXefgOwnsLatency() { return g_active.load(std::memory_order_acquire); }
bool XenosXefgWanted() { return g_inputs_wanted.load(std::memory_order_relaxed); }
uint32_t XenosXefgHudlessFormat() {
  return g_inputs_wanted.load(std::memory_order_relaxed) ? g_hudless_format.load(std::memory_order_relaxed) : 0u;
}
bool XenosXefgHudlessSize(uint32_t& width, uint32_t& height) {
  const uint32_t v = g_hl_rect_size.load(std::memory_order_relaxed);
  width = v >> 16, height = v & 0xFFFFu;
  return v != 0;
}

void XenosXefgSetInputs(void* guest_output_resource, const XenosFgFrame* frame) {
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

void XenosXefgFrameSubmitted() {
  const uint64_t k = g_submitted.fetch_add(1, std::memory_order_relaxed) + 1;
  Marker(XELL_RENDERSUBMIT_END, k);
}

#else  // !GN_HAVE_XEFG

bool XenosXefgInstall(bool, XenosXefgFallback) {
  REXLOG_INFO("gow2_frame_generation: this build has no Intel XeSS frame generation (the XeSS 3 SDK's xess_fg / xell "
              "headers were not in third_party when built)");
  return false;
}
bool XenosXefgActive() { return false; }
bool XenosXefgOwnsLatency() { return false; }
bool XenosXefgWanted() { return false; }
uint32_t XenosXefgHudlessFormat() { return 0; }
bool XenosXefgHudlessSize(uint32_t&, uint32_t&) { return false; }
void XenosXefgSetInputs(void*, const XenosFgFrame*) {}
void XenosXefgFrameSubmitted() {}

#endif  // GN_HAVE_XEFG
