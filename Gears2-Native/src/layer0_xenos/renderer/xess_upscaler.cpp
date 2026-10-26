// Layer 0 (game-free): Intel XeSS Super Resolution behind upscale::Upscaler (upscaler.h, notes/64, notes/65 item 4),
// D3D12. Built only when the XeSS SDK is in third_party\xess-sdk (GN_HAVE_XESS): its headers at build time, libxess.dll
// next to the exe at run time (copied by the build; never committed - Intel's licence). The DLL is loaded when XeSS is
// first chosen (LoadLibrary / GetProcAddress, no import library), so the exe still starts on a PC without it and the
// choice falls back like any unavailable upscaler. Runs on any
// GPU with Shader Model 6.4 (Intel Arc uses its XMX units, other GPUs the DP4a path), so it is a third choice for
// players whose card has neither DLSS nor FSR 4. Our code (MIT); the SDK is Intel's.
//
// Calls (XeSS-SR developer guide, SDK 3.0): xessD3D12CreateContext once per device; xessD3D12Init for an output size
// and a quality setting (AA = native resolution, 1.0; the others render smaller), re-initialised when they change;
// xessD3D12Execute once per frame on our command list. Conventions match ours (upscaler.h): low-res motion vectors in
// pixels, current -> previous ("motion in pixels from the current frame to the previous frame", no jitter in them);
// jitter in [-0.5, 0.5] as the shift applied to the image (guide: ProjectionMatrix[2][0] += Jx * 2 / W, [2][1] -= Jy * 2
// / H, i.e. +x right, +y down); depth 0 near (not inverted). Linear HDR colour with XeSS's auto-exposure, or LDR colour
// (XESS_INIT_FLAG_LDR_INPUT_COLOR) with exposure 1.
#if defined(GN_HAVE_XESS)

#include "xenos/renderer/upscaler.h"

#include <windows.h>
#include <d3d12.h>

#include <xess/xess.h>
#include <xess/xess_d3d12.h>

#include <rex/logging/macros.h>

#include <format>
#include <string>
#include <type_traits>

namespace xenos::renderer::upscale {
namespace {

// The XeSS entry points we use, from libxess.dll.
struct XessApi {
  decltype(&xessGetVersion) GetVersion = nullptr;
  decltype(&xessD3D12CreateContext) CreateContext = nullptr;
  decltype(&xessD3D12Init) Init = nullptr;
  decltype(&xessD3D12Execute) Execute = nullptr;
  decltype(&xessDestroyContext) DestroyContext = nullptr;
  decltype(&xessIsOptimalDriver) IsOptimalDriver = nullptr;
  decltype(&xessForceLegacyScaleFactors) ForceLegacyScaleFactors = nullptr;
  decltype(&xessGetInputResolution) GetInputResolution = nullptr;
  bool ok = false;
};

// Loaded once per process; ok = false when the DLL or an entry point is missing.
const XessApi& Api() {
  static const XessApi api = [] {
    XessApi a;
    HMODULE dll = LoadLibraryW(L"libxess.dll");
    if (!dll) return a;
    auto get = [dll](auto& fn, const char* name) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
      return fn != nullptr;
    };
    a.ok = get(a.GetVersion, "xessGetVersion") && get(a.CreateContext, "xessD3D12CreateContext") &&
           get(a.Init, "xessD3D12Init") && get(a.Execute, "xessD3D12Execute") &&
           get(a.DestroyContext, "xessDestroyContext") && get(a.IsOptimalDriver, "xessIsOptimalDriver") &&
           get(a.ForceLegacyScaleFactors, "xessForceLegacyScaleFactors") &&
           get(a.GetInputResolution, "xessGetInputResolution");
    return a;
  }();
  return api;
}

constexpr uint32_t kStateShaderRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr uint32_t kStateUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

// The XeSS quality setting for a render / output ratio. We ask XeSS for its pre-1.3 ratios
// (xessForceLegacyScaleFactors: AA 1.0, Ultra Quality 1.3, Quality 1.5, Balanced 1.7, Performance 2.0, Ultra
// Performance 3.0), which are the ones our Quality modes use for every upscaler (upscaler.h).
xess_quality_settings_t QualityFor(uint32_t rw, uint32_t ow) {
  if (ow <= rw) return XESS_QUALITY_SETTING_AA;
  const float r = float(ow) / float(rw);
  if (r >= 2.5f) return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
  if (r >= 1.85f) return XESS_QUALITY_SETTING_PERFORMANCE;
  if (r >= 1.6f) return XESS_QUALITY_SETTING_BALANCED;
  if (r >= 1.4f) return XESS_QUALITY_SETTING_QUALITY;
  return XESS_QUALITY_SETTING_ULTRA_QUALITY;
}

const char* QualityName(xess_quality_settings_t q) {
  switch (q) {
    case XESS_QUALITY_SETTING_AA: return "native AA";
    case XESS_QUALITY_SETTING_ULTRA_QUALITY: return "Ultra Quality";
    case XESS_QUALITY_SETTING_QUALITY: return "Quality";
    case XESS_QUALITY_SETTING_BALANCED: return "Balanced";
    case XESS_QUALITY_SETTING_PERFORMANCE: return "Performance";
    case XESS_QUALITY_SETTING_ULTRA_PERFORMANCE: return "Ultra Performance";
    default: return "?";
  }
}

class XessUpscaler final : public Upscaler {
 public:
  ~XessUpscaler() override {
    if (context_) Api().DestroyContext(context_);
  }
  Kind kind() const override { return Kind::kXess; }
  std::string Status() const override { return status_; }

