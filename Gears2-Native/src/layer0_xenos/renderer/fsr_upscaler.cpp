// Layer 0 (game-free): AMD FSR (the FidelityFX SDK's upscaler, ffx_api) behind upscale::Upscaler (upscaler.h,
// notes/64), D3D12. Built only when the SDK is in third_party\fidelityfx-sdk (GN_HAVE_FSR): its ffx_api headers at
// build time; at run time amd_fidelityfx_loader_dx12.dll (+ amd_fidelityfx_upscaler_dx12.dll) next to the exe, loaded
// with LoadLibrary - no import library. The loader picks the newest upscaler the GPU supports (FSR 4 on RDNA 4, the
// FSR 3.1 path elsewhere). Never committed (AMD's licence, docs/license.md). Our code (MIT); the SDK is AMD's (MIT).
#if defined(GN_HAVE_FSR)

#include "xenos/renderer/upscaler.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>

#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <ffx_api_types.h>
#include <ffx_upscale.h>
#include <dx12/ffx_api_dx12.h>

#include <rex/logging/macros.h>

#include <format>
#include <string>

namespace xenos::renderer::upscale {
namespace {

constexpr uint32_t kStateShaderRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr uint32_t kStateUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

void FfxMessage(uint32_t type, const wchar_t* message) {
  std::string m;
  for (const wchar_t* p = message; p && *p; ++p) m += *p < 128 ? char(*p) : '?';
  if (type == FFX_API_MESSAGE_TYPE_ERROR) REXLOG_WARN("upscale: FSR: {}", m);
  else REXLOG_INFO("upscale: FSR: {}", m);
}

class FsrUpscaler final : public Upscaler {
 public:
  ~FsrUpscaler() override {
    if (context_ && fn_.DestroyContext) fn_.DestroyContext(&context_, nullptr);
    context_ = nullptr;
    if (module_) FreeLibrary(module_);
  }
  Kind kind() const override { return Kind::kFsr; }
  std::string Status() const override { return status_; }

  bool Configure(gpu::Backend& backend, uint32_t render_width, uint32_t render_height, uint32_t output_width,
                 uint32_t output_height, bool hdr, bool depth_infinite) override {
    if (failed_) return false;
    if (context_ && render_width == rw_ && render_height == rh_ && output_width == ow_ && output_height == oh_ &&
        hdr == hdr_ && depth_infinite == infinite_)
      return true;
    if (!module_) {
      module_ = LoadLibraryW(L"amd_fidelityfx_loader_dx12.dll");
      if (!module_) return Fail("amd_fidelityfx_loader_dx12.dll not found next to the exe");
      ffxLoadFunctions(&fn_, module_);
      if (!fn_.CreateContext || !fn_.DestroyContext || !fn_.Dispatch || !fn_.Query)
        return Fail("the FidelityFX loader has no ffx_api entry points");
    }
    void* dev = nullptr;
    void* list = nullptr;
    if (!backend.BeginNative(&dev, &list)) return Fail("the backend gives no native D3D12 access");
    backend.EndNative();  // creation needs only the device
    if (context_) {
      fn_.DestroyContext(&context_, nullptr);
      context_ = nullptr;
    }
    ffxCreateBackendDX12Desc be{};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    be.device = static_cast<ID3D12Device*>(dev);
    ffxCreateContextDescUpscale c{};
    c.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    c.header.pNext = &be.header;
    // HDR linear colour; FSR's own exposure (we don't read the game's tone-mapper exposure).
    c.flags = (hdr ? (FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE) : 0u) |
              (depth_infinite ? FFX_UPSCALE_ENABLE_DEPTH_INFINITE : 0u);
    c.maxRenderSize = {render_width, render_height};
    c.maxUpscaleSize = {output_width, output_height};
    c.fpMessage = &FfxMessage;
    const ffxReturnCode_t r = fn_.CreateContext(&context_, &c.header, nullptr);
    if (r != FFX_API_RETURN_OK || !context_) return Fail(std::format("context creation failed ({})", uint32_t(r)));
    rw_ = render_width, rh_ = render_height, ow_ = output_width, oh_ = output_height, hdr_ = hdr, infinite_ = depth_infinite;
    // Which upscaler the loader chose (FSR 4 / 3.1.x): its provider version string.
    std::string version = "?";
    ffxQueryGetProviderVersion v{};
    v.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (fn_.Query(&context_, &v.header) == FFX_API_RETURN_OK && v.versionName) version = v.versionName;
    status_ = std::format("FSR {} {} {}x{} -> {}x{}{}", version, output_width == render_width ? "native AA" : "upscale",
                          render_width, render_height, output_width, output_height, hdr ? " HDR" : "");
    REXLOG_INFO("upscale: {}", status_);
    return true;
  }

  bool Evaluate(gpu::Backend& backend, const FrameInputs& in) override {
    if (!context_ || failed_) return false;
    void* dev = nullptr;
    void* list = nullptr;
    if (!backend.BeginNative(&dev, &list)) return false;
    auto* color = static_cast<ID3D12Resource*>(backend.NativeTexture(in.color, kStateShaderRead));
    auto* depth = static_cast<ID3D12Resource*>(backend.NativeTexture(in.depth, kStateShaderRead));
    auto* motion = static_cast<ID3D12Resource*>(backend.NativeTexture(in.motion, kStateShaderRead));
    auto* output = static_cast<ID3D12Resource*>(backend.NativeTexture(in.output, kStateUav));
    bool ok = color && depth && motion && output;
    if (ok) {
      ffxDispatchDescUpscale d{};
      d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
      d.commandList = list;
      d.color = ffxApiGetResourceDX12(color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
      d.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
      d.motionVectors = ffxApiGetResourceDX12(motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
      d.output = ffxApiGetResourceDX12(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
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
      const ffxReturnCode_t r = fn_.Dispatch(&context_, &d.header);
      ok = r == FFX_API_RETURN_OK;
      if (!ok && !eval_error_logged_) {
        eval_error_logged_ = true;
        REXLOG_WARN("upscale: FSR dispatch failed ({})", uint32_t(r));
      }
    }
    backend.EndNative();
    return ok;
  }

 private:
  bool Fail(const std::string& why) {
    failed_ = true;
    status_ = "FSR off: " + why;
    REXLOG_WARN("upscale: {}", status_);
    return false;
  }

  HMODULE module_ = nullptr;
  ffxFunctions fn_{};
  ffxContext context_ = nullptr;
  bool failed_ = false, hdr_ = false, infinite_ = false, eval_error_logged_ = false;
  uint32_t rw_ = 0, rh_ = 0, ow_ = 0, oh_ = 0;
  std::string status_ = "FSR not started";
};

}  // namespace

std::unique_ptr<Upscaler> CreateFsrUpscaler() { return std::make_unique<FsrUpscaler>(); }

}  // namespace xenos::renderer::upscale

#endif  // GN_HAVE_FSR
