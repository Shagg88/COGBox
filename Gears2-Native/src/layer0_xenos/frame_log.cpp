// Layer 0 (game-free): the frame log (--gow2_frame_log, off by default: a developer tool, Shagg 28 Oct). Records every frame's time and what it went on,
// for all three games:
//   - with --gow2_frame_log_csv, <log name>-frames.csv next to the log file (--log_file; else
//     --gow2_crash_dir\frames.csv): one row per frame;
//   - every --gow2_frame_log_every frames a summary line (frame time mean / p50 / p95 / p99 / max, 1% and 0.1% low,
//     frame-to-frame jitter, spikes by cause, and where the average frame went);
//   - one line per spike (up to --gow2_frame_log_spike_lines per summary window) with its full breakdown.
//
// Two clocks are compared. The game frame is measured on the GPU thread, swap to swap as the game hands frames over
// (XenosFrameGuestSwap, pm4_shadow.cpp); the shown frame on our renderer thread at the end of each swap it runs
// (XenosFrameRendered, shadow_renderer.cpp). Each shown frame is paired with the oldest game frame not yet shown
// (swaps run in order). Waits made
// outside the renderer are charged to the game frame they happened in: the game's own waits for the GPU and for query
// answers (layer 3 hooks, XenosFrameWaitBegin / End), the GPU thread's WAIT_REG_MEM spins and its waits at a swap for
// our renderer thread (pm4_shadow.cpp).
//
// The cause of a frame is decided like this: when our renderer thread was busy for at least half of the frame, the
// biggest of its parts (shader hold, draws waiting for new pipelines, waiting for our GPU, its own CPU work - with the
// biggest CPU part when --gow2_renderer_cpu_split >= 1); otherwise the renderer sat waiting for the game's next frame,
// and the cause is the biggest part of that game frame (the game waiting for the GPU, for a query, the GPU thread at a
// WAIT_REG_MEM, the GPU thread waiting for our renderer, or the rest: the game's own CPU work).
// Our code (MIT).
#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

REXCVAR_DEFINE_BOOL(gow2_frame_log, false, "Gears2",
                    "Frame log: every frame's time and its cause - <log name>-frames.csv next to the log, a summary "
                    "line every --gow2_frame_log_every frames and a line per spike (all three games)");
REXCVAR_DEFINE_INT32(gow2_frame_log_every, 600, "Gears2", "Frame log: frames per summary line (0 = no summary lines)");
REXCVAR_DEFINE_DOUBLE(gow2_frame_log_spike_ms, 0.0, "Gears2",
                      "Frame log: a frame longer than this is a spike (0 = automatic: over 1.5x the running average "
                      "frame time and at least 4 ms over it)");
REXCVAR_DEFINE_INT32(gow2_frame_log_spike_lines, 12, "Gears2",
                     "Frame log: spike lines per summary window (the rest are counted in the summary; 0 = none)");
REXCVAR_DEFINE_BOOL(gow2_frame_log_csv, false, "Gears2",
                    "Frame log: also write the per-frame CSV file (~200 bytes a frame: ~45 MB an hour at 60 fps)");
REXCVAR_DECLARE(std::string, gow2_crash_dir);  // crash_report.cpp

namespace {

uint64_t Qpc() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return uint64_t(t.QuadPart);
}
uint64_t QpcFreq() {
  static const uint64_t f = [] {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return uint64_t(v.QuadPart);
  }();
  return f;
}
double TicksToMs(uint64_t ticks) { return double(ticks) * 1000.0 / double(QpcFreq()); }

// Waits, charged as they happen (any thread); the GPU thread takes the difference at each game swap.
std::atomic<uint64_t> g_wait_ticks[kXenosWaitCount] = {};
std::atomic<uint64_t> g_wait_count[kXenosWaitCount] = {};

constexpr const char* kWaitNames[kXenosWaitCount] = {"game-gpu-wait", "game-query-wait", "gpu-thread-wait",
                                                     "renderer-behind"};

struct GuestFrame {
  uint64_t index = UINT64_MAX;
  uint64_t t = 0;   // QPC at its swap
  double ms = 0.0;  // swap to swap
  double wait_ms[kXenosWaitCount] = {};
  uint32_t waits[kXenosWaitCount] = {};
};

