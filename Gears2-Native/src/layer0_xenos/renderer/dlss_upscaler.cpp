// Layer 0 (game-free): NVIDIA DLSS Super Resolution / DLAA behind upscale::Upscaler (upscaler.h, notes/64), D3D12.
// Built only when the DLSS SDK is in third_party\dlss-sdk (GN_HAVE_DLSS): its headers and nvsdk_ngx_d.lib at build
// time, nvngx_dlss.dll next to the exe at run time (copied by the build from the SDK; never committed - NVIDIA RTX SDKs
// licence). Our code (MIT); the SDK is NVIDIA's.
//
// Calls (DLSS programming guide, SDK v310): NVSDK_NGX_D3D12_Init_with_ProjectID once per device; capability parameters
// say whether DLSS SR is available (an RTX GPU and a new enough driver); the feature is created on a command list for
// fixed render / output sizes and a quality mode (DLAA when they are equal), and evaluated once per frame. Inputs in
// NON_PIXEL_SHADER_RESOURCE, the output in UNORDERED_ACCESS - our backend moves them (Backend::NativeTexture).
#if defined(GN_HAVE_DLSS)

#include "xenos/renderer/upscaler.h"

#include <d3d12.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_d3d.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include <format>
#include <string>

REXCVAR_DEFINE_STRING(gow2_dlss_preset, "recommended", "Gears2",
                      "DLSS model preset: recommended (NVIDIA's current table, pinned: K for DLAA / Quality / Balanced, M "
                      "Performance, L Ultra Performance), driver (whatever the driver picks, may change with updates), or "
                      "one letter J, K, L or M for every mode");

namespace xenos::renderer::upscale {
namespace {

// The preset hints set before the feature is created (SDK 310 header: K = transformer, best quality, default for DLAA /
// Quality / Balanced; M default for Performance; L for Ultra Performance; A-F removed or deprecated).
void SetDlssPresets(NVSDK_NGX_Parameter* params, const std::string& choice) {
  if (choice == "driver") return;
  auto letter = [](char c) -> int {
    switch (c) {
      case 'J': case 'j': return NVSDK_NGX_DLSS_Hint_Render_Preset_J;
      case 'K': case 'k': return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
      case 'L': case 'l': return NVSDK_NGX_DLSS_Hint_Render_Preset_L;
      case 'M': case 'm': return NVSDK_NGX_DLSS_Hint_Render_Preset_M;
      default: return -1;
    }
  };
  const int one = choice.size() == 1 ? letter(choice[0]) : -1;
  const auto pick = [&](int recommended) { return unsigned(one >= 0 ? one : recommended); };
  NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, pick(NVSDK_NGX_DLSS_Hint_Render_Preset_K));
  NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, pick(NVSDK_NGX_DLSS_Hint_Render_Preset_K));
  NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, pick(NVSDK_NGX_DLSS_Hint_Render_Preset_K));
  NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, pick(NVSDK_NGX_DLSS_Hint_Render_Preset_M));
  NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, pick(NVSDK_NGX_DLSS_Hint_Render_Preset_L));
}

// Our project's own identifier for NGX (a GUID we made; NVIDIA's production approval uses an application ID instead).
constexpr const char* kProjectId = "6f3c2a9e-1b47-4d2a-9c58-0e7d4b3a5f21";
constexpr uint32_t kStateShaderRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr uint32_t kStateUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

class DlssUpscaler final : public Upscaler {
 public:
  ~DlssUpscaler() override { Release(); }
  Kind kind() const override { return Kind::kDlss; }
  std::string Status() const override { return status_; }

  bool Configure(gpu::Backend& backend, uint32_t render_width, uint32_t render_height, uint32_t output_width,
                 uint32_t output_height, bool hdr, bool depth_infinite) override {
    (void)depth_infinite;  // DLSS only needs near 0 / far 1 (guide 3.8)
    if (failed_) return false;
    if (feature_ && render_width == rw_ && render_height == rh_ && output_width == ow_ && output_height == oh_ &&
        hdr == hdr_)
      return true;
    void* dev = nullptr;
    void* list = nullptr;
    if (!backend.BeginNative(&dev, &list)) return Fail("the backend gives no native D3D12 access");
    const bool ok = Create(static_cast<ID3D12Device*>(dev), static_cast<ID3D12GraphicsCommandList*>(list),
                           render_width, render_height, output_width, output_height, hdr);
    backend.EndNative();
    return ok;
  }

