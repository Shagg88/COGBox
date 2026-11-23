// Layer 2 (Unreal Engine 3 on the 360): the engine tick rhythm log (ue3/tick_log.h). Moved here from Gears 3's and
// Judgment's game_patches.cpp (identical copies), 23 Nov parity pass. Our code (MIT).
#include "ue3/tick_log.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>

#include <rex/logging/macros.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ue3 {
namespace {
double Median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}
}  // namespace

void TickLog(const char* option, int32_t every_n) {  // the game thread is the only caller
#ifdef _WIN32
  if (every_n <= 0) return;
  static const uint64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return uint64_t(f.QuadPart); }();
  static uint64_t last = 0, ticks = 0;
  static std::vector<double> iv;  // tick lengths, ms
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const uint64_t t = uint64_t(now.QuadPart);
  if (last) iv.push_back(double(t - last) * 1000.0 / double(freq));
  last = t;
  ++ticks;
  if (int32_t(iv.size()) < every_n) return;
  std::vector<double> sorted = iv;
  std::sort(sorted.begin(), sorted.end());
  const double med = sorted[sorted.size() / 2], p95 = sorted[sorted.size() * 95 / 100], mx = sorted.back();
  const double slow_at = std::max(med * 1.5, med + 4.0);
  std::vector<size_t> slow;
  double slow_mean = 0;
  for (size_t i = 0; i < iv.size(); ++i)
    if (iv[i] > slow_at) slow.push_back(i), slow_mean += iv[i];
  if (!slow.empty()) slow_mean /= double(slow.size());
  std::string rhythm = "no slow ticks";
  if (slow.size() >= 3) {
    std::vector<double> gap_ms, gap_ticks;
    for (size_t k = 1; k < slow.size(); ++k) {
      double ms = 0;
      for (size_t i = slow[k - 1] + 1; i <= slow[k]; ++i) ms += iv[i];
      gap_ms.push_back(ms);
      gap_ticks.push_back(double(slow[k] - slow[k - 1]));
    }
    const double gm = Median(gap_ms), gt = Median(gap_ticks);
    std::vector<double> dev;
    for (double g : gap_ms) dev.push_back(std::fabs(g - gm));
    rhythm = std::format("every {:.0f} ms / {:.0f} ticks (spread {:.2f})", gm, gt, gm > 0 ? Median(dev) / gm : 0.0);
  } else if (!slow.empty()) {
    rhythm = std::format("{} slow tick(s)", slow.size());
  }
  REXLOG_INFO("{}: ticks {}-{}: median {:.2f} ms, p95 {:.2f}, max {:.1f} | slow (> {:.1f} ms): {} (mean {:.1f} ms), {}",
              option, ticks - iv.size(), ticks - 1, med, p95, mx, slow_at, slow.size(), slow_mean, rhythm);
  iv.clear();
#else
  (void)option;
  (void)every_n;
#endif
}

}  // namespace ue3
