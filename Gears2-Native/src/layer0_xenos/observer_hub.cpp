// Layer 0 (game-free): the SDK allows one draw observer per process (SDK patch 2). This hub is that one observer and
// forwards every callback to the layer-0 tools that registered (draw stats, frame capture, later the Stage 0
// comparison). Register before the game starts; nothing is ever unregistered.
//
// The hub also keeps the swap count (frame-timed scripted input) and, with --gow2_frame_times=N, measures its own cost:
// every N swaps it logs the swap-to-swap interval (mean, p99, max) and the time spent inside the forwarded callbacks
// per frame, so the observer's overhead can be read straight off the log (D19 item 4, notes/34 item 3).
#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/graphics/draw_observer.h>
#include <rex/logging/macros.h>

#include <intrin.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

REXCVAR_DEFINE_INT32(gow2_frame_times, 0, "Gears2",
                     "Log swap interval and observer callback time every N swaps (0 = off)");

namespace {

using namespace rex::graphics;

std::atomic<uint64_t> g_swaps{0};

class ObserverHub final : public DrawObserver {
 public:
  void Add(DrawObserver* o) {
    std::lock_guard<std::mutex> lock(mutex_);
    observers_.push_back(o);
    wants_images_ = wants_images_ || o->WantsFrameImages();
  }
  void OnDraw(const RegisterFile& regs, xenos::PrimitiveType primitive, uint32_t index_count,
              const ObservedIndexBuffer* index_buffer, bool major_mode_explicit) override {
    if (observers_.empty()) return;
    const uint64_t t0 = timing_ ? __rdtsc() : 0;
    for (DrawObserver* o : observers_) o->OnDraw(regs, primitive, index_count, index_buffer, major_mode_explicit);
    if (timing_) cb_ticks_ += __rdtsc() - t0;
  }
  void OnDrawShaders(const Shader* vertex_shader, const Shader* pixel_shader) override {
    if (observers_.empty()) return;
    const uint64_t t0 = timing_ ? __rdtsc() : 0;
    for (DrawObserver* o : observers_) o->OnDrawShaders(vertex_shader, pixel_shader);
    if (timing_) cb_ticks_ += __rdtsc() - t0;
  }
  void OnZpd(const RegisterFile& regs, const ObservedZpd& zpd) override {  // SDK patch 15
    if (observers_.empty()) return;
    const uint64_t t0 = timing_ ? __rdtsc() : 0;
    for (DrawObserver* o : observers_) o->OnZpd(regs, zpd);
    if (timing_) cb_ticks_ += __rdtsc() - t0;
  }
  void OnPrimaryRange(uint32_t ring_physical, uint32_t ring_dwords, uint32_t read_index,
                      uint32_t write_index) override {  // SDK patch 20
    if (observers_.empty()) return;
    for (DrawObserver* o : observers_) o->OnPrimaryRange(ring_physical, ring_dwords, read_index, write_index);
  }
  void OnSwap(const RegisterFile& regs, uint32_t frontbuffer, uint32_t width, uint32_t height) override {
    const uint64_t t0 = timing_ ? __rdtsc() : 0;
    for (DrawObserver* o : observers_) o->OnSwap(regs, frontbuffer, width, height);
    if (timing_) { cb_ticks_ += __rdtsc() - t0; FrameTiming(); }
    g_swaps.fetch_add(1, std::memory_order_relaxed);
  }
  // Stage 5B phase 3: a draw our PM4 processor ran (XenosDispatchOurDraw).
  void OurDraw(const RegisterFile& regs, xenos::PrimitiveType primitive, uint32_t index_count,
               const ObservedIndexBuffer* index_buffer, bool major_mode_explicit, const XenosUcode& vs,
               const XenosUcode& ps) {
    if (observers_.empty()) return;
    const uint64_t t0 = timing_ ? __rdtsc() : 0;
    for (DrawObserver* o : observers_) {
      if (auto* sink = dynamic_cast<XenosUcodeSink*>(o)) sink->OnDrawUcode(vs, ps);
      else o->OnDrawShaders(nullptr, nullptr);
      o->OnDraw(regs, primitive, index_count, index_buffer, major_mode_explicit);
    }
    if (timing_) cb_ticks_ += __rdtsc() - t0;
  }
  bool WantsFrameImages() const override { return wants_images_; }
  void OnFrameImage(const uint8_t* rgb10a2, uint32_t width, uint32_t height, uint32_t row_pitch,
                    uint64_t frame) override {
    const uint64_t t0 = timing_ ? __rdtsc() : 0;
    for (DrawObserver* o : observers_)
      if (o->WantsFrameImages()) o->OnFrameImage(rgb10a2, width, height, row_pitch, frame);
    if (timing_) cb_ticks_ += __rdtsc() - t0;
  }
  void EnableTiming(int every) {
    every_ = every;
    timing_ = every > 0;
    // Calibrate rdtsc against QueryPerformanceCounter over ~50 ms (invariant TSC on current CPUs).
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    const uint64_t r0 = __rdtsc();
    do { QueryPerformanceCounter(&b); } while ((b.QuadPart - a.QuadPart) * 20 < f.QuadPart);
    tsc_per_ms_ = double(__rdtsc() - r0) / (double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart));
  }

