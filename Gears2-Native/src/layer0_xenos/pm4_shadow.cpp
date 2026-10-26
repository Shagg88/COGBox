// Layer 0 (game-free) tool, Stage 5B phase 1 (notes/49): our own PM4 processor (xenos/pm4.h) runs on every primary
// ring range just before the SDK executes it (SDK patch 20), and at every draw the SDK reports we check that our
// processor reached the same draw with the same GPU state: registers 0x2000-0x5002 (context, control, ALU/fetch/bool/
// loop constants), the draw initiator and the active shaders' ucode. Behind --gow2_pm4_shadow=N (summary every N
// frames, 0 = off). Our code (MIT).
#include <xenos/dev_tools.h>
#include <xenos/display_gamma.h>
#include <xenos/gpu_system.h>
#include <xenos/pm4.h>

#include <rex/hash.h>  // XXH3_64bits, as the SDK hashes shader ucode

#include <rex/cvar.h>
#include <rex/graphics/draw_observer.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>
#include <immintrin.h>

#ifdef _WIN32
#include <windows.h>
#pragma comment(lib, "Synchronization.lib")  // job 225: WaitOnAddress / WakeByAddressAll
#endif
#include <memory>
#include <string>
#include <vector>

REXCVAR_DEFINE_INT32(gow2_pm4_shadow, 0, "Gears2",
                     "Stage 5B: run our own PM4 processor next to the SDK's and compare the GPU state at every draw; "
                     "summary every N frames (0 = off). Needs SDK patch 20");
REXCVAR_DEFINE_INT32(gow2_pm4_shadow_examples, 12, "Gears2", "Most mismatching registers listed per summary");
REXCVAR_DEFINE_BOOL(gow2_pm4_execute, false, "Gears2",
                    "Stage 5B phase 2: OUR PM4 processor runs the ring (SDK patch 21 --gpu_external_pm4); the SDK only "
                    "runs the single packets it is handed (draw notifications, shader objects, query reports, present, "
                    "interrupts). Stats every --gow2_pm4_shadow frames (300 if that is 0)");
REXCVAR_DEFINE_BOOL(gow2_pm4_own_zpd, false, "Gears2",
                    "Stage 5B: with --gow2_pm4_execute, occlusion-query reports (EVENT_WRITE_ZPD) are ours too: we write "
                    "the report as the SDK's fallback path does (query_occlusion_fake_sample_count) and tell the "
                    "observers (our renderer then answers it). Not with occlusion_query_no_fake.");
REXCVAR_DEFINE_BOOL(gow2_pm4_fast_constants, true, "Gears2",
                    "Our PM4 processor stores constant-file register writes (0x4000-0x4927) straight into the host "
                    "register file (job 168)");
REXCVAR_DEFINE_BOOL(gow2_pm4_split, true, "Gears2",
                    "Job 173 (notes/52 section 15): our PM4 processor and our renderer on separate threads - the GPU "
                    "thread parses the ring and queues draws (with their register changes), swaps, query reports and "
                    "fence writes; a renderer thread runs them in order. The title sees a fence / the read pointer only "
                    "once the renderer got there; interrupts, scratch write-backs and failing waits wait for it");
REXCVAR_DEFINE_BOOL(gow2_pm4_own_draws, false, "Gears2",
                    "Stage 5B phase 3 (with --gow2_pm4_execute): draws and shader loads are ours too - the observers get "
                    "our ucode (XenosUcodeSink) and the SDK's Shader objects and draw path are not used. Needs SDK patch "
                    "22");
REXCVAR_DEFINE_INT32(gow2_pm4_swap_ahead, 0, "Gears2",
                     "Patch 0148 (multi-core step 1): with --gow2_pm4_split, how many swaps the GPU thread may queue "
                     "past the one the renderer thread is on (0 = off: the GPU thread waits at every swap until the "
                     "renderer has presented, as before; 1 or 2). The title still sees each swap complete only once our "
                     "renderer presented it; the GPU thread just goes on parsing the next frame meanwhile. Our GPU "
                     "system only");

namespace {

using namespace rex::graphics;
namespace pm = ::xenos::pm4;

constexpr uint32_t kFirst = 0x2000, kEnd = pm::kRegisterCount;

// Stage 5C: the gamma ramps, fed by Pm4Execute's register writes (GPU thread only).
::xenos::display_gamma::Ramps& GammaState() {
  static ::xenos::display_gamma::Ramps ramps;
  return ramps;
}
bool g_gamma_tracked = false;
// Patch 0148: with --gow2_pm4_swap_ahead the GPU thread may take the next frame's gamma writes while the renderer
// thread presents, so the renderer presents with the ramps captured at the swap (renderer thread only).
::xenos::display_gamma::Ramps g_gamma_at_swap;
std::atomic<bool> g_gamma_from_swap{false};

std::string RegName(uint32_t reg) {
  struct Entry {
    uint32_t index;
    const char* name;
  };
  static constexpr Entry kTable[] = {
#define XE_GPU_REGISTER(index, type, name) {index, #name},
#include <rex/graphics/register_table.inc>
#undef XE_GPU_REGISTER
  };
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04X", reg);
  for (const Entry& e : kTable)
    if (e.index == reg) return std::string(e.name) + " (" + buf + ")";
  return std::string("reg ") + buf;
}

uint64_t HashDwords(const uint32_t* p, size_t n) {  // FNV-1a over the dwords (host order)
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h ^ n;
}

struct OurDraw {
  std::vector<std::pair<uint32_t, uint32_t>> changes;  // registers changed since our previous draw
  uint32_t initiator = 0;
  uint64_t vs = 0, ps = 0;  // ucode hashes (0 = none)
};

class Pm4Shadow final : public DrawObserver {
 public:
  Pm4Shadow(rex::memory::Memory* memory, int every)
      : every_(every),
        proc_(pm::Environment{[memory](uint32_t a) -> uint8_t* { return memory->TranslatePhysical<uint8_t*>(a); },
                              false},
              pm::Callbacks{[this](const std::array<uint32_t, pm::kRegisterCount>&, const pm::Draw& d,
                                   const pm::ShaderRef& vs, const pm::ShaderRef& ps) {
                              if (!d.executed) {  // the SDK drops these before its observer sees them
                                ++dropped_;
                                return;
                              }
                              OurDraw od;
                              od.changes = proc_.TakeChanges();
                              od.initiator = d.initiator;
                              od.vs = vs.dwords.empty() ? 0 : HashDwords(vs.dwords.data(), vs.dwords.size());
                              od.ps = ps.dwords.empty() ? 0 : HashDwords(ps.dwords.data(), ps.dwords.size());
                              queue_.push_back(std::move(od));
                            },
                            nullptr, nullptr}) {}

