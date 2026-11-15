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
  // Waits until the next frame's time for a limit of fps (<= 0: no wait, and the rhythm restarts).
  void Wait(double fps) {
    using clock = std::chrono::steady_clock;
    if (!(fps > 0.0)) { next_ = {}; return; }
    const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / fps));
    auto now = clock::now();
    if (fps != fps_ || next_.time_since_epoch().count() == 0 || now - next_ > period) next_ = now;  // first / changed / late
    fps_ = fps;
    if (now < next_) {
      // perf049: the yield spin before the deadline (1000 us in 0.4.8 = up to 1 ms of a core every frame). With the
      // high-resolution timer a shorter margin keeps the same pacing; without it (old Windows / Wine) keep 1000.
      const int32_t spin_us = std::clamp<int32_t>(REXCVAR_GET(gow2_frame_limiter_spin_us), 0, 4000);
      const auto margin = std::chrono::microseconds(HighResTimer() ? spin_us : (std::max)(spin_us, int32_t(1000)));
      const auto coarse = next_ - now - margin;
      if (coarse > std::chrono::microseconds(500)) SleepFor(coarse);
      while (clock::now() < next_) std::this_thread::yield();
    }
    next_ += period;
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

}  // namespace xenos
