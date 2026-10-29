// Layer 0 (game-free): occlusion queries (Stage 4, notes/46). Xenos has no per-query counter. The game programs
// RB_SAMPLE_COUNT_ADDR with a 32-byte report in guest memory and sends PM4 EVENT_WRITE_ZPD, and the GPU writes its
// running sample counters (ZFail/ZPass/StencilFail/Total, each in an A and a B lane) into that report. The CPU
// computes a query's result in software as "end report minus begin report".
//
// This header has what our renderer and the developer tools share, and nothing here depends on the SDK:
//   - SampleCounts: one 32-byte report, read from or written to guest memory;
//   - ReportLayout: where the D3D library puts the begin and end reports of one query. This is XDK knowledge, so the
//     app passes it in from layer 1/3 and layer 0 never hard-codes it;
//   - QueryTracker: which draws each query covers, and the emulator's answer. It is fed the exact ZPD events when the
//     SDK has patch 15 (DrawObserver::OnZpd), and otherwise works them out from RB_SAMPLE_COUNT_ADDR at each draw.
//     query_log.cpp uses it today; the renderer will use it in shadow mode;
//   - QueryRecord / Compare: one query with the emulator's answer and ours, and the verdict;
//   - OcclusionAnswerSource: the small interface the renderer implements (begin/end a host query, hand back samples);
//   - AnswerCache (upl-s4c): our renderer's immediate answers and their guarded correction.
//
// Threading: everything runs on the GPU worker thread (the draw observer's thread). Nothing here is thread-safe.
// Our code (MIT). The cumulative-snapshot remarks follow Xenia Canary's xenos_zpd_report.h (BSD, Ben Vanik), but no
// code is copied from it.
#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <unordered_map>
#include <vector>

namespace xenos::renderer::queries {

// Register indices (dword index into the register file), Xenos.
inline constexpr uint32_t kRegVgtEventInitiator = 0x21F9;
inline constexpr uint32_t kRegRbSampleCountCtl = 0x2324;
inline constexpr uint32_t kRegRbSampleCountAddr = 0x2325;
// VGT_EVENT_INITIATOR values (the low 6 bits of the event packet).
inline constexpr uint32_t kEventVizQueryStart = 7;
inline constexpr uint32_t kEventVizQueryEnd = 8;
inline constexpr uint32_t kEventZpassDone = 21;

// D3D marks a report it is waiting on by storing 0xFFFFFEED big-endian into its ZPass lanes. Counter values themselves
// are little-endian in guest memory (the D3D code byte-swaps them). So a host-endian (LE) load of the marker's bytes
// gives the value below.
inline constexpr uint32_t kPendingMarkerLE = 0xEDFEFFFFu;
inline constexpr uint32_t kReportBytes = 32;

// Resolve only ended slots, once per contiguous run, at submission. Resolving
// each query between proxy draws forces the GPU to finish its count before it
// can process the next proxy. Slot allocation can wrap or skip held slots.
template<class Resolve>
void ResolveEndedRuns(std::span<const uint32_t> slots, Resolve&& resolve) {
  size_t i = 0;
  while (i < slots.size()) {
    const uint32_t first = slots[i++];
    uint32_t count = 1;
    while (i < slots.size() && uint64_t(slots[i]) == uint64_t(first) + count) {
      ++i;
      ++count;
    }
    resolve(first, count);
  }
}

// One report as it sits in guest memory (8 little-endian dwords).
struct SampleCounts {
  uint32_t total_a = 0, total_b = 0, zfail_a = 0, zfail_b = 0, zpass_a = 0, zpass_b = 0, sfail_a = 0, sfail_b = 0;

