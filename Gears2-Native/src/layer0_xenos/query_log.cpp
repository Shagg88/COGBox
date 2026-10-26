// Layer 0 (game-free): occlusion-query log (Stage 4 reference answers, notes/46). --gow2_query_log=N logs every Nth
// frame in full (frame N = the work before the N-th swap, the same numbering as --gow2_draw_stats / --gow2_resolve_log):
//   one line per query: its begin and end report addresses (guest physical, the ZPD targets), the draws it covered,
//   whether the end report already held D3D's pending marker at the query's first draw, the emulator's final lanes of
//   the end report (ZPass/Total/ZFail/StencilFail A/B), the begin report and the emulator's answer as the game will compute it (end ZPass A+B minus begin ZPass A+B);
//   a summary line: query count, answers that are 0 / >0 / still pending / equal to the SDK's fake count, 0-draw queries,
//   reports whose begin isn't zero, report alignment, VIZ query events and draws right after a ZPD.
// Every N frames it also logs the totals over those N frames. With N = 1 every frame is logged in full, so
// --gow2_query_log_max_lines caps the per-query lines in each frame.
//
// How it sees the queries: with SDK patch 15 every EVENT_WRITE_ZPD arrives as DrawObserver::OnZpd (address, begin or
// end, and how the SDK answered: host / FAKE / no-real / none), and the per-query line shows that answer kind and the
// host segment count. This file needs the patch (it overrides OnZpd). The tracker's inferred mode (following
// RB_SAMPLE_COUNT_ADDR at each draw, upl-s4a) stays in queries.h for users without the callback. The observer hub must
// forward OnZpd (EDITS.md). The summary line says which mode ran. Which report is begin and which is end
// belongs to the D3D library, so the app passes it in (XenosQueryLogHints; Gears 2 / XDK 7645: end = begin - 32).
// These are the emulator's answers, kept as the reference for our renderer's own counts in shadow mode.
// Our code (MIT).
#include <xenos/dev_tools.h>
#include <xenos/renderer/queries.h>

#include <rex/cvar.h>
#include <rex/graphics/draw_observer.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <cstdint>
#include <string>

REXCVAR_DEFINE_INT32(gow2_query_log, 0, "Gears2",
                     "Log every occlusion query (report addresses, draws, the emulator's answer) of every Nth frame, "
                     "and totals every N frames (0 = off)");
REXCVAR_DEFINE_INT32(gow2_query_log_max_lines, 400, "Gears2", "Most per-query lines logged in one frame");
REXCVAR_DEFINE_INT32(gow2_query_log_fake, 1000, "Gears2",
                     "The SDK's fake sample count (query_occlusion_fake_sample_count); answers equal to it are counted "
                     "as 'fake?'");

namespace {

using namespace rex::graphics;
namespace q = ::xenos::renderer::queries;

struct Totals {
  uint64_t frames = 0, queries = 0, zero = 0, visible = 0, pending = 0, fake = 0, unanswered = 0, no_draws = 0,
           begin_nonzero = 0, samples = 0;
  // By the SDK's own answer kind (patch 15): host / fake / no-real / none, ends split over several host queries,
  // queries a new begin cut short.
  uint64_t host = 0, sdk_fake = 0, no_real = 0, none = 0, split = 0, unended = 0;
  uint32_t max_per_frame = 0;
};

class QueryLog final : public DrawObserver {
 public:
  QueryLog(rex::memory::Memory* memory, q::ReportLayout layout, int every)
      : memory_(memory), tracker_(layout, &Read, this), every_(every) {
    tracker_.UseExactZpds();  // this file overrides DrawObserver::OnZpd, so it only builds with SDK patch 15
  }

  void OnDraw(const RegisterFile& regs, rex::graphics::xenos::PrimitiveType, uint32_t, const ObservedIndexBuffer*, bool) override {
    const bool copy = regs.Get<reg::RB_MODECONTROL>().edram_mode == rex::graphics::xenos::EdramMode::kCopy;
    const uint32_t ev = regs[q::kRegVgtEventInitiator] & 0x3F;
    if (ev != last_event_) {
      if (ev == q::kEventVizQueryStart || ev == q::kEventVizQueryEnd) ++viz_events_;
      last_event_ = ev;
    }
    if (ev == q::kEventZpassDone) ++draws_after_zpd_;
    const auto events = tracker_.OnDraw(regs[q::kRegRbSampleCountAddr], copy, draws_);
    if (events.opened) {
      const uint32_t mod = tracker_.OpenBegin() & 63;
      ++(mod == 32 ? align32_ : mod == 0 ? align0_ : align_other_);
    }
    if (!copy) ++draws_;
  }