  void OnPrimaryRange(uint32_t ring_physical, uint32_t ring_dwords, uint32_t read_index,
                      uint32_t write_index) override {
    proc_.ExecutePrimary(ring_physical, ring_dwords, read_index, write_index);
  }

  void OnDrawShaders(const Shader* vs, const Shader* ps) override {
    sdk_vs_ = vs ? HashDwords(vs->ucode_dwords(), vs->ucode_dword_count()) : 0;
    sdk_ps_ = ps ? HashDwords(ps->ucode_dwords(), ps->ucode_dword_count()) : 0;
  }

  void OnDraw(const RegisterFile& regs, rex::graphics::xenos::PrimitiveType, uint32_t, const ObservedIndexBuffer*,
              bool) override {
    ++sdk_draws_;
    if (queue_.empty()) {  // the SDK drew something our processor did not reach
      ++missing_;
      return;
    }
    OurDraw od = std::move(queue_.front());
    queue_.pop_front();
    for (const auto& [i, v] : od.changes) ours_[i] = v;
    if (!synced_) {
      // First draw: differences here are state set before our first ring range (or by the CPU) - logged once, then
      // our copy starts from the SDK's.
      uint32_t initial = 0;
      std::string first;
      for (uint32_t i = kFirst; i < kEnd; ++i)
        if (ours_[i] != regs.values[i]) {
          if (initial < 6) first += " " + RegName(i);
          ++initial;
        }
      REXLOG_INFO("gow2_pm4_shadow: first draw: {} registers differ from the SDK's (state from before our first ring "
                  "range){}{} - starting from the SDK's values",
                  initial, initial ? ":" : "", first);
      std::memcpy(&ours_[kFirst], &regs.values[kFirst], sizeof(uint32_t) * (kEnd - kFirst));
      synced_ = true;
      return;
    }
    bool equal = true;
    if (std::memcmp(&ours_[kFirst], &regs.values[kFirst], sizeof(uint32_t) * (kEnd - kFirst)) != 0) {
      equal = false;
      for (uint32_t i = kFirst; i < kEnd; ++i) {
        if (ours_[i] == regs.values[i]) continue;
        ++reg_mismatch_values_;
        if (examples_.size() < size_t(std::max(1, REXCVAR_GET(gow2_pm4_shadow_examples))) &&
            std::none_of(examples_.begin(), examples_.end(), [&](const auto& e) { return e.first == i; }))
          examples_.emplace_back(i, std::make_pair(ours_[i], regs.values[i]));
        ours_[i] = regs.values[i];  // resync so one fault is counted once, not at every later draw
      }
    }
    if (od.initiator != regs.values[0x21FC]) {
      equal = false;
      ++initiator_mismatch_;
    }
    if (od.vs != sdk_vs_ || od.ps != sdk_ps_) {
      equal = false;
      ++shader_mismatch_;
    }
    ++compared_;
    if (equal) ++matched_;
  }

  void OnSwap(const RegisterFile&, uint32_t, uint32_t, uint32_t) override {
    ++frames_;
    if (frames_ % uint64_t(every_)) return;
    const pm::Stats& s = proc_.GetStats();
    REXLOG_INFO("gow2_pm4_shadow: frames 1-{}: SDK draws {} - compared {}, IDENTICAL {} ({:.4f}%) | state mismatches "
                "{} register values, initiator {}, shaders {} | SDK draws ours never reached {}, our draws still "
                "queued {}, dropped as the SDK does {} | our processor: packets {}, type3 {}, indirect buffers {}, shader loads {}, swaps {}, "
                "predicated skipped {}, waits left to the SDK {}, memory writes left to the SDK {}, unknown opcodes "
                "{} (last 0x{:02X}), errors {}",
                frames_, sdk_draws_, compared_, matched_, compared_ ? 100.0 * double(matched_) / double(compared_) : 0.0,
                reg_mismatch_values_, initiator_mismatch_, shader_mismatch_, missing_, queue_.size(), dropped_, s.packets,
                s.type3, s.indirect, s.shader_loads, s.swaps, s.predicated_skipped, s.waits, s.memory_writes_seen,
                s.unknown_opcodes, s.last_unknown_opcode, s.errors);
    for (const auto& [reg, v] : examples_)
      REXLOG_INFO("gow2_pm4_shadow:   mismatch {}: ours {:08X} SDK {:08X}", RegName(reg), v.first, v.second);
    examples_.clear();
  }