  static SampleCounts Read(const uint8_t* guest) {
    SampleCounts s;
    if (guest) std::memcpy(&s, guest, sizeof(s));  // host is little-endian (x64)
    return s;
  }
  // What the D3D query poll adds up (A + B of ZPass, 32-bit wrap), notes/46 section 2.
  uint32_t ZPass() const { return zpass_a + zpass_b; }
  // The poll treats the report as unanswered only while both ZPass lanes still hold the marker. (The SDK, when it
  // tells end from begin, checks either lane, and ZFail as well.)
  bool Pending() const { return zpass_a == kPendingMarkerLE && zpass_b == kPendingMarkerLE; }
  bool AnyMarker() const {
    return zpass_a == kPendingMarkerLE || zpass_b == kPendingMarkerLE || zfail_a == kPendingMarkerLE ||
           zfail_b == kPendingMarkerLE;
  }
  bool AllZero() const {
    return !(total_a | total_b | zfail_a | zfail_b | zpass_a | zpass_b | sfail_a | sfail_b);
  }

  // Takeover: write an answer the way the SDK's D3D12 path does today (Total_A = ZPass_A = n, the rest 0) or, with
  // split_lanes, spread each count over the A and B lanes (Canary does this for titles that mask the lanes, notes/46).
  // The ZPass lanes are what the poll wakes on, so they are written last.
  static void WriteAnswer(uint8_t* guest, uint64_t zpass, bool split_lanes) {
    if (!guest) return;
    const uint32_t n = zpass > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(zpass);
    const uint32_t a = split_lanes ? n - (n >> 1) : n, b = split_lanes ? (n >> 1) : 0;
    const uint32_t head[4] = {a, b, 0, 0};  // Total_A/B, ZFail_A/B
    std::memcpy(guest, head, sizeof(head));
    const uint32_t tail[4] = {a, b, 0, 0};  // ZPass_A/B, StencilFail_A/B
    std::memcpy(guest + 16, tail, sizeof(tail));
  }
};
static_assert(sizeof(SampleCounts) == kReportBytes, "report is 8 dwords");

// Strict queries (PR #43, legoliamneeson; --gow2_renderer_queries=strict): an end report is answered only with the
// count of the query issued for it - never a result left over from an older query that reused the same report address.
// Issue at the end ZPD (renderer thread), Complete when that query's GPU result lands: false (nothing written) when a
// later issuance replaced this one or the report is no longer pending (the guest moved on).
// Known plausible race, guard still to come: Issue only sees reissues the render thread has processed. If the guest
// re-marks a report pending and its next ZPD is still queued for the render thread when the previous issuance's
// result lands, the report is pending again and the sequence still matches, so that re-issue gets the previous answer.
class DeferredReports {
 public:
  uint64_t Issue(uint32_t address) { return issued_[address] = ++sequence_; }
  bool Complete(uint32_t address, uint64_t sequence, uint8_t* report, uint64_t samples) {
    const auto it = issued_.find(address);
    if (it == issued_.end() || it->second != sequence || !report || !SampleCounts::Read(report).Pending()) return false;
    SampleCounts::WriteAnswer(report, samples, false);
    issued_.erase(it);
    return true;
  }