class FrameLog {
 public:
  // GPU thread.
  void GuestSwap() {
    const uint64_t now = Qpc();
    std::lock_guard<std::mutex> lock(ring_mutex_);
    // Swaps run in order, so the renderer's next swap is the oldest game frame it has not taken yet. More than a few
    // waiting means swaps that never reach our renderer (it is off, lost or not presenting): drop the oldest.
    while (pending_.size() >= kPendingMax) pending_.pop_front();
    GuestFrame& g = pending_.emplace_back();
    g.index = guest_n_++;
    g.t = now;
    g.ms = guest_last_ ? TicksToMs(now - guest_last_) : 0.0;
    guest_last_ = now;
    for (uint32_t k = 0; k < kXenosWaitCount; ++k) {
      const uint64_t t = g_wait_ticks[k].load(std::memory_order_relaxed);
      const uint64_t n = g_wait_count[k].load(std::memory_order_relaxed);
      g.wait_ms[k] = TicksToMs(t - guest_ticks_[k]);
      g.waits[k] = uint32_t(n - guest_count_[k]);
      guest_ticks_[k] = t;
      guest_count_[k] = n;
    }
  }

  // Our renderer thread.
  void Rendered(const XenosRenderedFrame& f) {
    const uint64_t now = Qpc();
    const uint64_t i = render_n_++;
    const double frame_ms = render_last_ ? TicksToMs(now - render_last_) : 0.0;
    render_last_ = now;
    if (!t0_) t0_ = now;
    GuestFrame g;
    bool have_guest = false;
    {
      std::lock_guard<std::mutex> lock(ring_mutex_);
      if (!pending_.empty()) {
        g = pending_.front();
        pending_.pop_front();
        have_guest = true;
      }
    }
    if (frame_ms <= 0.0) return;  // the first frame has no interval

    // ---- cause ----
    const double rend_other = std::max(0.0, f.cpu_ms - f.gpu_wait_ms - f.hold_ms - f.pipeline_wait_ms);
    std::string cause;
    double cause_ms = 0.0;
    auto consider = [&](const char* name, double ms) {
      if (ms > cause_ms) {
        cause_ms = ms;
        cause = name;
      }
    };
    double game_cpu = 0.0;
    if (have_guest) {
      double waited = 0.0;
      for (uint32_t k = 0; k < kXenosWaitCount; ++k) waited += g.wait_ms[k];
      game_cpu = std::max(0.0, g.ms - waited);
    }
    if (f.cpu_ms >= 0.5 * frame_ms || !have_guest) {
      consider("shader-hold", f.hold_ms);
      consider("pipeline-wait", f.pipeline_wait_ms);
      consider("gpu-bound", f.gpu_wait_ms);
      consider("renderer-cpu", rend_other);
      if (cause == "renderer-cpu" && f.top_part && *f.top_part) cause = std::string("renderer-cpu:") + f.top_part;
    } else {
      for (uint32_t k = 0; k < kXenosWaitCount; ++k) consider(kWaitNames[k], g.wait_ms[k]);
      consider("game-cpu", game_cpu);
    }
    if (cause.empty()) cause = "unknown";

    // ---- spike ----
    const double fixed = REXCVAR_GET(gow2_frame_log_spike_ms);
    const bool stall = frame_ms >= kStallMs;  // a load or a pause, not pacing: counted apart
    bool spike = false;
    if (!stall) {
      if (fixed > 0.0) spike = frame_ms > fixed;
      else spike = warm_ >= 30 && frame_ms > std::max(avg_ * 1.5, avg_ + 4.0);
      if (!spike) {
        avg_ = warm_ ? avg_ * 0.95 + frame_ms * 0.05 : frame_ms;
        ++warm_;
      }
    }

    // ---- CSV row ----
    if (csv_ && !csv_failed_.load(std::memory_order_relaxed)) {
      if (!csv_started_) StartCsv();
      csv_buf_ += std::format(
          "{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{:.3f},{},"
          "{:.3f},{},{:.3f},{},{},{},{},{},{:.2f},{},{}\n",
          i, TicksToMs(now - t0_) / 1000.0, frame_ms, have_guest ? g.ms : 0.0,
          have_guest ? TicksToMs(now - g.t) : 0.0, f.cpu_ms, rend_other, f.top_part ? f.top_part : "", f.top_part_ms,
          f.gpu_wait_ms, f.hold_ms, f.pipeline_wait_ms, f.gpu_ms, g.wait_ms[kXenosWaitGameGpu],
          g.waits[kXenosWaitGameGpu], g.wait_ms[kXenosWaitGameQuery], g.waits[kXenosWaitGameQuery],
          g.wait_ms[kXenosWaitPm4], g.waits[kXenosWaitPm4], g.wait_ms[kXenosWaitRenderer], f.draws, f.skipped_draws,
          f.pipelines_new, f.shaders_new, have_guest ? 1 : 0, f.upload_mb, stall ? 2 : spike ? 1 : 0, cause);
      if (++csv_rows_ % 30 == 0) FlushCsv();
    }

    // ---- window ----
    if (stall) {
      ++w_.stalls;
      w_.stall_ms += frame_ms;
    } else {
      w_.frame.push_back(frame_ms);
      if (have_guest) w_.game.push_back(g.ms);
      if (prev_ms_ > 0.0) {
        w_.jitter += std::abs(frame_ms - prev_ms_);
        ++w_.jitter_n;
      }
      w_.rend_cpu += rend_other;
      w_.gpu_wait += f.gpu_wait_ms;
      w_.hold += f.hold_ms;
      w_.pipe_wait += f.pipeline_wait_ms;
      w_.gpu += f.gpu_ms;
      w_.game_cpu += game_cpu;
      for (uint32_t k = 0; k < kXenosWaitCount; ++k) {
        w_.wait[k] += g.wait_ms[k];
        w_.waits[k] += g.waits[k];
      }
      ++w_.causes[cause];
    }
    prev_ms_ = stall ? 0.0 : frame_ms;
    if (spike) {
      ++w_.spikes[cause];
      ++w_.spike_n;
      if (w_.spike_lines < uint32_t(std::max(0, REXCVAR_GET(gow2_frame_log_spike_lines)))) {
        ++w_.spike_lines;
        REXLOG_INFO(
            "gow2_frame_log: SPIKE frame {}: {:.1f} ms (running average {:.1f}) - cause {} {:.1f} ms | game frame "
            "{} | renderer {:.1f} ms busy: own CPU {:.1f}{}, waiting for our GPU {:.1f}, shader hold {:.1f}, draws "
            "waiting for pipelines {:.1f} | our GPU {:.1f} ms | {} draws ({} skipped, shaders pending), new pipelines "
            "{}, new shaders {}, uploads {:.1f} MB",
            i, frame_ms, avg_, cause, cause_ms,
            have_guest ? std::format("{:.1f} ms: game GPU waits {:.1f} ms ({}), query waits {:.1f} ms ({}), GPU "
                                     "thread WAIT_REG_MEM {:.1f} ms ({}), GPU thread waiting for renderer {:.1f} ms, "
                                     "rest (game CPU) {:.1f} ms, frame shown {:.1f} ms after the game's swap",
                                     g.ms, g.wait_ms[0], g.waits[0], g.wait_ms[1], g.waits[1], g.wait_ms[2], g.waits[2],
                                     g.wait_ms[3], game_cpu, TicksToMs(now - g.t))
                       : std::string("not measured"),
            f.cpu_ms, rend_other,
            f.top_part && *f.top_part ? std::format(" (most in {} {:.1f})", f.top_part, f.top_part_ms) : std::string(),
            f.gpu_wait_ms, f.hold_ms, f.pipeline_wait_ms, f.gpu_ms, f.draws, f.skipped_draws, f.pipelines_new,
            f.shaders_new, f.upload_mb);
      }
    }
    const int every = REXCVAR_GET(gow2_frame_log_every);
    if (every > 0 && w_.frame.size() + w_.stalls >= size_t(every)) Summary(i);
  }

