// Shared precise frame-rate limiter (all four games). UE3's own limiter (GetMaxTickRate -> FEngineLoop's wait) sleeps in
// whole guest milliseconds, so a cap of N gave about N +- 2 (Gears 3: 62.1 at 62, Judgment: 162 at 165, 121-123 at 120).
// This one waits on the host clock until the next frame's deadline: a high-resolution waitable timer to ~1 ms before it,
// then a yield spin. The deadline moves by exactly 1/N s each call, so the average is N; after a slow frame (more than
// one period late) it restarts from now instead of catching up in a burst, so the rate never reads above N.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/logging/macros.h>

// perf049: how long before the deadline the limiter stops sleeping and spins (frame_log.cpp; default 1000 = as 0.4.8).
REXCVAR_DECLARE(int32_t, gow2_frame_limiter_spin_us);

namespace xenos {

class FrameLimiter {
 public:
  // Waits until the next frame's time for a limit of fps (<= 0: no wait, and the rhythm restarts). Returns how long it
  // waited, in ms (0 = the frame was already late: the game was slower than the limit) - GitHub #313's pace check.
  double Wait(double fps) {
    using clock = std::chrono::steady_clock;
    if (!(fps > 0.0)) { next_ = {}; return 0.0; }
    const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / fps));
    auto now = clock::now();
    if (fps != fps_ || next_.time_since_epoch().count() == 0 || now - next_ > period) next_ = now;  // first / changed / late
    fps_ = fps;
    double waited_ms = 0.0;
    if (now < next_) {
      // perf049: the yield spin before the deadline (1000 us in 0.4.8 = up to 1 ms of a core every frame). With the
      // high-resolution timer a shorter margin keeps the same pacing; without it (old Windows / Wine) keep 1000.
      const int32_t spin_us = std::clamp<int32_t>(REXCVAR_GET(gow2_frame_limiter_spin_us), 0, 4000);
      const auto margin = std::chrono::microseconds(HighResTimer() ? spin_us : (std::max)(spin_us, int32_t(1000)));
      const auto coarse = next_ - now - margin;
      if (coarse > std::chrono::microseconds(500)) SleepFor(coarse);
      while (clock::now() < next_) std::this_thread::yield();
      waited_ms = std::chrono::duration<double, std::milli>(clock::now() - now).count();
    }
    next_ += period;
    return waited_ms;
  }

 private:
#ifdef _WIN32
  struct Timer { HANDLE h = nullptr; bool high_res = false; };
  static Timer& ThreadTimer() {
    static thread_local Timer t = [] {
      Timer r;
      r.h = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
      r.high_res = r.h != nullptr;
      if (!r.h) r.h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
      return r;
    }();
    return t;
  }
  static bool HighResTimer() { return ThreadTimer().high_res; }
#else
  static bool HighResTimer() { return false; }
#endif
  static void SleepFor(std::chrono::steady_clock::duration d) {
#ifdef _WIN32
    const HANDLE timer = ThreadTimer().h;
    if (timer) {
      LARGE_INTEGER due;
      due.QuadPart = -int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count() / 100);  // relative, 100 ns
      if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) { WaitForSingleObject(timer, INFINITE); return; }
    }
#endif
    std::this_thread::sleep_for(d);
  }
  std::chrono::steady_clock::time_point next_{};
  double fps_ = 0.0;
};

// 0.6.3 (Discord, Gears 3 host: 60 -> 30, 144 -> 72 while a friend is in the match; unlimited not affected): UE3 asks
// GEngine->GetMaxTickRate twice per frame on a host with remote players - once from the engine loop (the frame wait) and
// once from UWorld::ServerTickClients, which runs only while NetDriver->ClientConnections is non-empty and uses the value
// only for the replication interval ("ServerTickTime": 0 -> DeltaSeconds, else 1/x). The exact limiters waited a full
// frame on every call, so a host waited twice per frame. The games now pass the guest return address of each call
// (the GetMaxTickRate override's ctx.lr) and skip the wait (and the per-frame ticks) for the ServerTickClients one.
// This counter logs each call site once, and - only when more than one site is in use - how many calls each made per
// window of engine-loop calls, so a report shows a second caller. Game thread only. Read-only.
class TickCallSites {
 public:
  // lr: the call's guest return address; main: it is an engine-loop call (the one that waits).
  void Note(const char* tag, uint32_t lr, bool main) {
    int i = 0;
    while (i < n_ && site_[i] != lr) ++i;
    if (i == n_) {
      if (n_ == kMax) return;  // more sites than kept: not counted (logged below once)
      site_[n_] = lr, count_[n_] = 0, main_[n_] = main, ++n_;
      REXLOG_INFO("{}: GetMaxTickRate called from 0x{:08X} ({})", tag, lr,
                  main ? "engine loop - the frame-rate limit waits here" : "UWorld::ServerTickClients (host with remote "
                  "players) - no wait, the limit is only reported");
      if (n_ == kMax) REXLOG_INFO("{}: {} GetMaxTickRate call sites seen - later new ones are not counted", tag, kMax);
    }
    ++count_[i];
    if (main && ++frames_ >= kWindow) {
      if (n_ > 1 && lines_ < 30) {
        ++lines_;
        std::string s;
        for (int k = 0; k < n_; ++k) s += std::string(k ? ", " : "") + Hex(site_[k]) + (main_[k] ? "=" : " (server)=") + std::to_string(count_[k]);
        REXLOG_INFO("{}: GetMaxTickRate calls in the last {} engine-loop calls: {}", tag, frames_, s);
      }
      frames_ = 0;
      for (int k = 0; k < n_; ++k) count_[k] = 0;
    }
  }

