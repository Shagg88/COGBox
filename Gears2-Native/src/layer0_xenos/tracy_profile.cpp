#include <xenos/tracy_profile.h>

#include <rex/cvar.h>
#include <rex/frame_hooks.h>
#include <rex/logging/macros.h>
#include <chrono>
#include <mutex>
#include <optional>

#if defined(GN_HAVE_TRACY)
#include <common/TracyVersion.hpp>
#endif

REXCVAR_DEFINE_BOOL(gow2_tracy, true, "Gears2",
                    "Enable on-demand Tracy renderer profiling; connect a matching Tracy viewer to localhost:8086. "
                    "Restart required; false disables COGBox zones and startup");
REXCVAR_DEFINE_BOOL(gow2_tracy_detail, false, "Gears2",
                    "Record high-volume renderer-batch, texture-lookup and guest-poll zones in Tracy. "
                    "Off keeps frame/phase, submission and query-completion timing with less capture overhead. Restart required");

namespace xenos::profiling {
#if defined(GN_HAVE_TRACY)
namespace {
rex::FrameHook previous_present_begin = nullptr, previous_present_end = nullptr;
struct PresentTiming {
  std::optional<tracy::ScopedZone> zone;
  std::chrono::steady_clock::time_point begin{}, previous{};
};
thread_local PresentTiming present_timing;
void PresentBegin() {
  if (previous_present_begin) previous_present_begin();
  if (!Enabled() || !TracyIsConnected) {
    present_timing.previous = {};
    return;
  }
  static constexpr tracy::SourceLocationData location{
      "DXGI Present", "PresentBegin", __FILE__, __LINE__, 0};
  const auto now = std::chrono::steady_clock::now();
  if (present_timing.previous.time_since_epoch().count()) {
    const double interval_ms = std::chrono::duration<double, std::milli>(
        now - present_timing.previous).count();
    TracyPlot("Native present interval ms", interval_ms);
  }
  present_timing.previous = present_timing.begin = now;
  present_timing.zone.emplace(&location, true);
}
void PresentEnd() {
  if (present_timing.zone) {
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - present_timing.begin).count();
    present_timing.zone.reset();
    if (Enabled() && TracyIsConnected) TracyPlot("Native Present API ms", ms);
  }
  if (previous_present_end) previous_present_end();
}
void InstallPresentHooks() {
  static std::once_flag once;
  std::call_once(once, [] {
    auto& hooks = rex::GetFrameHooks();
    previous_present_begin = hooks.present_begin.load(std::memory_order_acquire);
    previous_present_end = hooks.present_end.load(std::memory_order_acquire);
    hooks.present_begin.store(&PresentBegin, std::memory_order_release);
    hooks.present_end.store(&PresentEnd, std::memory_order_release);
  });
}
}  // namespace
#endif
void Initialize() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (!REXCVAR_GET(gow2_tracy)) {
      REXLOG_INFO("cogbox_tracy: disabled by --gow2_tracy=false");
      return;
    }
#if defined(GN_HAVE_TRACY)
    if (!tracy::IsProfilerStarted()) tracy::StartupProfiler();
    detailed.store(REXCVAR_GET(gow2_tracy_detail), std::memory_order_relaxed);
    enabled.store(true, std::memory_order_relaxed);
    // The process-lifetime renderer workers can still emit zones during SDK
    // teardown. Keep a client we started alive until process exit; never shut
    // down a client owned by the SDK here.
    REXLOG_INFO("cogbox_tracy: {}.{}.{} enabled on demand; connect a matching viewer to 127.0.0.1:8086 "
                "(CPU zones and frame/GPU-time plots)",
                tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch);
    REXLOG_INFO("cogbox_tracy: {} collection (--gow2_tracy_detail=true restores high-volume zones)",
                Detailed() ? "detailed" : "overview");
#else
    REXLOG_WARN("cogbox_tracy: not built in (configure with GN_WITH_TRACY=ON)");
#endif
  });
}

void ThreadName(const char* name) {
#if defined(GN_HAVE_TRACY)
  if (Enabled()) tracy::SetThreadName(name);
#else
  (void)name;
#endif
}

void Frame(double renderer_ms, double gpu_ms) {
#if defined(GN_HAVE_TRACY)
  if (!Enabled()) return;
  // Latency/Reflex hooks are installed before the first game frame. Chain them
  // so profiling also measures the SDK Present call without replacing markers.
  InstallPresentHooks();
  // Keep the cumulative timestamp baseline current while disconnected too.
  static double previous_gpu_ms = gpu_ms;
  const double completed_gpu_ms = gpu_ms >= previous_gpu_ms ? gpu_ms - previous_gpu_ms : 0.0;
  previous_gpu_ms = gpu_ms;
  if (!TracyIsConnected) return;
  FrameMark;
  TracyPlot("Renderer elapsed ms (includes waits)", renderer_ms);
  TracyPlot("Completed GPU ms (existing timestamps)", completed_gpu_ms);
#else
  (void)renderer_ms;
  (void)gpu_ms;
#endif
}
}  // namespace xenos::profiling
