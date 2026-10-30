#pragma once

#if defined(GN_HAVE_TRACY)
#include <atomic>
#include <cstring>
#include <tracy/Tracy.hpp>
#endif

namespace xenos::profiling {
void Initialize();
void ThreadName(const char* name);
void Frame(double renderer_ms, double gpu_ms);

#if defined(GN_HAVE_TRACY)
inline std::atomic<bool> enabled{false};
inline std::atomic<bool> detailed{false};
inline bool Enabled() {
  return enabled.load(std::memory_order_relaxed) && tracy::IsProfilerStarted();
}
inline bool Detailed() {
  return detailed.load(std::memory_order_relaxed) && Enabled();
}
#define COGBOX_TRACY_SCOPE(name) ZoneNamedN(_cogbox_tracy_zone, name, ::xenos::profiling::Enabled())
#define COGBOX_TRACY_DETAIL_SCOPE(name) ZoneNamedN(_cogbox_tracy_zone, name, ::xenos::profiling::Detailed())
#define COGBOX_TRACY_NAME(name) do { \
  if (::xenos::profiling::Enabled() && TracyIsConnected) \
    ZoneNameV(_cogbox_tracy_zone, name, std::strlen(name)); \
} while (0)
#else
inline bool Enabled() { return false; }
inline bool Detailed() { return false; }
#define COGBOX_TRACY_SCOPE(name)
#define COGBOX_TRACY_DETAIL_SCOPE(name)
#define COGBOX_TRACY_NAME(name)
#endif
}  // namespace xenos::profiling