 private:
  int every_;
  pm::Processor proc_;
  std::deque<OurDraw> queue_;
  std::array<uint32_t, pm::kRegisterCount> ours_{};
  bool synced_ = false;
  uint64_t sdk_vs_ = 0, sdk_ps_ = 0;
  uint64_t frames_ = 0, sdk_draws_ = 0, compared_ = 0, matched_ = 0, missing_ = 0, reg_mismatch_values_ = 0,
           initiator_mismatch_ = 0, shader_mismatch_ = 0, dropped_ = 0;
  std::vector<std::pair<uint32_t, std::pair<uint32_t, uint32_t>>> examples_;
};

// ---- Job 173: the PM4 / renderer split (notes/52 section 15) ----
// The GPU thread (producer) parses the ring and appends items to blocks; a renderer thread (consumer) runs them in
// order with its own copy of the register file, kept current by the register changes each item carries. Items:
// draws, query reports, swaps, deferred guest-memory writes (fences, the read pointer) and drains (the producer
// waits until the consumer has run everything before it). Swaps and drains also refresh the consumer's whole register
// copy from the live one (the producer is waiting then, so it is consistent) - registers only the title's CPU writes
// (MMIO) reach the renderer that way at least once a frame.
// Job 240: constant-range generations (dev_tools.h XenosConstGens) - written and read on the renderer thread only.
XenosConstGenerations g_const_gens;
inline void BumpConstGen(uint32_t index) {
  if (index >= 0x4000 && index < 0x4400) ++g_const_gens.vs;
  else if (index >= 0x4400 && index < 0x4800) ++g_const_gens.ps;
  else if (index >= 0x4900 && index < 0x4928) ++g_const_gens.bool_loop;
}
inline void BumpAllConstGens() {
  ++g_const_gens.vs;
  ++g_const_gens.ps;
  ++g_const_gens.bool_loop;
}

class RenderPipe {
 public:
  enum Type : uint8_t { kDraw, kZpd, kSwap, kWrite, kDrain };
  // Patch 0148: the live register file and gamma ramps at a swap the producer does not wait for.
  struct SwapSnap {
    std::vector<uint32_t> regs;
    ::xenos::display_gamma::Ramps gamma;
  };
  struct Item {
    Type type = kDraw;
    uint32_t a = 0, b = 0, c = 0, d = 0;  // per type: draw initiator / index base / index size / indexed
    uint64_t e = 0;
    uint32_t changes_end = 0;  // this item's register changes are block.changes[previous item's end, this end)
    uint8_t bytes[16] = {};
    std::shared_ptr<const pm::ShaderRef> vs, ps;
    std::shared_ptr<const SwapSnap> snap;  // patch 0148: a kSwap queued with --gow2_pm4_swap_ahead
  };
  struct Block {
    std::vector<Item> items;
    std::vector<std::pair<uint32_t, uint32_t>> changes;
  };
  struct Stats {
    uint64_t items = 0, blocks = 0, drains = 0, waits_full = 0;  // producer
    uint64_t swaps_synced = 0, swaps_ahead = 0, swap_waits = 0;  // patch 0148 (producer)
    double swap_sync_ms = 0, swap_wait_ms = 0;  // producer time spent waiting at swaps (synced / ahead)
    std::atomic<uint64_t> writes{0}, consumer_sleeps{0};       // consumer (read for the log)
  };

  RenderPipe(const RegisterFile* live, rex::memory::Memory* memory) : live_(live), memory_(memory) {
    std::memcpy(Shadow().values, live_->values, sizeof(live_->values));
    BumpAllConstGens();
    g_const_gens.valid = true;
    current_ = NewBlock();
    thread_ = std::thread([this] { Run(); });
  }

  // ---- producer (the GPU thread) ----
  // Register changes since the previous item, read from the live file now.
  void Changes(const std::vector<uint32_t>& indices) {
    for (uint32_t i : indices)
      if (i < RegisterFile::kRegisterCount) current_->changes.emplace_back(i, live_->values[i]);
  }
  Item& Add(Type t) {
    Item& it = current_->items.emplace_back();
    it.type = t;
    it.changes_end = uint32_t(current_->changes.size());
    ++stats_.items;
    return it;
  }
  void Added() {
    if (current_->items.size() >= kBlockItems || current_->changes.size() >= kBlockChanges) Publish();
  }
  // Job 173b: a deferred write goes out at once - the title may be waiting for it with the ring idle (job 173 hung
  // after the first frame: the fence sat in an unpublished block). The read pointer write ends every kick, so the
  // draws before it go out then too.
  void AddedNow() { Publish(); }
  void Drain() {
    const uint64_t seq = ++drain_seq_;
    Item& it = Add(kDrain);
    it.e = seq;
    Publish();
    ++stats_.drains;
    for (int i = 0; i < kSpin && drained_.load(std::memory_order_acquire) < seq; ++i) _mm_pause();
    if (drained_.load(std::memory_order_acquire) >= seq) return;
    std::unique_lock<std::mutex> lock(mutex_);
    producer_waiting_ = true;
    done_cv_.wait(lock, [&] { return drained_.load(std::memory_order_acquire) >= seq; });
    producer_waiting_ = false;
  }
  // Patch 0148: a swap the renderer thread completes itself (done() there, right after it presented). The producer
  // goes on parsing and waits only while more than `ahead` queued swaps have not been run yet.
  void SetSwapDone(std::function<void()> done) { swap_done_ = std::move(done); }
  void SwapAhead(uint32_t fb, uint32_t w, uint32_t h, uint32_t ahead) {
    auto snap = std::make_shared<SwapSnap>();
    snap->regs.assign(live_->values, live_->values + std::size(live_->values));
    snap->gamma = GammaState();
    Item& it = Add(kSwap);
    it.a = fb;
    it.b = w;
    it.c = h;
    it.snap = std::move(snap);
    const uint64_t n = ++swaps_queued_;
    Publish();
    ++stats_.swaps_ahead;
    auto caught_up = [&] { return n <= swaps_run_.load(std::memory_order_acquire) + ahead; };
    if (caught_up()) return;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kSpin && !caught_up(); ++i) _mm_pause();
    if (!caught_up()) {
      std::unique_lock<std::mutex> lock(mutex_);
      producer_waiting_ = true;
      done_cv_.wait(lock, caught_up);
      producer_waiting_ = false;
    }
    ++stats_.swap_waits;
    stats_.swap_wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }
  // The old swap: queue it and wait until the renderer has run it (and everything before it).
  void SwapSynced(uint32_t fb, uint32_t w, uint32_t h) {
    Item& it = Add(kSwap);
    it.a = fb;
    it.b = w;
    it.c = h;
    const auto t0 = std::chrono::steady_clock::now();
    Drain();
    ++stats_.swaps_synced;
    stats_.swap_sync_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }
  const Stats& GetStats() const { return stats_; }

