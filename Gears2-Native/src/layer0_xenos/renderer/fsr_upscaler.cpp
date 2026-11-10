// Layer 0 (game-free): AMD FSR (the AMD FSR SDK's upscaler, ffx_api) behind upscale::Upscaler (upscaler.h, notes/64).
// Built only when the SDK is in third_party\fidelityfx-sdk (GN_HAVE_FSR): its ffx_api headers at build time; at run
// time amd_fidelityfx_loader_dx12.dll + amd_fidelityfx_upscaler_dx12.dll (D3D12) or amd_fidelityfx_loader_vk.dll +
// amd_fidelityfx_upscaler_vk.dll (Vulkan, fsr049) next to the exe, loaded with LoadLibrary - no import library.
// fsr049 (AMD FSR SDK 2.4.0): the upscaler DLL holds FSR Upscaling 4.1.2 (ML; RX 9000 FP8, RX 7000 / 6000 INT8, Strix
// Point / Z2 the light INT8 model) and FSR 3.1.6 (analytical, any GPU). With no override the SDK picks the newest
// version this GPU (and driver) supports, so FSR 4 on RX 6000 or newer, FSR 3.1 otherwise; --gow2_fsr_version pins one.
// Since SDK 2.1 the context must be made with ffxCreateContextDescUpscaleVersion chained (FFX_UPSCALER_VERSION).
// Never committed (AMD's licence, docs/license.md). Our code (MIT); the SDK's signed DLLs are AMD's (binary licence).
#if defined(GN_HAVE_FSR)

#include "xenos/renderer/upscaler.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "shared_dlls.h"  // shared049: vendor DLLs from <COGBox>\shared when not on the search path
#include <d3d12.h>

#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <ffx_api_types.h>
#include <ffx_upscale.h>
#include <dx12/ffx_api_dx12.h>

#include "fsr_vulkan.h"  // fsr049: the Vulkan half (its own file: ffx_api_vk.h clashes with ffx_api_dx12.h)

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include <format>
#include <string>
#include <vector>

REXCVAR_DEFINE_STRING(gow2_fsr_version, "auto", "Gears2",
                      "AMD FSR upscaler version: auto = the SDK's choice (FSR 4 on RX 6000 or newer, FSR 3.1 otherwise), "
                      "3 = newest FSR 3.x, 4 = newest FSR 4.x (auto when this GPU has none), or an exact version name "
                      "from the log");

namespace xenos::renderer::upscale {
namespace {

// fsr049: every upscaler version the loader offers on this device (shipped DLL + driver-provided), for the log and the
// --gow2_fsr_version override. device: ID3D12Device* or VkDeviceContext*.
struct FsrVersions {
  std::vector<uint64_t> ids;
  std::vector<std::string> names;
};
FsrVersions QueryVersions(PfnFfxQuery query, void* device) {
  FsrVersions v;
  if (!query || !device) return v;
  ffxQueryDescGetVersions q{};
  q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  q.device = device;
  uint64_t count = 0;
  q.outputCount = &count;
  if (query(nullptr, &q.header) != FFX_API_RETURN_OK || !count || count > 64) return v;
  std::vector<uint64_t> ids(count);
  std::vector<const char*> names(count, nullptr);
  q.versionIds = ids.data();
  q.versionNames = names.data();
  if (query(nullptr, &q.header) != FFX_API_RETURN_OK) return v;
  for (uint64_t i = 0; i < count; ++i) {
    v.ids.push_back(ids[i]);
    v.names.push_back(names[i] ? names[i] : "?");
  }
  return v;
}
// The id --gow2_fsr_version asks for (0 = no override, the SDK's choice).
uint64_t PickVersion(const FsrVersions& v, const std::string& want) {
  if (want.empty() || want == "auto") return 0;
  uint64_t best = 0;
  std::string best_name;
  for (size_t i = 0; i < v.ids.size(); ++i) {
    const std::string& n = v.names[i];
    if (n == want) return v.ids[i];
    if ((want == "3" || want == "4") && n.size() > 1 && n[0] == want[0] && n[1] == '.' && n > best_name)
      best = v.ids[i], best_name = n;
  }
  return best;
}
std::string JoinVersions(const FsrVersions& v) {
  std::string out;
  for (size_t i = 0; i < v.names.size(); ++i) out += (i ? ", " : "") + v.names[i];
  return out.empty() ? std::string("none listed") : out;
}

constexpr uint32_t kStateShaderRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr uint32_t kStateUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

void FfxMessage(uint32_t type, const wchar_t* message) {
  std::string m;
  for (const wchar_t* p = message; p && *p; ++p) m += *p < 128 ? char(*p) : '?';
  if (type == FFX_API_MESSAGE_TYPE_ERROR) REXLOG_WARN("upscale: FSR: {}", m);
  else REXLOG_INFO("upscale: FSR: {}", m);
}

// 0.2.0 (GitHub #6): an RX 6800 crashed inside AMD's driver (amdxc64.dll) during an FSR dispatch, the graphics card
// lost. The dispatch runs under a structured-exception guard: if it faults, the game says so and closes with a code
// COGBox knows, and COGBox turns FSR off for that game (SMAA instead) - instead of a bare crash.
ffxReturnCode_t GuardedDispatch(PfnFfxDispatch dispatch, ffxContext* context, ffxDispatchDescHeader* desc, bool* faulted) {
  __try {
    return dispatch(context, desc);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *faulted = true;
    return FFX_API_RETURN_ERROR;
  }
}

void FsrFatal() {
  REXLOG_ERROR("upscale: FSR crashed inside the graphics driver - closing; COGBox turns FSR off for this game");
  MessageBoxW(nullptr,
              L"AMD FSR crashed inside the graphics driver, so the game will close.\n\nCOGBox will switch this game to SMAA "
              L"(no upscaler). Updating the graphics driver may let FSR work again - you can turn it back on in the game's "
              L"Graphics tab.",
              L"COGBox - FSR", MB_ICONWARNING | MB_OK | MB_TOPMOST);
  ExitProcess(0xE0C0B00Au);   // COGBox: turn FSR off for this game
}

class FsrUpscaler final : public Upscaler {
 public:
  ~FsrUpscaler() override {
    if (context_ && fn_.DestroyContext) fn_.DestroyContext(&context_, nullptr);
    for (Retired& r : retired_) if (fn_.DestroyContext) fn_.DestroyContext(&r.context, nullptr);   // shutdown: the GPU is idle
    context_ = nullptr;
    fsrvk::DeleteBackend(vk_backend_);
    if (module_) FreeLibrary(module_);
  }
  Kind kind() const override { return Kind::kFsr; }
  std::string Status() const override { return status_; }