 private:
  std::unordered_map<uint32_t, uint64_t> issued_;
  uint64_t sequence_ = 0;
};

// Where one query's two reports are relative to each other, from the D3D library (layer 1). For XDK 7645 (Gears 2) the
// end report sits 32 bytes before the begin report (notes/46 section 2: Issue 82978120 / the ZPD writer 82993080 / the
// poll 829783A8). 0 = not known: the tracker then lists report addresses without pairing them.
struct ReportLayout {
  int32_t end_minus_begin = 0;
  bool known() const { return end_minus_begin != 0; }
};

// How the emulator answered an end report (mirrors the SDK's ZpdAnswer, patch 15; kept SDK-free here).
enum class EmuAnswer : uint8_t {
  kUnknown,      // no ZPD callback (inferred mode), or no end ZPD seen
  kNone,         // the SDK wrote nothing
  kHost,         // a real host count
  kFake,         // the SDK's fake count (query_occlusion_fake_sample_count)
  kNoRealCount,  // occlusion_query_no_fake: no real count could be had, 1 written
};
inline const char* EmuAnswerName(EmuAnswer a) {
  switch (a) {
    case EmuAnswer::kUnknown: return "?";
    case EmuAnswer::kNone: return "none";
    case EmuAnswer::kHost: return "host";
    case EmuAnswer::kFake: return "FAKE";
    case EmuAnswer::kNoRealCount: return "no-real";
  }
  return "?";
}

enum class Verdict : uint8_t {
  kSame,               // identical sample counts
  kClose,              // both visible (or both zero) and within the tolerance
  kCountDiffers,       // both visible, counts differ by more than the tolerance
  kVisibilityDiffers,  // one says 0 and the other doesn't: this is what causes popping
  kNoOurs,             // our renderer had no answer (query not run / not ready)
  kNoEmulator,         // the emulator left the report unanswered
};
inline const char* VerdictName(Verdict v) {
  switch (v) {
    case Verdict::kSame: return "same";
    case Verdict::kClose: return "close";
    case Verdict::kCountDiffers: return "count-differs";
    case Verdict::kVisibilityDiffers: return "VISIBILITY-DIFFERS";
    case Verdict::kNoOurs: return "no-ours";
    case Verdict::kNoEmulator: return "no-emulator";
  }
  return "?";
}

// One query in one frame.
struct QueryRecord {
  uint64_t frame = 0;          // swaps before it (same numbering as the other layer-0 tools: frame N = before swap N)
  uint32_t index = 0;          // order within the frame
  uint32_t begin_address = 0;  // guest physical address of the begin report (RB_SAMPLE_COUNT_ADDR during its draws)
  uint32_t end_address = 0;    // derived from ReportLayout (0 if unknown)
  uint32_t first_draw = 0, last_draw = 0, draws = 0;  // non-copy draws issued while the begin report was programmed
  bool end_seen = false;       // a draw ran with the end report programmed (draws between end and the next begin)
  bool answered = false;       // the end report was read after the emulator processed it
  bool end_marker_at_open = false;  // the end report already held D3D's pending marker at the query's first draw
  // Exact ZPD events (patch 15).
  bool begin_zpd = false, end_zpd = false;  // seen as ZPD callbacks
  bool unended = false;        // a new begin came before this query's end
  EmuAnswer answer = EmuAnswer::kUnknown;
  uint64_t sdk_samples = 0;    // what the SDK says it wrote
  uint32_t host_segments = 0;  // host queries the SDK summed (> 1: split by submissions, occlusion_query_no_fake)
  SampleCounts begin, end;     // as read at finalisation
  // Our renderer (shadow mode).
  bool has_ours = false;
  uint64_t our_samples = 0;