 private:
  static constexpr size_t kBlockItems = 24, kBlockChanges = 16384, kMaxQueued = 512;
  static constexpr int kSpin = 4000;
  RegisterFile& Shadow() { return *reinterpret_cast<RegisterFile*>(shadow_storage_); }
  std::unique_ptr<Block> NewBlock() {
    std::unique_ptr<Block> b;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!free_.empty()) {
        b = std::move(free_.back());
        free_.pop_back();
      }
    }
    if (!b) b = std::make_unique<Block>();
    return b;
  }
  void Publish() {
    if (current_->items.empty()) return;
    ++stats_.blocks;
    bool wake;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (queue_.size() >= kMaxQueued) {  // the renderer is far behind: wait (bounded memory)
        ++stats_.waits_full;
        producer_waiting_ = true;
        done_cv_.wait(lock, [&] { return queue_.size() < kMaxQueued / 2; });
        producer_waiting_ = false;
      }
      queue_.push_back(std::move(current_));
      queued_.store(uint32_t(queue_.size()), std::memory_order_release);
      wake = consumer_sleeping_;
    }
    if (wake) cv_.notify_one();
    current_ = NewBlock();
  }

  // ---- consumer (the renderer thread) ----
  void Run() {
#ifdef _WIN32
    using SetDesc = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    if (auto fn = reinterpret_cast<SetDesc>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")))
      fn(GetCurrentThread(), L"GPU Render (ours)");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
    for (;;) {
      std::unique_ptr<Block> b;
      {
        for (int i = 0; i < kSpin && !queued_.load(std::memory_order_acquire); ++i) _mm_pause();
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.empty()) {
          ++stats_.consumer_sleeps;
          consumer_sleeping_ = true;
          cv_.wait(lock, [&] { return !queue_.empty(); });
          consumer_sleeping_ = false;
        }
        b = std::move(queue_.front());
        queue_.pop_front();
        queued_.store(uint32_t(queue_.size()), std::memory_order_release);
        if (producer_waiting_ && queue_.size() < kMaxQueued / 2) done_cv_.notify_all();
      }
      RunBlock(*b);
      b->items.clear();
      b->changes.clear();
      std::lock_guard<std::mutex> lock(mutex_);
      if (free_.size() < 64) free_.push_back(std::move(b));
    }
  }
 public:
  // Patch 0051 (renderer thread only, from inside a draw RunBlock dispatched): visit the shaders of the draws after
  // the current item, in this block and then in the queued ones. Returns the number of draws visited.
  uint32_t Lookahead(XenosUcodeVisitor visitor, void* ctx, uint32_t max_draws) {
    uint32_t n = 0;
    auto visit = [&](const Block& blk, size_t from) {
      for (size_t i = from; i < blk.items.size() && n < max_draws; ++i) {
        const Item& it = blk.items[i];
        if (it.type != kDraw || !it.vs || !it.ps) continue;
        const XenosUcode v{it.vs->hash, it.vs->dwords.empty() ? nullptr : it.vs->dwords.data(),
                           uint32_t(it.vs->dwords.size())};
        const XenosUcode p{it.ps->hash, it.ps->dwords.empty() ? nullptr : it.ps->dwords.data(),
                           uint32_t(it.ps->dwords.size())};
        visitor(ctx, v, p);
        ++n;
      }
    };
    if (running_block_) visit(*running_block_, running_item_ + 1);
    std::lock_guard<std::mutex> lock(mutex_);  // queued blocks are complete and not changed once published
    for (const auto& blk : queue_) {
      if (n >= max_draws) break;
      visit(*blk, 0);
    }
    return n;
  }

 private:
  void RunBlock(const Block& b) {
    RegisterFile& regs = Shadow();
    uint32_t c = 0;
    running_block_ = &b;  // patch 0051
    for (size_t index = 0; index < b.items.size(); ++index) {
      const Item& it = b.items[index];
      running_item_ = index;
      for (; c < it.changes_end; ++c) {
        regs.values[b.changes[c].first] = b.changes[c].second;
        BumpConstGen(b.changes[c].first);
      }
      switch (it.type) {
        case kDraw: {
          // ExecutePacketType3Draw's observer call, from our state (VGT_DRAW_INITIATOR / VGT_DMA_SIZE bitfields).
          const uint32_t initiator = it.a, index_base = it.b, index_size = it.c;
          const uint32_t prim = initiator & 0x3F, major = (initiator >> 8) & 3, is32 = (initiator >> 11) & 1;
          const uint32_t num_indices = initiator >> 16;
          ObservedIndexBuffer ib{};
          if (it.d) {
            const uint32_t bytes = is32 ? 4u : 2u;
            ib.format = rex::graphics::xenos::IndexFormat(is32);
            ib.endianness = rex::graphics::xenos::Endian(index_size >> 30);
            ib.count = num_indices;
            ib.guest_base = index_base & ~(bytes - 1);
            ib.length = uint64_t(index_size & 0xFFFFFF) * bytes;
          }
          const XenosUcode v{it.vs->hash, it.vs->dwords.empty() ? nullptr : it.vs->dwords.data(),
                             uint32_t(it.vs->dwords.size())};
          const XenosUcode p{it.ps->hash, it.ps->dwords.empty() ? nullptr : it.ps->dwords.data(),
                             uint32_t(it.ps->dwords.size())};
          XenosDispatchOurDraw(regs, prim, num_indices, it.d ? &ib : nullptr, major != 0 || prim >= 0x10, v, p);
          break;
        }
        case kZpd:
          XenosDispatchOurZpd(regs, it.a, it.b != 0, uint8_t(it.c), it.e);
          break;
        case kSwap:
          if (it.snap) {  // patch 0148: the producer may be parsing the next frame - the live file is not ours to read
            std::memcpy(regs.values, it.snap->regs.data(),
                        std::min(sizeof(regs.values), it.snap->regs.size() * sizeof(uint32_t)));
            g_gamma_at_swap = it.snap->gamma;
            g_gamma_from_swap.store(true, std::memory_order_release);
          } else {
            std::memcpy(regs.values, live_->values, sizeof(regs.values));  // the producer waits right after this
          }
          BumpAllConstGens();
          XenosDispatchOurSwap(regs, it.a, it.b, it.c);
          if (it.snap) {
            XenosRendererRelease();  // as the synced swap's drain did: our deferred guest-memory copies land first
            // The title sees this swap complete now, as with the synced swap. Known differences (review of 0148): an
            // EVENT_WRITE_SHD timestamp parsed in the next frame before this point reads the counter one lower; the
            // title's CPU reading GPU registers can see the next frame's values before this swap completes.
            if (swap_done_) swap_done_();
            swaps_run_.fetch_add(1, std::memory_order_acq_rel);
            std::lock_guard<std::mutex> lock(mutex_);
            if (producer_waiting_) done_cv_.notify_all();
          }
          break;
        case kWrite:
          XenosRendererRelease();  // our deferred guest-memory copies first: the title may reuse them after this
          if (uint8_t* p = memory_->TranslatePhysical<uint8_t*>(it.a)) std::memcpy(p, it.bytes, it.b);
          XenosNotifyGuestWrite();  // job 225: a title thread may be sleeping on this (fence / read pointer)
          ++stats_.writes;
          break;
        case kDrain:
          XenosRendererRelease();
          std::memcpy(regs.values, live_->values, sizeof(regs.values));
          BumpAllConstGens();
          {
            std::lock_guard<std::mutex> lock(mutex_);
            drained_.store(it.e, std::memory_order_release);
            if (producer_waiting_) done_cv_.notify_all();
          }
          break;
      }
    }
    running_block_ = nullptr;
  }

  const RegisterFile* live_;
  rex::memory::Memory* memory_;
  alignas(RegisterFile) unsigned char shadow_storage_[sizeof(RegisterFile)];
  std::unique_ptr<Block> current_;  // producer only
  std::mutex mutex_;
  std::condition_variable cv_, done_cv_;
  std::deque<std::unique_ptr<Block>> queue_;
  std::vector<std::unique_ptr<Block>> free_;
  std::atomic<uint32_t> queued_{0};
  std::atomic<uint64_t> drained_{0};
  uint64_t drain_seq_ = 0;  // producer only
  uint64_t swaps_queued_ = 0;  // patch 0148: producer only
  std::atomic<uint64_t> swaps_run_{0};  // patch 0148: renderer thread writes, producer reads
  std::function<void()> swap_done_;  // patch 0148: set before the first item, called on the renderer thread
  bool consumer_sleeping_ = false, producer_waiting_ = false;
  const Block* running_block_ = nullptr;  // patch 0051: renderer thread only
  size_t running_item_ = 0;
  Stats stats_;  // producer-side counters (consumer_sleeps / writes: consumer; read for the log only)
  std::thread thread_;  // never joined: the pipe lives until the process exits
};
RenderPipe* g_pipe = nullptr;  // set once, on the GPU thread, before the first item