  void OnZpd(const RegisterFile&, const ObservedZpd& zpd) override {
    q::EmuAnswer a = q::EmuAnswer::kUnknown;
    switch (zpd.answer) {
      case ZpdAnswer::kNone: a = q::EmuAnswer::kNone; break;
      case ZpdAnswer::kBegin: break;
      case ZpdAnswer::kHost: a = q::EmuAnswer::kHost; break;
      case ZpdAnswer::kFake: a = q::EmuAnswer::kFake; break;
      case ZpdAnswer::kNoRealCount: a = q::EmuAnswer::kNoRealCount; break;
    }
    tracker_.OnZpd(zpd.report_address, zpd.is_end, a, zpd.samples, zpd.host_segments, draws_);
  }

  void OnSwap(const RegisterFile&, uint32_t, uint32_t, uint32_t) override {
    auto& records = tracker_.EndFrame();
    const uint64_t frame = tracker_.frame() + 1;  // 1-based like the other tools
    const bool full = frame % uint64_t(every_) == 0;
    Totals f;
    f.frames = 1;
    f.queries = records.size();
    const uint32_t fake = uint32_t(std::max(0, REXCVAR_GET(gow2_query_log_fake)));
    const int max_lines = REXCVAR_GET(gow2_query_log_max_lines);
    int lines = 0;
    for (const q::QueryRecord& r : records) {
      const uint32_t emu = r.EmulatorSamples();
      if (!r.answered) ++f.unanswered;
      else if (r.EmulatorPending()) ++f.pending;
      else if (emu == 0) ++f.zero;
      else ++f.visible;
      if (r.answered && !r.EmulatorPending()) {
        f.samples += emu;
        if (fake && emu == fake) ++f.fake;
      }
      if (!r.draws) ++f.no_draws;
      if (r.begin.ZPass()) ++f.begin_nonzero;
      if (r.end_marker_at_open) ++marker_at_open_;
      switch (r.answer) {
        case q::EmuAnswer::kHost: ++f.host; break;
        case q::EmuAnswer::kFake: ++f.sdk_fake; break;
        case q::EmuAnswer::kNoRealCount: ++f.no_real; break;
        case q::EmuAnswer::kNone: ++f.none; break;
        case q::EmuAnswer::kUnknown: break;
      }
      if (r.host_segments > 1) ++f.split;
      if (r.unended) ++f.unended;
      if (full && lines < max_lines) {
        ++lines;
        REXLOG_INFO("gow2_query_log: frame {} q{} begin {:08X} end {:08X}{} draws {} [{}..{}] | emu {}{} | end zpass {:08X}/{:08X} "
                    "total {:08X}/{:08X} zfail {:08X}/{:08X} sfail {:08X}/{:08X} | begin zpass {:08X}/{:08X} total {:08X} | "
                    "marker at first draw {} | sdk {} {} seg {}{}",
                    frame, r.index, r.begin_address, r.end_address, r.end_seen ? " (seen)" : "", r.draws, r.first_draw,
                    r.last_draw, r.answered ? (r.EmulatorPending() ? "PENDING" : std::to_string(emu)) : "unread",
                    (fake && emu == fake) ? " fake?" : "", r.end.zpass_a, r.end.zpass_b, r.end.total_a, r.end.total_b,
                    r.end.zfail_a, r.end.zfail_b, r.end.sfail_a, r.end.sfail_b, r.begin.zpass_a, r.begin.zpass_b,
                    r.begin.total_a, r.end_marker_at_open ? "yes" : "no", q::EmuAnswerName(r.answer), r.sdk_samples,
                    r.host_segments, r.unended ? " UNENDED" : "");
      }
    }
    f.max_per_frame = uint32_t(f.queries);
    if (full) {
      REXLOG_INFO("gow2_query_log: frame {} [{}] draws {} queries {} ({} lines) | answers: 0 -> {}, >0 -> {}, pending {}, unread {}, "
                  "fake? {} | sdk: host {}, FAKE {}, no-real {}, none {}, split {}, unended {} | ZPDs so far: begin {} end {} "
                  "unmatched end {} | samples {} | 0-draw queries {}, begin reports not 0: {} | report addr mod 64: 32 -> {}, "
                  "0 -> {}, other {} | end marker already set at first draw {} | viz events {}, draws after a ZPD {}",
                  frame, tracker_.exact() ? "exact ZPDs" : "inferred", draws_, f.queries, lines, f.zero, f.visible,
                  f.pending, f.unanswered, f.fake, f.host, f.sdk_fake, f.no_real, f.none, f.split, f.unended,
                  tracker_.zpd_begins(), tracker_.zpd_ends(), tracker_.unmatched_ends(), f.samples, f.no_draws,
                  f.begin_nonzero, align32_, align0_, align_other_, marker_at_open_, viz_events_, draws_after_zpd_);
    }
    Add(f);
    if (total_.frames >= uint64_t(every_)) {
      REXLOG_INFO("gow2_query_log: frames {}-{} queries {} (max {}/frame) | 0 -> {}, >0 -> {}, pending {}, unread {}, fake? {} | "
                  "sdk: host {}, FAKE {}, no-real {}, none {}, split {}, unended {} | "
                  "0-draw {}, begin not 0 {} | mean samples per visible query {:.1f}",
                  frame + 1 - total_.frames, frame, total_.queries, total_.max_per_frame, total_.zero, total_.visible,
                  total_.pending, total_.unanswered, total_.fake, total_.host, total_.sdk_fake, total_.no_real,
                  total_.none, total_.split, total_.unended, total_.no_draws, total_.begin_nonzero,
                  total_.visible ? double(total_.samples) / double(total_.visible) : 0.0);
      total_ = {};
    }
    tracker_.NextFrame();
    draws_ = 0;
    viz_events_ = draws_after_zpd_ = align32_ = align0_ = align_other_ = marker_at_open_ = 0;
  }