  bool Configure(gpu::Backend& backend, uint32_t render_width, uint32_t render_height, uint32_t output_width,
                 uint32_t output_height, bool hdr, bool depth_infinite, bool depth_inverted) override {
    if (failed_) return false;
    if (context_ && render_width == rw_ && render_height == rh_ && output_width == ow_ && output_height == oh_ &&
        hdr == hdr_ && depth_infinite == infinite_ && depth_inverted == inverted_)
      return true;
    // fsr049: which back end - D3D12 (BeginNative) or Vulkan (BeginNativeVulkan), decided once.
    void* dev = nullptr;
    void* list = nullptr;
    if (api_ == Api::kUnknown) {
      if (backend.BeginNative(&dev, &list)) {
        backend.EndNative();
        api_ = Api::kD3D12;
      } else if (backend.BeginNativeVulkan(&dev, &list)) {
        backend.EndNativeVulkan();  // review: only the API is asked here - close it on every path
        if (!fsrvk::Built()) return Fail("this exe was built without the FSR Vulkan headers");
        if (!fsrvk::Enabled()) return Fail("--gow2_fsr_vulkan=false");
        api_ = Api::kVulkan;
      } else {
        return Fail("the backend gives no native D3D12 or Vulkan access");
      }
    }
    if (!module_) {
      const wchar_t* dll = api_ == Api::kVulkan ? L"amd_fidelityfx_loader_vk.dll" : L"amd_fidelityfx_loader_dx12.dll";
      module_ = gn_shared_dlls::LoadVendorDll(dll);  // shared049: or <COGBox>\shared by full path
      if (!module_)
        return Fail(api_ == Api::kVulkan ? "amd_fidelityfx_loader_vk.dll not found next to the exe"
                                         : "amd_fidelityfx_loader_dx12.dll not found next to the exe");
      ffxLoadFunctions(&fn_, module_);
      if (!fn_.CreateContext || !fn_.DestroyContext || !fn_.Dispatch || !fn_.Query)
        return Fail("the FidelityFX loader has no ffx_api entry points");
    }
    // The backend description: D3D12 needs the device; Vulkan the device, physical device and vkGetDeviceProcAddr.
    ffxCreateBackendDX12Desc be12{};
    ffxApiHeader* backend_desc = nullptr;
    void* version_device = nullptr;  // ID3D12Device* or VkDeviceContext* (ffxQueryDescGetVersions)
    if (api_ == Api::kD3D12) {
      if (!backend.BeginNative(&dev, &list)) return Fail("the backend gives no native D3D12 access");
      backend.EndNative();  // creation needs only the device
      be12.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
      be12.device = static_cast<ID3D12Device*>(dev);
      backend_desc = &be12.header;
      version_device = dev;
    } else {
      if (!vk_backend_) vk_backend_ = fsrvk::NewBackend(backend);  // the device does not change
      if (!vk_backend_) return Fail("the Vulkan backend gives no native device");
      backend_desc = fsrvk::BackendHeader(vk_backend_);
      version_device = fsrvk::DeviceContext(vk_backend_);
    }
    if (context_) {
      // 0.2.0 (#6): FSR's rule - its context may only go once the GPU has finished the work that uses it (a new render
      // size, e.g. the resolution-scale threshold, re-creates it mid-game). Retire it and destroy it 8 frames later,
      // past every frame that can still be in flight.
      retired_.push_back({context_, 8});
      context_ = nullptr;
    }
    // fsr049: the versions on offer (logged once) and the --gow2_fsr_version override. The same override must go on
    // every create and null-context query (AMD's rule), so it is chosen once.
    if (!versions_logged_) {
      versions_logged_ = true;
      const FsrVersions v = QueryVersions(fn_.Query, version_device);
      const std::string want = REXCVAR_GET(gow2_fsr_version);
      override_id_ = PickVersion(v, want);
      for (size_t i = 0; i < v.ids.size(); ++i)
        if (v.ids[i] == override_id_) override_name_ = v.names[i];
      REXLOG_INFO("upscale: FSR ({}): versions on this GPU: {}; --gow2_fsr_version={} -> {}",
                  api_ == Api::kVulkan ? "Vulkan" : "D3D12", JoinVersions(v), want,
                  override_id_ ? "pinned to " + override_name_ : std::string("the SDK's choice"));
      if (!override_id_ && want != "auto" && !want.empty())
        REXLOG_WARN("upscale: FSR: version '{}' is not offered on this GPU / driver; the SDK's choice instead", want);
    }
    ffxCreateContextDescUpscaleVersion ver{};  // SDK 2.1+: required, "the version the app was built against"
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver.version = FFX_UPSCALER_VERSION;
    ver.header.pNext = backend_desc;
    ffxOverrideVersion ov{};
    ffxApiHeader* chain = &ver.header;
    if (override_id_) {
      ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
      ov.header.pNext = chain;
      ov.versionId = override_id_;
      chain = &ov.header;
    }
    ffxCreateContextDescUpscale c{};
    c.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    c.header.pNext = chain;
    // HDR linear colour; FSR's own exposure (we don't read the game's tone-mapper exposure).
    c.flags = (hdr ? (FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE) : 0u) |
              (depth_infinite ? FFX_UPSCALE_ENABLE_DEPTH_INFINITE : 0u) |
              (depth_inverted ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0u);  // notes/80 s.6
    c.maxRenderSize = {render_width, render_height};
    c.maxUpscaleSize = {output_width, output_height};
    c.fpMessage = &FfxMessage;
    ffxReturnCode_t r = fn_.CreateContext(&context_, &c.header, nullptr);
    if ((r != FFX_API_RETURN_OK || !context_) && override_id_) {  // a pinned version that will not start: the SDK's choice
      REXLOG_WARN("upscale: FSR: pinned version {} failed ({}); trying the SDK's choice", override_name_, uint32_t(r));
      override_id_ = 0;
      context_ = nullptr;
      c.header.pNext = &ver.header;
      r = fn_.CreateContext(&context_, &c.header, nullptr);
    }
    if (r != FFX_API_RETURN_OK || !context_) return Fail(std::format("context creation failed ({})", uint32_t(r)));
    rw_ = render_width, rh_ = render_height, ow_ = output_width, oh_ = output_height, hdr_ = hdr, infinite_ = depth_infinite;
    inverted_ = depth_inverted;
    // Which upscaler actually runs (FSR 4.1.x ML / 3.1.x): its provider version string.
    std::string version = "?";
    ffxQueryGetProviderVersion v{};
    v.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (fn_.Query(&context_, &v.header) == FFX_API_RETURN_OK && v.versionName) version = v.versionName;
    const char* family = version.rfind("4.", 0) == 0 ? "FSR 4 (ML)" : version.rfind("3.", 0) == 0 ? "FSR 3.1 (fallback)" : "FSR";
    status_ = std::format("FSR {} {} {}x{} -> {}x{}{}", version, output_width == render_width ? "native AA" : "upscale",
                          render_width, render_height, output_width, output_height, hdr ? " HDR" : "");
    REXLOG_INFO("upscale: {} - {} provider {:#x}, {}, {}", status_, family, v.versionId,
                api_ == Api::kVulkan ? "Vulkan" : "D3D12", override_id_ ? "pinned" : "SDK's choice");
    return true;
  }