// Patch 0053: shaders met for the first time, recorded by the producer (GPU thread) as it parses each draw and taken by
// the renderer thread, so their compiles start while the renderer is still on earlier draws.
struct NewShaders {
  std::mutex mutex;
  std::vector<std::pair<std::shared_ptr<const pm::ShaderRef>, bool>> list;  // (ucode, is_vertex)
  std::atomic<bool> pending{false};
  std::unordered_set<uint64_t> seen[2];  // producer only: [pixel, vertex] hashes already recorded
};
NewShaders g_new_shaders;
void NoteNewShader(const std::shared_ptr<const pm::ShaderRef>& s, bool is_vertex) {
  if (!s || !s->valid || s->dwords.empty()) return;
  if (!g_new_shaders.seen[is_vertex ? 1 : 0].insert(s->hash).second) return;
  std::lock_guard<std::mutex> lock(g_new_shaders.mutex);
  g_new_shaders.list.emplace_back(s, is_vertex);
  g_new_shaders.pending.store(true, std::memory_order_release);
}
std::string PipeSummary() {
  if (!g_pipe) return {};
  const RenderPipe::Stats& p = g_pipe->GetStats();
  return " | split: items " + std::to_string(p.items) + " in " + std::to_string(p.blocks) + " blocks, drains " +
         std::to_string(p.drains) + ", writes run " + std::to_string(p.writes.load()) + ", producer waits (full) " +
         std::to_string(p.waits_full) + ", renderer sleeps " + std::to_string(p.consumer_sleeps.load()) +
         " | swaps synced " + std::to_string(p.swaps_synced) + " (waited " +
         std::to_string(int64_t(p.swap_sync_ms)) + " ms), ahead " + std::to_string(p.swaps_ahead) + " (waits " +
         std::to_string(p.swap_waits) + ", " + std::to_string(int64_t(p.swap_wait_ms)) + " ms)";
}