 private:
  static constexpr size_t kPendingMax = 8;
  static constexpr double kStallMs = 250.0;

  struct Window {
    std::vector<double> frame, game;
    double jitter = 0.0;
    uint64_t jitter_n = 0;
    double rend_cpu = 0, gpu_wait = 0, hold = 0, pipe_wait = 0, gpu = 0, game_cpu = 0;
    double wait[kXenosWaitCount] = {};
    uint64_t waits[kXenosWaitCount] = {};
    std::map<std::string, uint32_t> causes, spikes;
    uint32_t spike_n = 0, spike_lines = 0, stalls = 0;
    double stall_ms = 0.0;
  };

  void Summary(uint64_t last) {
    Window& w = w_;
    const size_t n = w.frame.size();
    if (n) {
      std::vector<double> s = w.frame;
      std::sort(s.begin(), s.end());
      auto pct = [&](double p) { return s[std::min(s.size() - 1, size_t(p * double(s.size() - 1) + 0.5))]; };
      double sum = 0.0;
      for (double v : s) sum += v;
      const double mean = sum / double(n);
      auto low = [&](double share) {  // average of the slowest share of frames, as fps
        const size_t k = std::max<size_t>(1, size_t(double(n) * share));
        double t = 0.0;
        for (size_t j = 0; j < k; ++j) t += s[n - 1 - j];
        return 1000.0 / (t / double(k));
      };
      double game_mean = 0.0;
      for (double v : w.game) game_mean += v;
      if (!w.game.empty()) game_mean /= double(w.game.size());
      auto list = [](const std::map<std::string, uint32_t>& m) {
        std::vector<std::pair<uint32_t, std::string>> v;
        for (const auto& [k, c] : m) v.push_back({c, k});
        std::sort(v.rbegin(), v.rend());
        std::string t;
        for (size_t j = 0; j < v.size() && j < 6; ++j) t += std::format("{}{} {}", j ? ", " : "", v[j].second, v[j].first);
        return t.empty() ? std::string("none") : t;
      };
      const double d = double(n);
      REXLOG_INFO(
          "gow2_frame_log: frames {}-{}: frame time mean {:.2f} ms ({:.1f} fps), p50 {:.2f}, p95 {:.2f}, p99 {:.2f}, "
          "max {:.2f} | 1% low {:.1f} fps, 0.1% low {:.1f} fps | jitter (frame-to-frame change) {:.2f} ms | spikes {}: "
          "{} | per frame: game {:.2f} ms (own CPU {:.2f}, GPU waits {:.2f} ms x{:.1f}, query waits {:.2f} ms x{:.1f}, GPU "
          "thread WAIT_REG_MEM {:.2f} ms x{:.1f}, GPU thread waiting for renderer {:.2f} ms), renderer own CPU {:.2f} "
          "ms, waiting for our GPU {:.2f} ms, shader holds {:.2f} ms, pipeline waits {:.2f} ms, our GPU {:.2f} ms | "
          "frames by cause: {}{}",
          last + 1 - n - w.stalls, last, mean, 1000.0 / mean, pct(0.50), pct(0.95), pct(0.99), s.back(), low(0.01),
          low(0.001), w.jitter_n ? w.jitter / double(w.jitter_n) : 0.0, w.spike_n, list(w.spikes), game_mean,
          w.game_cpu / d, w.wait[0] / d, double(w.waits[0]) / d, w.wait[1] / d, double(w.waits[1]) / d, w.wait[2] / d,
          double(w.waits[2]) / d, w.wait[3] / d, w.rend_cpu / d, w.gpu_wait / d, w.hold / d, w.pipe_wait / d,
          w.gpu / d, list(w.causes),
          w.stalls ? std::format(" | stalls over {:.0f} ms (loads, pauses; not in the numbers above): {} ({:.1f} s)",
                                 kStallMs, w.stalls, w.stall_ms / 1000.0)
                   : std::string());
      if (w.spike_n > w.spike_lines)
        REXLOG_INFO("gow2_frame_log: {} more spike(s) in frames {}-{} not listed (--gow2_frame_log_spike_lines); all "
                    "are in the CSV", w.spike_n - w.spike_lines, last + 1 - n - w.stalls, last);
    }
    w_ = Window{};
  }

