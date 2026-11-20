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
#include <dxgi1_4.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_d3d.h>

#include "shared_dlls.h"  // shared049: nvngx_dlss.dll from the exe folder or <COGBox>\shared

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include <windows.h>

#include <format>
#include <string>

REXCVAR_DEFINE_STRING(gow2_dlss_preset, "recommended", "Gears2",
                      "DLSS model preset: auto / recommended (K for DLAA / Quality / Balanced; Performance M and Ultra "
                      "Performance L on RTX 40 / 50 cards, K on RTX 20 / 30 cards, where the newest models are slow - "
                      "job 2137b), nvidia (NVIDIA's table on every card: K / M / L), driver (whatever the driver "
                      "picks, may change with updates), or one letter J, K, L or M for every mode (0.4.9: the "
                      "launcher's \"DLSS model\"); E and F (the older, lighter models, deprecated by NVIDIA) for tests");

namespace xenos::renderer::upscale {
namespace {

// The preset hints set before the feature is created (SDK 310 header: K = transformer, best quality, default for DLAA /
// Quality / Balanced; M default for Performance; L for Ultra Performance; A-F removed or deprecated).
// The graphics card's name, from the device's adapter ("" when it can't be read).
std::string DeviceCardName(ID3D12Device* device) {
  std::string name;
  if (!device) return name;
  const LUID want = device->GetAdapterLuid();
  // dxgi.dll is already in the process (the device came from it); looked up by name so nothing new is linked
  HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
  using MakeFactory = HRESULT(WINAPI*)(REFIID, void**);
  auto make = dxgi ? reinterpret_cast<MakeFactory>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
  IDXGIFactory1* factory = nullptr;
  if (!make || FAILED(make(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) return name;
  IDXGIAdapter1* a = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &a) == S_OK; ++i) {
    DXGI_ADAPTER_DESC1 d{};
    if (a->GetDesc1(&d) == S_OK && d.AdapterLuid.LowPart == want.LowPart && d.AdapterLuid.HighPart == want.HighPart)
      for (const wchar_t* p = d.Description; *p; ++p) name += *p < 128 ? char(*p) : '?';
    a->Release();
    a = nullptr;
    if (!name.empty()) break;
  }
  factory->Release();
  return name;
}
// GeForce RTX 20 / 30 series ("RTX 2070 SUPER", "RTX 3080 Ti", laptop names too): NVIDIA's newest models (L, M - DLSS
// 4.5) are made for RTX 40 / 50 and run slowly here. Job 2137b, RTX 3080 Ti, 720p -> 1440p with M: the scene got 1.1 ms
// cheaper and the upscale pass 1.8 ms dearer.
bool OlderRtxCard(const std::string& name) {
  if (name.find(" Ada") != std::string::npos) return false;  // "RTX 3000 Ada Generation" is a 40-series part
  for (size_t at = name.find("RTX "); at != std::string::npos; at = name.find("RTX ", at + 1)) {
    const size_t d = at + 4;
    if (d + 3 < name.size() && (name[d] == '2' || name[d] == '3') && name[d + 1] == '0' && name[d + 2] >= '0' &&
        name[d + 2] <= '9' && name[d + 3] >= '0' && name[d + 3] <= '9')
      return true;
  }
  return false;
}

void SetDlssPresets(NVSDK_NGX_Parameter* params, const std::string& choice, bool older_card) {
  if (choice == "driver") return;   // "auto" / "recommended" / anything else: the table below
  const bool light = older_card && choice != "nvidia";
  auto letter = [](char c) -> int {
    switch (c) {
      case 'E': case 'e': return NVSDK_NGX_DLSS_Hint_Render_Preset_E;
      case 'F': case 'f': return NVSDK_NGX_DLSS_Hint_Render_Preset_F;
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
  NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                            pick(light ? NVSDK_NGX_DLSS_Hint_Render_Preset_K : NVSDK_NGX_DLSS_Hint_Render_Preset_M));
  NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                            pick(light ? NVSDK_NGX_DLSS_Hint_Render_Preset_K : NVSDK_NGX_DLSS_Hint_Render_Preset_L));
}

// 0.4.9: which nvngx_dlss.dll NGX loaded and its version (the launcher's "Custom DLSS file" puts the player's own
// official copy beside the exe) - read from the loaded module's VS_FIXEDFILEINFO, no version.lib needed.
std::string LoadedDlssDll() {
  HMODULE m = GetModuleHandleW(L"nvngx_dlss.dll");
  if (!m) return "nvngx_dlss.dll not loaded";
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(m, path, MAX_PATH);
  std::string where;
  for (const wchar_t* c = path; *c; ++c) where += *c < 128 ? char(*c) : '?';
  std::string ver = "?";
  if (HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16)))  // RT_VERSION
    if (HGLOBAL g = LoadResource(m, r)) {
      const auto* b = static_cast<const uint8_t*>(LockResource(g));
      const auto* f = reinterpret_cast<const VS_FIXEDFILEINFO*>(b + 40);  // after the VS_VERSION_INFO header + key
      if (b && SizeofResource(m, r) >= 40 + sizeof(VS_FIXEDFILEINFO) && f->dwSignature == 0xFEEF04BD)
        ver = std::format("{}.{}.{}.{}", HIWORD(f->dwFileVersionMS), LOWORD(f->dwFileVersionMS),
                          HIWORD(f->dwFileVersionLS), LOWORD(f->dwFileVersionLS));
    }
  return std::format("{} version {}", where, ver);
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
  // GitHub #323: the output always fills the creation target (InRenderSubrectDimensions maps the render subrect onto
  // all of it; there is no output size per evaluate), so a narrower scene is padded to the creation size by the renderer.
  ResizeMode Resize() const override { return ResizeMode::kPadded; }