// Phase 2: our processor runs the ring; the SDK's GpuServices do what still lives there.
class Pm4Execute final : public DrawObserver {
 public:
  Pm4Execute(rex::memory::Memory* memory, int every) : memory_(memory), every_(every) {}

  void OnPrimaryRange(uint32_t ring_physical, uint32_t ring_dwords, uint32_t read_index,
                      uint32_t write_index) override {
    if (!proc_) {
      GpuServices* s = GetGpuServices();
      if (!s) return;  // cannot happen with patch 21 (set right before this call)
      pm::Environment env;
      rex::memory::Memory* memory = memory_;
      env.translate = [memory](uint32_t a) -> uint8_t* { return memory->TranslatePhysical<uint8_t*>(a); };
      // Stage 5C: every register write we run also feeds our gamma ramp tracker (the DC_LUT_* protocol).
      env.write_register = [w = s->write_register](uint32_t index, uint32_t value) {
        GammaState().OnRegisterWrite(index, value);
        w(index, value);
      };
      g_gamma_tracked = true;
      env.read_register = s->read_register;
      env.execute_packet = s->execute_packet;
      env.counter = s->counter;
      env.running = s->running;
      env.make_coherent = s->make_coherent;
      env.vsync = s->vsync;
      env.release = [] { XenosRendererRelease(); };  // job 171: our renderer's deferred copies finish first
      const RegisterFile* split_rf = s->register_file ? s->register_file() : nullptr;
      const bool split = REXCVAR_GET(gow2_pm4_split) && REXCVAR_GET(gow2_pm4_own_draws) && split_rf;
      if (REXCVAR_GET(gow2_pm4_split) && !split)
        REXLOG_ERROR("gow2_pm4_split: needs --gow2_pm4_own_draws and the register file; staying on one thread");
      if (REXCVAR_GET(gow2_pm4_own_draws)) {
        const RegisterFile* rf = s->register_file ? s->register_file() : nullptr;
        if (!rf) {
          REXLOG_ERROR("gow2_pm4_execute: the SDK gives no register file (patch 22 missing); draws stay with the SDK");
        } else {
          env.hash = [](const uint8_t* p, size_t n) { return uint64_t(XXH3_64bits(p, n)); };
          if (REXCVAR_GET(gow2_pm4_fast_constants)) {
            env.host_registers = const_cast<uint32_t*>(rf->values);
            env.track_changes = false;
          }
          if (split) {
            g_pipe = new RenderPipe(rf, memory);  // never destroyed
            env.track_changes = true;  // every register change travels with the next item
            env.release = [] { g_pipe->Drain(); };
            env.defer_write = [](uint32_t a, const void* bytes, uint32_t n) { return XenosPm4DeferWrite(a, bytes, n); };
            env.own_draw = [this](const pm::Draw& d, const pm::ShaderRef&, const pm::ShaderRef&) {
              PipeChanges();
              RenderPipe::Item& it = g_pipe->Add(RenderPipe::kDraw);
              it.a = d.initiator;
              it.b = d.index_base;
              it.c = d.index_size;
              it.d = d.indexed ? 1u : 0u;
              it.vs = proc_->CurrentVs();
              it.ps = proc_->CurrentPs();
              NoteNewShader(it.vs, true);  // patch 0053
              NoteNewShader(it.ps, false);
              g_pipe->Added();
            };
            REXLOG_INFO("gow2_pm4_split: ON - the ring is parsed on this thread, our renderer runs on its own");
          } else
          env.own_draw = [rf](const pm::Draw& d, const pm::ShaderRef& vs, const pm::ShaderRef& ps) {
            // ExecutePacketType3Draw's observer call, from our state (VGT_DRAW_INITIATOR / VGT_DMA_SIZE bitfields).
            const uint32_t prim = d.initiator & 0x3F, major = (d.initiator >> 8) & 3, index_size = (d.initiator >> 11) & 1;
            const uint32_t num_indices = d.initiator >> 16;
            ObservedIndexBuffer ib{};
            if (d.indexed) {
              const uint32_t bytes = index_size ? 4u : 2u;
              ib.format = rex::graphics::xenos::IndexFormat(index_size);
              ib.endianness = rex::graphics::xenos::Endian(d.index_size >> 30);
              ib.count = num_indices;
              ib.guest_base = d.index_base & ~(bytes - 1);
              ib.length = uint64_t(d.index_size & 0xFFFFFF) * bytes;
            }
            const XenosUcode v{vs.hash, vs.dwords.empty() ? nullptr : vs.dwords.data(), uint32_t(vs.dwords.size())};
            const XenosUcode p{ps.hash, ps.dwords.empty() ? nullptr : ps.dwords.data(), uint32_t(ps.dwords.size())};
            XenosDispatchOurDraw(*rf, prim, num_indices, d.indexed ? &ib : nullptr, major != 0 || prim >= 0x10, v, p);
          };
        }
      }
      if (REXCVAR_GET(gow2_pm4_own_zpd)) {
        const RegisterFile* rf = s->register_file ? s->register_file() : nullptr;
        const bool no_fake = rex::cvar::GetFlagByName("occlusion_query_no_fake") == "true";
        const std::string fake_text = rex::cvar::GetFlagByName("query_occlusion_fake_sample_count");
        int fake = 1000;
        try { fake = std::stoi(fake_text); } catch (...) {}
        if (!rf) {
          REXLOG_ERROR("gow2_pm4_execute: the SDK gives no register file (patch 22 missing); ZPD stays with the SDK");
        } else if (no_fake) {
          REXLOG_ERROR("gow2_pm4_execute: occlusion_query_no_fake is on (SDK host counts); ZPD stays with the SDK");
        } else {
          env.own_zpd = [this, rf, memory, fake](uint32_t report, bool is_end) {
            // CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD's fallback (what the SDK did in takeover, job 117):
            // clear the 8 lanes; an end gets the fake count in ZPass_A and Total_A (little-endian lanes, as the SDK).
            uint8_t* p = report ? memory->TranslatePhysical<uint8_t*>(report) : nullptr;
            auto answer = rex::graphics::ZpdAnswer::kNone;
            uint64_t samples = 0;
            if (fake >= 0 && p) {
              std::memset(p, 0, 32);
              if (is_end) {
                const uint32_t v = uint32_t(fake);
                std::memcpy(p + 0, &v, 4);   // Total_A
                std::memcpy(p + 16, &v, 4);  // ZPass_A
                samples = v;
              }
              answer = is_end ? rex::graphics::ZpdAnswer::kFake : rex::graphics::ZpdAnswer::kBegin;
            }
            if (g_pipe) {  // job 173: in order with the draws, on the renderer thread
              PipeChanges();
              RenderPipe::Item& it = g_pipe->Add(RenderPipe::kZpd);
              it.a = report;
              it.b = is_end ? 1u : 0u;
              it.c = uint32_t(answer);
              it.e = samples;
              g_pipe->Added();
              return;
            }
            XenosDispatchOurZpd(*rf, report, is_end, uint8_t(answer), samples);
          };
        }
        REXLOG_INFO("gow2_pm4_execute: ZPD is ours: {} (fake count {})", env.own_zpd ? "on" : "OFF", fake);
      }
      if (XenosRendererPresents()) {  // Stage 5C-2: our renderer presents, so XE_SWAP is ours
        const RegisterFile* rf = s->register_file ? s->register_file() : nullptr;
        if (!rf || !s->swap_done) {
          REXLOG_ERROR("gow2_pm4_execute: the SDK lacks patch {} ; XE_SWAP stays with the SDK (our image is NOT shown)",
                       rf ? "23 (GpuServices::swap_done)" : "22 (register_file)");
        } else {
          // Patch 0148: --gow2_pm4_swap_ahead (our GPU system only: its swap_done is an atomic counter, safe to bump
          // from the renderer thread).
          const int ahead_cvar = REXCVAR_GET(gow2_pm4_swap_ahead);
          const uint32_t ahead = (g_pipe && XenosOurGpuSystem()) ? uint32_t(std::clamp(ahead_cvar, 0, 2)) : 0u;
          if (ahead_cvar && !ahead)
            REXLOG_ERROR("gow2_pm4_swap_ahead: needs --gow2_pm4_split and our GPU system; swaps stay synced");
          if (ahead) {
            g_pipe->SetSwapDone(s->swap_done);
            swap_ahead_ = true;
            REXLOG_INFO("gow2_pm4_swap_ahead: ON - the GPU thread may queue {} swap(s) past the one being presented",
                        ahead);
          }
          env.own_swap = [this, rf, done = s->swap_done, ahead](uint32_t fb, uint32_t w, uint32_t h) {
            if (g_pipe) {  // job 173: the renderer thread presents; we wait for it (a swap is a frame boundary)
              PipeChanges();
              if (ahead) {  // patch 0148: the renderer calls done() itself once it presented
                g_pipe->SwapAhead(fb, w, h, ahead);
                CountFrame();  // the stats line, on this thread (the counters it reads are ours)
                return;
              }
              g_pipe->SwapSynced(fb, w, h);
              done();
              return;
            }
            XenosDispatchOurSwap(*rf, fb, w, h);  // observers: our renderer presents here
            done();                                // the SDK's profiler flip + swap counter
          };
          REXLOG_INFO("gow2_pm4_execute: XE_SWAP is ours (our renderer presents)");
        }
      }
      proc_ = std::make_unique<pm::Processor>(std::move(env), pm::Callbacks{});
      REXLOG_INFO("gow2_pm4_execute: OUR PM4 processor now runs the ring (the SDK parses nothing){}",
                  REXCVAR_GET(gow2_pm4_own_draws) ? "; draws and shader loads are ours too" : "");
    }
    proc_->ExecutePrimary(ring_physical, ring_dwords, read_index, write_index);
  }
  void OnDraw(const RegisterFile&, rex::graphics::xenos::PrimitiveType, uint32_t, const ObservedIndexBuffer*,
              bool) override {}
  void OnSwap(const RegisterFile&, uint32_t, uint32_t, uint32_t) override {
    if (!swap_ahead_) CountFrame();  // patch 0148: with swaps ahead the GPU thread counts (this runs on the renderer's)
  }
  void CountFrame() {
    ++frames_;
    if (!proc_ || frames_ % uint64_t(every_)) return;
    const pm::Stats& s = proc_->GetStats();
    REXLOG_INFO("gow2_pm4_execute: frames 1-{}: packets {}, type3 {}, indirect buffers {}, draws {}, shader loads {} (cached {}), "
                "swaps {} | handed back to the SDK {} | memory words written by us {} (of {}) | waits {} (loops {}, "
                "spins {}) | ZPD ours {} (ends {}) | predicated skipped {}, unknown opcodes {} (last 0x{:02X}), errors {} | releases {} (reads {}, waits {}), deferred writes {}{}",
                frames_, s.packets, s.type3, s.indirect, s.draws, s.shader_loads, s.shader_loads_cached, s.swaps, s.handed_back,
                s.memory_writes_done, s.memory_writes_seen, s.waits, s.wait_loops, s.wait_spins, s.zpds, s.zpd_ends,
                s.predicated_skipped,
                s.unknown_opcodes, s.last_unknown_opcode, s.errors, s.releases, s.release_reads, s.release_waits,
                s.deferred_writes, PipeSummary());
  }