  // The answer the game will compute: end minus begin, both A+B of ZPass, 32-bit wrap.
  uint32_t EmulatorSamples() const { return end.ZPass() - begin.ZPass(); }
  bool EmulatorPending() const { return end.Pending(); }
};

// tolerance_pct: allowed relative difference when both sides are visible (MSAA / resolution scale / rasterisation-rule
// differences make exact equality unlikely; visibility is what the game uses).
inline Verdict Compare(const QueryRecord& q, uint32_t tolerance_pct = 5) {
  if (!q.answered || q.EmulatorPending() || q.answer == EmuAnswer::kFake || q.answer == EmuAnswer::kNone ||
      q.answer == EmuAnswer::kNoRealCount) {
    return Verdict::kNoEmulator;  // no real reference count (job 113: the stock SDK answers almost every query fake)
  }
  if (!q.has_ours) return Verdict::kNoOurs;
  const uint64_t emu = q.EmulatorSamples(), ours = q.our_samples;
  if (emu == ours) return Verdict::kSame;
  if ((emu == 0) != (ours == 0)) return Verdict::kVisibilityDiffers;
  const uint64_t hi = emu > ours ? emu : ours, diff = emu > ours ? emu - ours : ours - emu;
  return diff * 100 <= hi * tolerance_pct ? Verdict::kClose : Verdict::kCountDiffers;
}

// Implemented by our renderer (shadow mode now, takeover later). Calls arrive on the GPU worker thread in stream order,
// driven by QueryTracker::Events.
class OcclusionAnswerSource {
 public:
  virtual ~OcclusionAnswerSource() = default;
  // A query opened: the following draws (until EndQuery) count towards it. Shadow: BeginQuery on a D3D12 occlusion heap
  // slot around our copies of those draws.
  virtual void BeginQuery(uint32_t begin_address) = 0;
  // The query closed (seen at the next query's first draw or at the swap). Shadow: EndQuery + ResolveQueryData.
  virtual void EndQuery(uint32_t begin_address) = 0;
  // Our result once our GPU has it (after our frame fence). False = not ready / not run.
  virtual bool TryGetSamples(uint32_t begin_address, uint64_t* samples) = 0;
};

// How queries are answered (for --gow2_renderer_queries later): off; shadow = the emulator answers, we compare; takeover =
// we answer (the emulator no longer draws). Parsed by the renderer; listed here so the tools and the renderer agree.
enum class QueryMode : uint8_t { kOff, kShadow, kTakeover };

// Follows RB_SAMPLE_COUNT_ADDR across draws. The SDK's draw observer doesn't see EVENT_WRITE_ZPD packets, only the
// register state at each draw. That is enough for this D3D library:
//   - during a query's draws the register holds the begin report;
//   - the end ZPD and the next begin ZPD both happen between draws, so a change of address at a draw means the open
//     query has been closed. The SDK writes the end report synchronously when it executes the end ZPD, so its contents
//     are final by then;
//   - a query with no draws inside, or one whose end has no draw after it, is still seen through the next address change.
// What it cannot see: a begin/end pair with no draw in between and no address change (two empty queries on the same
// report). It can also be misled: if a query's begin and end both fall between two draws, the first address seen is
// the end report and is taken for a begin (a phantom query with 0 draws). query_log logs the report alignment and
// the 0-draw count so a run shows how often that happens. An SDK hook on EVENT_WRITE_ZPD (proposed in notes/46) would remove the guesswork; this class takes either.
class QueryTracker {
 public:
  using ReadFn = const uint8_t* (*)(void* user, uint32_t physical_address);  // nullptr = not mapped

  struct Events {
    bool opened = false;  // a new query starts with this draw (begin address = OpenBegin())
    bool closed = false;  // a query was finalised before this draw (it is records().back())
  };

  QueryTracker(ReportLayout layout, ReadFn read, void* user) : layout_(layout), read_(read), user_(user) {}

  // Exact mode (SDK patch 15): call UseExactZpds() before the first draw when ZPD callbacks will come (otherwise draws
  // before the first ZPD are inferred into phantom queries), then OnZpd for every ZPD, after the SDK wrote the report.
  // OnDraw then only counts draws. draw_index = the number of draws before this event.
  void UseExactZpds() { exact_ = true; }
  Events OnZpd(uint32_t address, bool is_end, EmuAnswer answer, uint64_t samples, uint32_t segments,
               uint32_t draw_index) {
    Events ev;
    exact_ = true;
    last_addr_ = address;
    if (!is_end) {
      ++zpd_begins_;
      if (open_) {
        cur_.unended = true;
        ev.closed = Close();
      }
      Open(address, draw_index);
      cur_.begin_zpd = true;
      cur_.draws = 0;
      ev.opened = true;
      return ev;
    }
    ++zpd_ends_;
    if (open_ && (!layout_.known() || address == cur_.end_address)) {
      cur_.end_address = address;
      cur_.end_zpd = true;
      cur_.answer = answer;
      cur_.sdk_samples = samples;
      cur_.host_segments = segments;
      ev.closed = Close();
    } else {
      ++unmatched_ends_;
    }
    return ev;
  }

