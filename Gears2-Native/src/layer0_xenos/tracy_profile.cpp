#include <xenos/tracy_profile.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <mutex>

#if defined(GN_HAVE_TRACY)
#include <common/TracyVersion.hpp>
#endif

REXCVAR_DEFINE_BOOL(gow2_tracy, true, "Gears2",
                    "Enable on-demand Tracy renderer profiling; connect a matching Tracy viewer to localhost:8086. "
                    "Restart required; false disables COGBox zones and startup");

namespace xenos::profiling {
void Initialize() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (!REXCVAR_GET(gow2_tracy)) {
      REXLOG_INFO("cogbox_tracy: disabled by --gow2_tracy=false");
      return;
    }
#if defined(GN_HAVE_TRACY)
    if (!tracy::IsProfilerStarted()) tracy::StartupProfiler();
    enabled.store(true, std::memory_order_relaxed);
    // The process-lifetime renderer workers can still emit zones during SDK
    // teardown. Keep a client we started alive until process exit; never shut
    // down a client owned by the SDK here.
    REXLOG_INFO("cogbox_tracy: {}.{}.{} enabled on demand; connect a matching viewer to 127.0.0.1:8086 "
                "(CPU zones and frame/GPU-time plots)",
                tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch);
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