 public:
  // Job 173: the register changes since the previous item go into the pipe (GPU thread).
  void PipeChanges() {
    changed_.clear();
    proc_->TakeChangedIndices(changed_);
    g_pipe->Changes(changed_);
  }

 private:
  rex::memory::Memory* memory_;
  int every_;
  uint64_t frames_ = 0;
  bool swap_ahead_ = false;  // patch 0148: set before the processor exists
  std::unique_ptr<pm::Processor> proc_;
  std::vector<uint32_t> changed_;
};

}  // namespace

namespace {
std::atomic<uint32_t> g_guest_write_seq{0};
}  // namespace

uint32_t XenosGuestWriteSeq() { return g_guest_write_seq.load(std::memory_order_acquire); }

uint32_t XenosLookaheadUcode(XenosUcodeVisitor visitor, void* ctx, uint32_t max_draws) {  // patch 0051
  return g_pipe && visitor ? g_pipe->Lookahead(visitor, ctx, max_draws) : 0u;
}

bool XenosNewUcodePending() { return g_new_shaders.pending.load(std::memory_order_acquire); }  // patch 0053

uint32_t XenosTakeNewUcode(XenosUcodeVisitor visitor, void* ctx) {  // patch 0053
  if (!visitor || !g_new_shaders.pending.load(std::memory_order_acquire)) return 0;
  std::vector<std::pair<std::shared_ptr<const pm::ShaderRef>, bool>> list;
  {
    std::lock_guard<std::mutex> lock(g_new_shaders.mutex);
    list.swap(g_new_shaders.list);
    g_new_shaders.pending.store(false, std::memory_order_release);
  }
  for (const auto& [s, is_vertex] : list) {
    const XenosUcode u{s->hash, s->dwords.data(), uint32_t(s->dwords.size())};
    if (is_vertex) visitor(ctx, u, XenosUcode{});
    else visitor(ctx, XenosUcode{}, u);
  }
  return uint32_t(list.size());
}