  bool Evaluate(gpu::Backend& backend, const FrameInputs& in) override {
    if (!feature_ || failed_) return false;
    void* dev = nullptr;
    void* list = nullptr;
    if (!backend.BeginNative(&dev, &list)) return false;
    NVSDK_NGX_D3D12_DLSS_Eval_Params e{};
    e.Feature.pInColor = static_cast<ID3D12Resource*>(backend.NativeTexture(in.color, kStateShaderRead));
    e.Feature.pInOutput = static_cast<ID3D12Resource*>(backend.NativeTexture(in.output, kStateUav));
    e.pInDepth = static_cast<ID3D12Resource*>(backend.NativeTexture(in.depth, kStateShaderRead));
    e.pInMotionVectors = static_cast<ID3D12Resource*>(backend.NativeTexture(in.motion, kStateShaderRead));
    // Guide 3.7.3: the jitter applied to the projection, in render pixels, in the motion vectors' system (origin top
    // left, +y down) - our convention (upscaler.h). Motion: guide 3.6, previous - current position in render pixels.
    e.InJitterOffsetX = in.jitter.x;
    e.InJitterOffsetY = in.jitter.y;
    e.InRenderSubrectDimensions = {in.render_width, in.render_height};
    e.InReset = in.reset ? 1 : 0;
    e.InMVScaleX = 1.0f;  // already render-resolution pixels, previous - current
    e.InMVScaleY = 1.0f;
    e.InPreExposure = in.pre_exposure;
    e.InExposureScale = 1.0f;
    e.InFrameTimeDeltaInMsec = in.frame_ms;
    bool ok = e.Feature.pInColor && e.Feature.pInOutput && e.pInDepth && e.pInMotionVectors;
    if (ok) {
      const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(static_cast<ID3D12GraphicsCommandList*>(list), feature_,
                                                             params_, &e);
      ok = NVSDK_NGX_SUCCEED(r);
      if (!ok && !eval_error_logged_) {
        eval_error_logged_ = true;
        REXLOG_WARN("upscale: DLSS evaluate failed ({:08X})", uint32_t(r));
      }
    }
    backend.EndNative();
    if (ok) ++frames_;
    return ok;
  }

 private:
  bool Fail(const std::string& why) {
    failed_ = true;
    status_ = "DLSS off: " + why;
    REXLOG_WARN("upscale: {}", status_);
    return false;
  }

  bool Create(ID3D12Device* device, ID3D12GraphicsCommandList* list, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh,
              bool hdr) {
    if (!device || !list) return Fail("no device / command list");
    if (!initialised_) {
      const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
                                                                     L".", device);
      if (NVSDK_NGX_FAILED(r)) return Fail(std::format("NGX init failed ({:08X})", uint32_t(r)));
      initialised_ = true;
      device_ = device;
      NVSDK_NGX_Parameter* caps = nullptr;
      if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_GetCapabilityParameters(&caps)) || !caps)
        return Fail("no NGX capability parameters");
      int available = 0, needs_driver = 0;
      NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
      NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
      NVSDK_NGX_D3D12_DestroyParameters(caps);
      if (needs_driver) return Fail("the NVIDIA driver is too old for this DLSS version");
      if (!available) return Fail("DLSS Super Resolution is not available on this GPU");
      if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&params_)) || !params_)
        return Fail("could not allocate NGX parameters");
    }
    if (feature_) {
      NVSDK_NGX_D3D12_ReleaseFeature(feature_);
      feature_ = nullptr;
    }
    NVSDK_NGX_PerfQuality_Value q = NVSDK_NGX_PerfQuality_Value_DLAA;
    if (ow > rw) {
      const float ratio = float(ow) / float(rw);
      q = ratio >= 2.9f ? NVSDK_NGX_PerfQuality_Value_UltraPerformance  // patch 0143: 3x (ultra mode)
          : ratio >= 1.9f ? NVSDK_NGX_PerfQuality_Value_MaxPerf
          : ratio >= 1.6f ? NVSDK_NGX_PerfQuality_Value_Balanced
                          : NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }
    NVSDK_NGX_DLSS_Create_Params c{};
    c.Feature.InWidth = rw;
    c.Feature.InHeight = rh;
    c.Feature.InTargetWidth = ow;
    c.Feature.InTargetHeight = oh;
    c.Feature.InPerfQualityValue = q;
    // Render-resolution motion vectors without jitter (guide 3.6.2); linear HDR colour when the scene is HDR, with
    // DLSS's own exposure (guide 3.9: HDR needs the tone mapper's exposure, which we don't read from the game).
    c.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                             (hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0);
    const std::string preset = REXCVAR_GET(gow2_dlss_preset);
    SetDlssPresets(params_, preset);
    const NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(list, 1, 1, &feature_, params_, &c);
    if (NVSDK_NGX_FAILED(r) || !feature_) return Fail(std::format("feature creation failed ({:08X})", uint32_t(r)));
    rw_ = rw, rh_ = rh, ow_ = ow, oh_ = oh, hdr_ = hdr;
    status_ = std::format("DLSS {} {}x{} -> {}x{}{} preset {}", q == NVSDK_NGX_PerfQuality_Value_DLAA ? "DLAA" : "SR", rw,
                          rh, ow, oh, hdr ? " HDR" : "", preset);
    REXLOG_INFO("upscale: {}", status_);
    return true;
  }

  void Release() {
    if (feature_) NVSDK_NGX_D3D12_ReleaseFeature(feature_);
    feature_ = nullptr;
    if (params_) NVSDK_NGX_D3D12_DestroyParameters(params_);
    params_ = nullptr;
    if (initialised_ && device_) NVSDK_NGX_D3D12_Shutdown1(device_);
    initialised_ = false;
  }

  bool initialised_ = false, failed_ = false, hdr_ = false, eval_error_logged_ = false;
  ID3D12Device* device_ = nullptr;
  NVSDK_NGX_Parameter* params_ = nullptr;
  NVSDK_NGX_Handle* feature_ = nullptr;
  uint32_t rw_ = 0, rh_ = 0, ow_ = 0, oh_ = 0;
  uint64_t frames_ = 0;
  std::string status_ = "DLSS not started";
};

}  // namespace

std::unique_ptr<Upscaler> CreateDlssUpscaler() { return std::make_unique<DlssUpscaler>(); }

}  // namespace xenos::renderer::upscale

#endif  // GN_HAVE_DLSS