 private:
  void FrameTiming() {
    const uint64_t now = __rdtsc();
    if (last_swap_) {
      intervals_.push_back(double(now - last_swap_) / tsc_per_ms_);
      callbacks_.push_back(double(cb_ticks_) / tsc_per_ms_);
    }
    last_swap_ = now;
    cb_ticks_ = 0;
    if (int(intervals_.size()) >= every_) {
      std::vector<double> s = intervals_;
      std::sort(s.begin(), s.end());
      double mean = 0, cb = 0, cbmax = 0;
      for (double v : intervals_) mean += v;
      for (double v : callbacks_) { cb += v; cbmax = std::max(cbmax, v); }
      mean /= s.size(); cb /= callbacks_.size();
      REXLOG_INFO("gow2_frame_times: swaps {}-{} interval mean {:.2f} ms p99 {:.2f} max {:.2f} | observer callbacks mean {:.3f} ms/frame max {:.3f} ({} tool(s))",
                  g_swaps.load() + 1 - s.size(), g_swaps.load(), mean, s[size_t(0.99 * (s.size() - 1))], s.back(), cb, cbmax,
                  observers_.size());
      intervals_.clear(); callbacks_.clear();
    }
  }

  std::mutex mutex_;
  std::vector<DrawObserver*> observers_;  // only appended before the guest starts
  bool wants_images_ = false;
  bool timing_ = false;
  int every_ = 0;
  double tsc_per_ms_ = 1.0;
  uint64_t cb_ticks_ = 0, last_swap_ = 0;   // GPU worker thread only
  std::vector<double> intervals_, callbacks_;
};

ObserverHub& Hub() {
  static ObserverHub hub;
  return hub;
}

}  // namespace

void XenosRegisterDrawObserver(rex::graphics::DrawObserver* observer) {
  Hub().Add(observer);
  SetDrawObserver(&Hub());
}

void XenosEnsureObserverHub() { SetDrawObserver(&Hub()); }

void XenosDispatchOurDraw(const rex::graphics::RegisterFile& regs, uint32_t primitive, uint32_t index_count,
                          const rex::graphics::ObservedIndexBuffer* index_buffer, bool major_mode_explicit,
                          const XenosUcode& vertex, const XenosUcode& pixel) {
  Hub().OurDraw(regs, rex::graphics::xenos::PrimitiveType(primitive), index_count, index_buffer, major_mode_explicit,
                vertex, pixel);
}

void XenosDispatchOurSwap(const rex::graphics::RegisterFile& regs, uint32_t frontbuffer, uint32_t width,
                          uint32_t height) {
  Hub().OnSwap(regs, frontbuffer, width, height);
}

bool XenosFrameImagesWanted() { return Hub().WantsFrameImages(); }

void XenosDispatchFrameImage(const uint8_t* rgb10a2, uint32_t width, uint32_t height, uint32_t row_pitch,
                             uint64_t frame) {
  Hub().OnFrameImage(rgb10a2, width, height, row_pitch, frame);
}

void XenosDispatchOurZpd(const rex::graphics::RegisterFile& regs, uint32_t report_address, bool is_end,
                         uint8_t answer, uint64_t samples) {
  Hub().OnZpd(regs, rex::graphics::ObservedZpd{report_address, is_end, rex::graphics::ZpdAnswer(answer), samples, 0});
}

uint64_t XenosSwapCount() { return g_swaps.load(std::memory_order_relaxed); }

void XenosInstallFrameTimesIfRequested() {
  const int every = REXCVAR_GET(gow2_frame_times);
  if (every <= 0) return;
  Hub().EnableTiming(every);
  XenosEnsureObserverHub();
  REXLOG_INFO("gow2_frame_times: logging every {} swaps", every);
}