  // Call at every draw (copies included: is_copy draws don't count as inside a query). Returns what happened.
  Events OnDraw(uint32_t sample_count_addr, bool is_copy, uint32_t draw_index) {
    Events ev;
    if (exact_) {
      if (open_ && !is_copy) {
        if (!cur_.draws) cur_.first_draw = draw_index;
        cur_.last_draw = draw_index;
        ++cur_.draws;
      }
      return ev;
    }
    if (sample_count_addr != last_addr_) {
      last_addr_ = sample_count_addr;
      ++address_changes_;
      if (sample_count_addr != 0) {
        if (open_ && layout_.known() && sample_count_addr == uint32_t(int64_t(open_begin_) + layout_.end_minus_begin)) {
          // The open query's end report is programmed and a draw follows it (not inside any query).
          cur_.end_seen = true;
          ev.closed = Close();
        } else {
          if (open_) ev.closed = Close();
          Open(sample_count_addr, draw_index);
          ev.opened = true;
        }
      }
    }
    if (open_ && !is_copy && sample_count_addr == open_begin_) {
      if (!cur_.draws) cur_.first_draw = draw_index;
      cur_.last_draw = draw_index;
      ++cur_.draws;
    }
    return ev;
  }

  // At the swap: closes the open query (its end has been processed by now if the game ended it this frame) and hands
  // back this frame's records. The caller may fill has_ours/our_samples before calling NextFrame().
  std::vector<QueryRecord>& EndFrame() {
    // Inferred mode: the open query's end has been processed by now. Exact mode: a query still open at the swap is
    // carried into the next frame (its end ZPD comes later).
    if (open_ && !exact_) Close();
    return records_;
  }
  void NextFrame() {
    records_.clear();
    ++frame_;
    // last_addr_ is kept on purpose: the register keeps its value across the swap, and forgetting it would open a
    // phantom query on the stale address at the next frame's first draw. Cost: a next-frame first query on exactly
    // the same report as the last one is missed (not expected: reports differ per query object).
  }

  bool open() const { return open_; }
  bool exact() const { return exact_; }
  uint64_t zpd_begins() const { return zpd_begins_; }
  uint64_t zpd_ends() const { return zpd_ends_; }
  uint64_t unmatched_ends() const { return unmatched_ends_; }
  uint32_t OpenBegin() const { return open_ ? open_begin_ : 0; }
  uint64_t frame() const { return frame_; }
  uint64_t address_changes() const { return address_changes_; }
  const std::vector<QueryRecord>& records() const { return records_; }

 private:
  void Open(uint32_t addr, uint32_t draw_index) {
    open_ = true;
    open_begin_ = addr;
    cur_ = QueryRecord{};
    cur_.frame = frame_;
    cur_.index = uint32_t(records_.size());
    cur_.begin_address = addr;
    cur_.first_draw = cur_.last_draw = draw_index;
    cur_.end_address = layout_.known() ? uint32_t(int64_t(addr) + layout_.end_minus_begin) : 0;
    // D3D writes the marker on the CPU at Issue(END) when the report is idle, or with a GPU MEM_WRITE (after our
    // draw in the stream) when it is still in flight; either way it is a hint about where the CPU is, not an answer.
    if (cur_.end_address && read_) cur_.end_marker_at_open = SampleCounts::Read(read_(user_, cur_.end_address)).AnyMarker();
  }
  bool Close() {
    open_ = false;
    cur_.begin = SampleCounts::Read(read_ ? read_(user_, cur_.begin_address) : nullptr);
    if (cur_.end_address) {
      const uint8_t* p = read_ ? read_(user_, cur_.end_address) : nullptr;
      cur_.end = SampleCounts::Read(p);
      cur_.answered = p != nullptr;
    }
    records_.push_back(cur_);
    return true;
  }