  // ---- CSV: written by its own thread, so the renderer thread never waits for the disk ----
  void StartCsv() {
    csv_started_ = true;
    std::filesystem::path path;
    const std::string log = rex::cvar::Query<std::string>("log_file");
    if (!log.empty()) {
      const std::filesystem::path lp(log);
      path = lp.parent_path() / (lp.stem().string() + "-frames.csv");
    } else {
      const std::string dir = REXCVAR_GET(gow2_crash_dir);
      path = dir.empty() ? std::filesystem::path("frames.csv") : std::filesystem::path(dir) / "frames.csv";
    }
    REXLOG_INFO("gow2_frame_log: per-frame CSV {} (frame times, cause per frame; stall=2 / spike=1 in the spike "
                "column)", path.string());
    csv_buf_ =
        "frame,time_s,frame_ms,game_frame_ms,shown_after_game_swap_ms,renderer_busy_ms,renderer_own_cpu_ms,"
        "renderer_top_part,renderer_top_part_ms,renderer_gpu_wait_ms,shader_hold_ms,pipeline_wait_ms,our_gpu_ms,"
        "game_gpu_wait_ms,game_gpu_waits,game_query_wait_ms,game_query_waits,gpu_thread_wait_reg_mem_ms,"
        "gpu_thread_wait_reg_mem_waits,gpu_thread_waiting_for_renderer_ms,draws,skipped_draws,new_pipelines,"
        "new_shaders,game_frame_matched,upload_mb,spike,cause\n";
    std::thread([this, path] { CsvThread(path); }).detach();  // lives as long as the process (never destroyed)
  }
  void FlushCsv() {
    {
      std::lock_guard<std::mutex> lock(csv_mutex_);
      if (csv_out_.size() > (64u << 20)) return;  // the writer is stuck: drop rather than grow without end
      csv_out_ += csv_buf_;
    }
    csv_buf_.clear();
    csv_cv_.notify_one();
  }
  void CsvThread(std::filesystem::path path) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
      REXLOG_ERROR("gow2_frame_log: cannot write {}; no CSV this session", path.string());
      csv_failed_.store(true, std::memory_order_relaxed);
      return;
    }
    std::string chunk;
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(csv_mutex_);
        csv_cv_.wait(lock, [this] { return !csv_out_.empty(); });
        chunk.swap(csv_out_);
      }
      f.write(chunk.data(), std::streamsize(chunk.size()));
      f.flush();  // a crash or a closed window loses at most the last ~30 frames
      chunk.clear();
    }
  }

  // Guest side (GPU thread), under ring_mutex_.
  std::mutex ring_mutex_;
  std::deque<GuestFrame> pending_;
  uint64_t guest_n_ = 0, guest_last_ = 0;
  uint64_t guest_ticks_[kXenosWaitCount] = {}, guest_count_[kXenosWaitCount] = {};
  // Renderer side (renderer thread only).
  uint64_t render_n_ = 0, render_last_ = 0, t0_ = 0;
  double avg_ = 0.0, prev_ms_ = 0.0;
  uint64_t warm_ = 0;
  Window w_;
  bool csv_ = REXCVAR_GET(gow2_frame_log_csv), csv_started_ = false;
  uint64_t csv_rows_ = 0;
  std::string csv_buf_;
  std::mutex csv_mutex_;
  std::condition_variable csv_cv_;
  std::string csv_out_;
  std::atomic<bool> csv_failed_{false};
};