XenosConstGenerations XenosConstGens() { return g_const_gens; }

void XenosNotifyGuestWrite() {
  g_guest_write_seq.fetch_add(1, std::memory_order_acq_rel);
  WakeByAddressAll(&g_guest_write_seq);
}

void XenosWaitGuestWrite(uint32_t seen, uint32_t timeout_ms) {
  if (g_guest_write_seq.load(std::memory_order_acquire) != seen) return;
  WaitOnAddress(&g_guest_write_seq, &seen, sizeof(seen), timeout_ms);  // returns early on any write; spurious wakes ok
}

bool XenosPm4DeferWrite(uint32_t physical, const void* bytes, uint32_t size) {
  if (!g_pipe || size > 16) return false;
  RenderPipe::Item& it = g_pipe->Add(RenderPipe::kWrite);
  it.a = physical;
  it.b = size;
  std::memcpy(it.bytes, bytes, size);
  g_pipe->AddedNow();
  return true;
}

const ::xenos::display_gamma::Ramps* XenosGammaRamps() {
  if (!g_gamma_tracked) return nullptr;
  return g_gamma_from_swap.load(std::memory_order_acquire) ? &g_gamma_at_swap : &GammaState();  // patch 0148
}

void XenosInstallPm4ShadowIfRequested(rex::Runtime* runtime) {
  if (REXCVAR_GET(gow2_pm4_execute)) {
    rex::memory::Memory* memory = runtime ? runtime->memory() : nullptr;
    if (!memory) {
      REXLOG_ERROR("gow2_pm4_execute: no guest memory; the SDK keeps parsing");
      return;
    }
    if (XenosOurGpuSystem()) {  // Stage 6: no SDK GPU plugin - our graphics system calls OnPrimaryRange itself
      const int every = REXCVAR_GET(gow2_pm4_shadow) > 0 ? REXCVAR_GET(gow2_pm4_shadow) : 300;
      XenosRegisterDrawObserver(new Pm4Execute(memory, every));
      REXLOG_INFO("gow2_pm4_execute: on (our graphics system) - stats every {} frames", every);
      return;
    }
    if (!rex::cvar::SetFlagByName("gpu_external_pm4", "true")) {
      REXLOG_ERROR("gow2_pm4_execute: the SDK has no gpu_external_pm4 (patch 21); the SDK keeps parsing");
      return;
    }
    const int every = REXCVAR_GET(gow2_pm4_shadow) > 0 ? REXCVAR_GET(gow2_pm4_shadow) : 300;
    XenosRegisterDrawObserver(new Pm4Execute(memory, every));
    REXLOG_INFO("gow2_pm4_execute: on - stats every {} frames", every);
    return;  // the shadow comparison needs the SDK parsing too: not both
  }
  const int every = REXCVAR_GET(gow2_pm4_shadow);
  if (every <= 0) return;
  rex::memory::Memory* memory = runtime ? runtime->memory() : nullptr;
  if (!memory) {
    REXLOG_ERROR("gow2_pm4_shadow: no guest memory; staying off");
    return;
  }
  XenosRegisterDrawObserver(new Pm4Shadow(memory, every));  // never destroyed (the hub keeps the pointer)
  REXLOG_INFO("gow2_pm4_shadow: on - our PM4 processor runs on every ring range and is compared at every draw");
}