  bool Configure(gpu::Backend& backend, uint32_t render_width, uint32_t render_height, uint32_t output_width,
                 uint32_t output_height, bool hdr, bool depth_infinite) override {
    (void)depth_infinite;  // XeSS uses depth only to dilate motion vectors (smaller = closer)
    if (failed_) return false;
    if (initialised_ && render_width == rw_ && render_height == rh_ && output_width == ow_ && output_height == oh_ &&
        hdr == hdr_)
      return true;
    void* dev = nullptr;
    void* list = nullptr;
    if (!backend.BeginNative(&dev, &list)) return Fail("the backend gives no native D3D12 access");
    backend.EndNative();
    auto* device = static_cast<ID3D12Device*>(dev);
    const XessApi& x = Api();
    if (!x.ok) return Fail("XeSS is off: libxess.dll is not next to the exe (or lacks an entry point we use)");
    if (!context_) {
      xess_version_t v{};
      x.GetVersion(&v);
      const xess_result_t r = x.CreateContext(device, &context_);
      if (r != XESS_RESULT_SUCCESS || !context_) {
        context_ = nullptr;
        return Fail(std::format("XeSS {}.{}.{} is not available on this GPU / driver (result {})", v.major, v.minor,
                                v.patch, int(r)));
      }
      version_ = std::format("{}.{}.{}", v.major, v.minor, v.patch);
      if (x.IsOptimalDriver(context_) != XESS_RESULT_SUCCESS)
        REXLOG_WARN("upscale: XeSS {} reports a non-optimal driver (it still runs)", version_);
      x.ForceLegacyScaleFactors(context_, true);  // must come before xessD3D12Init
    }
    const xess_quality_settings_t q = QualityFor(render_width, output_width);
    xess_d3d12_init_params_t p{};
    p.outputResolution = {output_width, output_height};
    p.qualitySetting = q;
    p.initFlags = hdr ? XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE : XESS_INIT_FLAG_LDR_INPUT_COLOR;  // low-res MVs + depth
    // Blocking: builds XeSS's pipelines / kernels for these settings (first time only noticeably slow).
    const xess_result_t r = x.Init(context_, &p);
    if (r != XESS_RESULT_SUCCESS) return Fail(std::format("XeSS {} initialisation failed (result {})", version_, int(r)));
    // The input size XeSS expects for this quality (AA: the output size). Our render size must match it.
    xess_2d_t out{output_width, output_height}, in{};
    if (x.GetInputResolution(context_, &out, q, &in) == XESS_RESULT_SUCCESS &&
        (in.x != render_width || in.y != render_height))
      REXLOG_WARN("upscale: XeSS {} expects {}x{} input for {}, we render {}x{}", version_, in.x, in.y, QualityName(q),
                  render_width, render_height);
    initialised_ = true;
    rw_ = render_width, rh_ = render_height, ow_ = output_width, oh_ = output_height, hdr_ = hdr;
    status_ = std::format("XeSS {} {} {}x{} -> {}x{}{}", version_, QualityName(q), rw_, rh_, ow_, oh_, hdr ? " HDR" : "");
    REXLOG_INFO("upscale: {}", status_);
    return true;
  }

  bool Evaluate(gpu::Backend& backend, const FrameInputs& in) override {
    if (!initialised_ || failed_) return false;
    void* dev = nullptr;
    void* list = nullptr;
    if (!backend.BeginNative(&dev, &list)) return false;
    xess_d3d12_execute_params_t e{};
    e.pColorTexture = static_cast<ID3D12Resource*>(backend.NativeTexture(in.color, kStateShaderRead));
    e.pVelocityTexture = static_cast<ID3D12Resource*>(backend.NativeTexture(in.motion, kStateShaderRead));
    e.pDepthTexture = static_cast<ID3D12Resource*>(backend.NativeTexture(in.depth, kStateShaderRead));
    e.pOutputTexture = static_cast<ID3D12Resource*>(backend.NativeTexture(in.output, kStateUav));
    e.jitterOffsetX = in.jitter.x;
    e.jitterOffsetY = in.jitter.y;
    e.exposureScale = 1.0f;
    e.resetHistory = in.reset ? 1u : 0u;
    e.inputWidth = in.render_width;
    e.inputHeight = in.render_height;
    bool ok = e.pColorTexture && e.pVelocityTexture && e.pDepthTexture && e.pOutputTexture;
    if (ok) {
      const xess_result_t r = Api().Execute(context_, static_cast<ID3D12GraphicsCommandList*>(list), &e);
      ok = r == XESS_RESULT_SUCCESS;
      if (!ok && !error_logged_) {
        error_logged_ = true;
        REXLOG_WARN("upscale: XeSS execute failed (result {}) - logged once", int(r));
      }
    }
    backend.EndNative();
    return ok;
  }

 private:
  bool Fail(const std::string& why) {
    failed_ = true;
    status_ = why;
    REXLOG_WARN("upscale: {}", why);
    return false;
  }

  xess_context_handle_t context_ = nullptr;
  bool initialised_ = false, failed_ = false, hdr_ = false, error_logged_ = false;
  uint32_t rw_ = 0, rh_ = 0, ow_ = 0, oh_ = 0;
  std::string version_ = "?", status_ = "XeSS not started";
};

}  // namespace

std::unique_ptr<Upscaler> CreateXessUpscaler() { return std::make_unique<XessUpscaler>(); }

}  // namespace xenos::renderer::upscale

#endif  // GN_HAVE_XESS