 private:
  static std::string Hex(uint32_t v) {
    char b[12];
    std::snprintf(b, sizeof b, "0x%08X", unsigned(v));
    return b;
  }
  static constexpr int kMax = 8;
  static constexpr uint32_t kWindow = 1800;  // 30 s at 60
  uint32_t site_[kMax] = {}, count_[kMax] = {};
  bool main_[kMax] = {};
  int n_ = 0, lines_ = 0;
  uint32_t frames_ = 0;
};

// GitHub #313 (Gears 2 and 3, a 120 limit "stops working" in split screen, vsync on or off; no log yet). In the
// recompiled code of both games (disc and TU6) nothing on the limiter's way depends on the number of local players:
// the engine loop asks GEngine->GetMaxTickRate every frame unless GIsBenchmarking / GUseFixedTimeStep (start-up flags),
// and GetMaxTickRate reaches our hook whenever its smoothing flag is set - written once, at start-up. So the report has
// to tell which of three things happens. Every 10 s one line: engine frames per second, game frames handed to the screen
// per second (swaps), the limit asked, and in how many frames the limiter had to wait (and for how long on average).
//  - engine frames above the limit: the limit is not applied (frames that did not reach the limiter are counted too);
//  - frames shown above engine frames: more than one picture per engine frame;
//  - engine frames below the limit with few waits: the game (CPU or GPU) is slower than the limit, nothing is broken.
// Lines while 2+ local players are in (at most 120), and one after each change in the number of players (so a report
// has the one-player rate beside it). Game thread only (the engine loop's GetMaxTickRate call). Read-only.
class PaceCheck {
 public:
  // waited_ms: the limiter's wait in this frame (0 = none needed; < 0 = the limiter did not run in this frame).
  // swaps: XenosSwapCount(). players(): local players now (-1 = unknown), called only when a window ends.
  template <class Players>
  void Tick(const char* tag, int32_t limit_asked, double waited_ms, uint64_t swaps, Players&& players) {
    using clock = std::chrono::steady_clock;
    const clock::time_point now = clock::now();
    if (start_.time_since_epoch().count() == 0) { start_ = now, swaps_ = swaps; return; }
    ++ticks_;
    if (waited_ms < 0.0) ++missed_;
    else if (waited_ms > 0.0) ++waits_, wait_ms_ += waited_ms;
    const double span = std::chrono::duration<double>(now - start_).count();
    if (span < 10.0) return;
    const int p = players();
    const bool changed = p != last_players_;
    last_players_ = p;
    if ((p >= 2 || changed) && lines_ < 120) {
      ++lines_;
      REXLOG_INFO("{}: pace, local players {}: engine frames {:.1f}/s, frames shown {:.1f}/s, limit asked {} ({}); the "
                  "limiter waited in {:.0f}% of frames (mean {:.2f} ms), did not run in {} of {}",
                  tag, p, double(ticks_) / span, double(swaps - swaps_) / span, limit_asked,
                  limit_asked > 0 ? "fps" : (limit_asked < 0 ? "none" : "the game's own"),
                  ticks_ ? 100.0 * double(waits_) / double(ticks_) : 0.0, waits_ ? wait_ms_ / double(waits_) : 0.0,
                  missed_, ticks_);
    }
    start_ = now, swaps_ = swaps, ticks_ = waits_ = missed_ = 0, wait_ms_ = 0.0;
  }

 private:
  std::chrono::steady_clock::time_point start_{};
  uint64_t swaps_ = 0, ticks_ = 0, waits_ = 0, missed_ = 0;
  double wait_ms_ = 0.0;
  int last_players_ = -2, lines_ = 0;
};

}  // namespace xenos