FrameLog& Log() {
  static FrameLog* log = new FrameLog();  // never destroyed: the CSV thread may still run at exit
  return *log;
}

}  // namespace

bool XenosFrameLogOn() { return REXCVAR_GET(gow2_frame_log); }

void XenosFrameWaitBegin(uint32_t kind, XenosWaitClock& clock) {
  if (kind >= kXenosWaitCount || !REXCVAR_GET(gow2_frame_log)) return;
  const uint64_t now = Qpc();
  // The same wait goes on when this step starts within 200 us of the previous step's end: the whole time since that
  // step began is waiting. A longer gap is a new wait.
  if (clock.end && now >= clock.end && now - clock.end < QpcFreq() / 5000)
    g_wait_ticks[kind].fetch_add(now - clock.begin, std::memory_order_relaxed);
  else
    g_wait_count[kind].fetch_add(1, std::memory_order_relaxed);
  clock.begin = now;
}

void XenosFrameWaitEnd(XenosWaitClock& clock) {
  if (!REXCVAR_GET(gow2_frame_log)) return;
  clock.end = Qpc();
}

void XenosFrameAddWait(uint32_t kind, double ms, uint32_t waits) {
  if (kind >= kXenosWaitCount || !REXCVAR_GET(gow2_frame_log)) return;
  if (ms > 0.0) g_wait_ticks[kind].fetch_add(uint64_t(ms * double(QpcFreq()) / 1000.0), std::memory_order_relaxed);
  if (waits) g_wait_count[kind].fetch_add(waits, std::memory_order_relaxed);
}

void XenosFrameGuestSwap() {
  if (!REXCVAR_GET(gow2_frame_log)) return;
  Log().GuestSwap();
}

void XenosFrameRendered(const XenosRenderedFrame& frame) {
  if (!REXCVAR_GET(gow2_frame_log)) return;
  Log().Rendered(frame);
}