  bool Configure(gpu::Backend& backend, uint32_t render_width, uint32_t render_height, uint32_t output_width,
                 uint32_t output_height, bool hdr, bool depth_infinite, bool depth_inverted) override {
    (void)depth_infinite;  // DLSS only needs near 0 / far 1 (guide 3.8) - or the reverse, with the inverted flag
    if (failed_) return false;
    if (feature_ && render_width == rw_ && render_height == rh_ && output_width == ow_ && output_height == oh_ &&
        hdr == hdr_ && depth_inverted == inverted_)
      return true;
    inverted_ = depth_inverted;
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
      // 0.4.9 (shared049): where NGX looks for nvngx_dlss.dll - the exe's folder first (developer builds; the player's
      // own file from the launcher's "Custom DLSS file"), then the release's <COGBox>\shared\ (gn_shared_dlls). NGX
      // still checks NVIDIA's signature on whichever file it loads; "upscale: DLSS uses <path>" logs which one it was.
      static const std::wstring exe_dir = gn_shared_dlls::ExeDir();
      static const std::wstring shared_dir = gn_shared_dlls::SharedDir();
      static const wchar_t* ngx_paths[2] = {};
      unsigned ngx_count = 0;
      if (!exe_dir.empty()) ngx_paths[ngx_count++] = exe_dir.c_str();
      if (!shared_dir.empty()) ngx_paths[ngx_count++] = shared_dir.c_str();
      NVSDK_NGX_FeatureCommonInfo common{};
      common.PathListInfo.Path = ngx_paths;
      common.PathListInfo.Length = ngx_count;
      REXLOG_INFO("upscale: NGX looks for nvngx_dlss.dll in {}{}", gn_shared_dlls::Narrow(exe_dir),
                  shared_dir.empty() ? std::string() : " then " + gn_shared_dlls::Narrow(shared_dir));
      const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
                                                                     L".", device, ngx_count ? &common : nullptr);
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
                             (hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0) |
                             (inverted_ ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0);  // notes/80 s.6
    const std::string preset = REXCVAR_GET(gow2_dlss_preset);
    const std::string card = DeviceCardName(device);
    const bool older = OlderRtxCard(card);
    SetDlssPresets(params_, preset, older);
    if (older && preset != "nvidia" && preset != "driver" && preset.size() != 1)
      REXLOG_INFO("upscale: DLSS on '{}' (RTX 20 / 30): model K for Performance and Ultra Performance too - the newest "
                  "models M / L are slow on these cards (--gow2_dlss_preset=nvidia for NVIDIA's table)", card);
    const NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(list, 1, 1, &feature_, params_, &c);
    if (NVSDK_NGX_FAILED(r) || !feature_) return Fail(std::format("feature creation failed ({:08X})", uint32_t(r)));
    rw_ = rw, rh_ = rh, ow_ = ow, oh_ = oh, hdr_ = hdr;
    status_ = std::format("DLSS {} {}x{} -> {}x{}{} preset {}", q == NVSDK_NGX_PerfQuality_Value_DLAA ? "DLAA" : "SR", rw,
                          rh, ow, oh, hdr ? " HDR" : "", preset);
    REXLOG_INFO("upscale: {}", status_);
    if (!dll_logged_) {
      dll_logged_ = true;
      REXLOG_INFO("upscale: DLSS uses {}", LoadedDlssDll());
    }
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
  bool dll_logged_ = false;
  bool inverted_ = false;  // notes/80 s.6: the depth input is reversed-Z
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