  ReportLayout layout_;
  ReadFn read_;
  void* user_;
  bool open_ = false, exact_ = false;
  uint32_t open_begin_ = 0, last_addr_ = 0;
  uint64_t zpd_begins_ = 0, zpd_ends_ = 0, unmatched_ends_ = 0;  // cumulative
  uint64_t frame_ = 0, address_changes_ = 0;
  QueryRecord cur_;
  std::vector<QueryRecord> records_;
};

// Our renderer's answers (upl-s4c, notes/46 section 4). Never blocks:
// - the immediate answer for an end report is the last real result we had for that report address (D3D query objects
//   keep their report block), or `fallback` (1, visible) when there is none (Canary "fast", notes/47);
// - when our count lands, the report is corrected if it still holds exactly what we wrote for that same ZPD (the
//   game hasn't re-issued the query), and the count becomes the next estimate for that address.
// Counts are host samples scaled to guest samples (divide by the resolution scale area; our renderer draws at guest
// resolution and sample count, so the scale is 1 today).
class AnswerCache {
 public:
  struct Written {
    uint64_t value = 0;
    uint64_t sequence = 0;  // which end ZPD wrote it
  };
  // Job 139 (Shagg saw distant surfaces flicker; gone with no answers written): a wrong "hidden" is a missing object on
  // screen, a wrong "visible" only costs a draw. So "hidden" (0) is answered only after `hidden_after` real zero
  // counts in a row for the same query, and the estimate is keyed by the report address AND the number of draws in
  // the query (a cheap identity: a query object reused for another primitive does not inherit its zeros).
  void SetHiddenAfter(uint32_t n) { hidden_after_ = n ? n : 1; }
  static uint64_t Key(uint32_t end_address, uint32_t draws) { return (uint64_t(draws) << 32) | end_address; }
  // The immediate answer for an end ZPD, never below `minimum` (0 keeps real zeros).
  uint64_t Estimate(uint64_t key, uint64_t fallback, uint64_t minimum, bool* from_cache = nullptr) const {
    const auto it = last_.find(key);
    const bool hit = it != last_.end();
    if (from_cache) *from_cache = hit;
    uint64_t v = hit ? it->second.samples : fallback;
    if (hit && v == 0 && it->second.zero_streak < hidden_after_) v = 1;  // not trusted yet: visible
    return v < minimum ? minimum : v;
  }
  void Learn(uint64_t key, uint64_t samples) {
    Entry& e = last_[key];
    e.samples = samples;
    e.zero_streak = samples ? 0 : e.zero_streak + 1;
  }
  // A zero count may be written into the report (correction) only once it is trusted.
  bool TrustZero(uint64_t key) const {
    const auto it = last_.find(key);
    return it != last_.end() && it->second.samples == 0 && it->second.zero_streak >= hidden_after_;
  }
  // Bookkeeping of what we wrote, for the guarded correction.
  uint64_t NoteWritten(uint32_t end_address, uint64_t value) {
    Written& w = written_[end_address];
    w.value = value;
    w.sequence = ++sequence_;
    return w.sequence;
  }
  // True when the report at end_address still holds our write `sequence` (no later ZPD wrote it, and the guest bytes
  // still equal what we wrote: D3D hasn't put its marker back for a new Issue).
  bool StillOurs(uint32_t end_address, uint64_t sequence, const SampleCounts& now) const {
    const auto it = written_.find(end_address);
    if (it == written_.end() || it->second.sequence != sequence) return false;
    const uint32_t v = it->second.value > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(it->second.value);
    return now.zpass_a == v && now.zpass_b == 0 && now.total_a == v && !now.AnyMarker();
  }
  static uint64_t ToGuestSamples(uint64_t host_samples, uint32_t scale_area) {
    if (scale_area <= 1 || !host_samples) return host_samples;
    const uint64_t v = (host_samples + (scale_area >> 1)) / scale_area;
    return v ? v : 1;  // something passed: never round a visible query down to 0
  }
  size_t size() const { return last_.size(); }

 private:
  struct Entry {
    uint64_t samples = 0;
    uint32_t zero_streak = 0;
  };
  std::unordered_map<uint64_t, Entry> last_;
  std::unordered_map<uint32_t, Written> written_;
  uint32_t hidden_after_ = 1;
  uint64_t sequence_ = 0;
};

}  // namespace xenos::renderer::queries