  bool Evaluate(gpu::Backend& backend, const FrameInputs& in) override {
    for (size_t i = 0; i < retired_.size();) {   // 0.2.0 (#6)
      if (--retired_[i].frames_left <= 0) {
        fn_.DestroyContext(&retired_[i].context, nullptr);
        retired_.erase(retired_.begin() + ptrdiff_t(i));
      } else ++i;
    }
    if (!context_ || failed_) return false;
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    // FSR takes the jitter in render pixels, the same value its sample adds to the projection (the image shift).
    d.jitterOffset = {in.jitter.x, in.jitter.y};
    d.motionVectorScale = {1.0f, 1.0f};  // our motion is in render pixels already
    d.renderSize = {in.render_width, in.render_height};
    d.upscaleSize = {in.output_width, in.output_height};
    d.enableSharpening = in.sharpness > 0.0f;
    d.sharpness = in.sharpness;
    d.frameTimeDelta = in.frame_ms;
    d.preExposure = in.pre_exposure > 0.0f ? in.pre_exposure : 1.0f;
    d.reset = in.reset;
    d.cameraNear = in.camera_near;
    d.cameraFar = in.camera_far;
    d.cameraFovAngleVertical = in.vertical_fov;
    d.viewSpaceToMetersFactor = 0.01f;  // Unreal units are centimetres
    d.flags = 0;
    bool ok = false;
    if (api_ == Api::kD3D12) {
      void* dev = nullptr;
      void* list = nullptr;
      if (!backend.BeginNative(&dev, &list)) return false;
      auto* color = static_cast<ID3D12Resource*>(backend.NativeTexture(in.color, kStateShaderRead));
      auto* depth = static_cast<ID3D12Resource*>(backend.NativeTexture(in.depth, kStateShaderRead));
      auto* motion = static_cast<ID3D12Resource*>(backend.NativeTexture(in.motion, kStateShaderRead));
      auto* output = static_cast<ID3D12Resource*>(backend.NativeTexture(in.output, kStateUav));
      ok = color && depth && motion && output;
      if (ok) {
        d.commandList = list;
        d.color = ffxApiGetResourceDX12(color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.motionVectors = ffxApiGetResourceDX12(motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.output = ffxApiGetResourceDX12(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        ok = Dispatch(d);
      }
      backend.EndNative();
    } else {
      void* dev = nullptr;
      void* cb = nullptr;
      if (!backend.BeginNativeVulkan(&dev, &cb)) return false;
      FfxApiResource color{}, depth{}, motion{}, output{};
      ok = fsrvk::Resource(backend, in.color, false, &color) && fsrvk::Resource(backend, in.depth, false, &depth) &&
           fsrvk::Resource(backend, in.motion, false, &motion) && fsrvk::Resource(backend, in.output, true, &output);
      if (ok) {
        d.commandList = cb;
        d.color = color;
        d.depth = depth;
        d.motionVectors = motion;
        d.output = output;
        ok = Dispatch(d);
      }
      backend.EndNativeVulkan();  // the SDK changed our bound pipeline / sets; review: on the failure path too
    }
    if (ok && !first_logged_) {
      first_logged_ = true;
      REXLOG_INFO("upscale: FSR: first frame upscaled ({})", status_);
    }
    return ok;
  }

 private:
  enum class Api { kUnknown, kD3D12, kVulkan };

  bool Dispatch(ffxDispatchDescUpscale& d) {
    bool faulted = false;
    const ffxReturnCode_t r = GuardedDispatch(fn_.Dispatch, &context_, &d.header, &faulted);
    if (faulted) FsrFatal();
    if (r != FFX_API_RETURN_OK && !eval_error_logged_) {
      eval_error_logged_ = true;
      REXLOG_WARN("upscale: FSR dispatch failed ({})", uint32_t(r));
    }
    return r == FFX_API_RETURN_OK;
  }


  bool Fail(const std::string& why) {
    failed_ = true;
    status_ = "FSR off: " + why;
    REXLOG_WARN("upscale: {}", status_);
    return false;
  }

  Api api_ = Api::kUnknown;
  void* vk_backend_ = nullptr;  // fsr049: fsrvk::NewBackend (its VkDeviceContext outlives the contexts)
  HMODULE module_ = nullptr;
  ffxFunctions fn_{};
  struct Retired { ffxContext context; int frames_left; };
  std::vector<Retired> retired_;   // 0.2.0 (#6): old contexts the GPU may still be using
  ffxContext context_ = nullptr;
  bool failed_ = false, hdr_ = false, infinite_ = false, eval_error_logged_ = false;
  bool inverted_ = false;  // notes/80 s.6
  bool versions_logged_ = false, first_logged_ = false;
  uint64_t override_id_ = 0;  // fsr049: --gow2_fsr_version (0 = the SDK's choice)
  std::string override_name_;
  uint32_t rw_ = 0, rh_ = 0, ow_ = 0, oh_ = 0;
  std::string status_ = "FSR not started";
};

}  // namespace

std::unique_ptr<Upscaler> CreateFsrUpscaler() { return std::make_unique<FsrUpscaler>(); }

}  // namespace xenos::renderer::upscale

#endif  // GN_HAVE_FSR