 private:
  static const uint8_t* Read(void* user, uint32_t physical) {
    auto* self = static_cast<QueryLog*>(user);
    if (!physical || !self->memory_) return nullptr;
    return self->memory_->TranslatePhysical<const uint8_t*>(physical);
  }
  void Add(const Totals& f) {
    total_.frames += f.frames; total_.queries += f.queries; total_.zero += f.zero; total_.visible += f.visible;
    total_.pending += f.pending; total_.fake += f.fake; total_.unanswered += f.unanswered; total_.no_draws += f.no_draws;
    total_.begin_nonzero += f.begin_nonzero; total_.samples += f.samples;
    total_.host += f.host; total_.sdk_fake += f.sdk_fake; total_.no_real += f.no_real; total_.none += f.none;
    total_.split += f.split; total_.unended += f.unended;
    total_.max_per_frame = std::max(total_.max_per_frame, f.max_per_frame);
  }

  rex::memory::Memory* memory_;
  q::QueryTracker tracker_;
  int every_;
  uint32_t draws_ = 0, last_event_ = 0;  // GPU worker thread only
  uint64_t viz_events_ = 0, draws_after_zpd_ = 0, align32_ = 0, align0_ = 0, align_other_ = 0, marker_at_open_ = 0;
  Totals total_;
};

}  // namespace

void XenosInstallQueryLogIfRequested(rex::Runtime* runtime, const XenosQueryLogHints& hints) {
  const int every = REXCVAR_GET(gow2_query_log);
  if (every <= 0) return;
  rex::memory::Memory* memory = runtime ? runtime->memory() : nullptr;
  if (!memory) {
    REXLOG_ERROR("gow2_query_log: no guest memory; staying off");
    return;
  }
  // Never destroyed (the hub keeps the pointer until the process exits).
  auto* log = new QueryLog(memory, q::ReportLayout{hints.end_minus_begin}, every);
  XenosRegisterDrawObserver(log);
  REXLOG_INFO("gow2_query_log: every {} frames; end report = begin {:+d} bytes{}", every, hints.end_minus_begin,
              hints.end_minus_begin ? "" : " (layout unknown: report addresses only, no answers)");
}
