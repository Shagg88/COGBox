// Layer 0 (game-free): which guest physical memory may have changed since we last copied it to the GPU (S3.5, upload
// cost; D24 "page watch"). Version counters per 4 KB page, bumped from the guest memory system's write-invalidation
// callbacks (rex::memory::Memory, the mechanism the emulator's shared memory uses: watched pages are write-protected
// and the first CPU write or kernel file read into them fires the callbacks, then the page is unprotected) and by our
// resolve path (GPU writes into guest memory, which raise no callback).
//
// Use: Watch(range) BEFORE copying the bytes, remember Version(range), and skip the next copy while Version(range) is
// unchanged. A write after Watch() always bumps the version; when the memory system unprotects more than the exact
// pages (up to the enclosing 4 MB block, xmemory.cpp kMaxUnwatchExcess) the whole block is bumped, because later writes
// there raise no further callback until the range is watched again.
//
// Thread safety: the callback runs on whichever guest thread wrote; counters are atomics. Our code (MIT).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace rex::memory { class Memory; }

namespace xenos::renderer {

class GuestWatch {
 public:
  static constexpr uint32_t kPageShift = 12;
  static constexpr uint32_t kPages = (512u << 20) >> kPageShift;
  explicit GuestWatch(rex::memory::Memory* memory);
  // Sum of the page versions of [address, address + length): changes whenever any page in it was written.
  uint64_t Version(uint32_t address, uint32_t length) const;
  // Ask the memory system to report CPU writes to [address, address + length).
  void Watch(uint32_t address, uint32_t length);
  // A write we know about without a callback (resolves).
  void Bump(uint32_t address, uint32_t length);
  // Takeover T2 (notes/50): our own resolve wrote [address, address + length) into the GPU's guest-memory mirror. Bumps
  // the pages and records that, at this version, the mirror (not the CPU bytes) holds their content: texture loads skip
  // re-uploading them and treat them as changed. A later CPU write bumps the version again and ends that.
  // Stage 7 (notes/53): scaled = the same resolve also wrote its resolution-scaled copy, which then holds these pages'
  // newest content at the higher resolution (until any other write - CPU or an unscaled resolve - to the page).
  void MarkGpuWritten(uint32_t address, uint32_t length, bool scaled = false);
  // True while the page's newest content is the one our GPU wrote (no CPU write since).
  bool GpuCurrent(uint32_t page) const {
    return page < kPages && (pages_[page].load(std::memory_order_acquire) & kGpuCurrent) != 0;
  }
  // Stage 7: true while the page's newest content came from a resolve that also wrote its scaled copy.
  bool ScaledCurrent(uint32_t page) const {
    return page < kPages && (pages_[page].load(std::memory_order_acquire) & kScaledCurrent) != 0;
  }
  // Job 193: the page never held scaled data, nor did our GPU ever write it (no resolve touched it).
  bool NeverResolved(uint32_t page) const {
    return page < kPages && (pages_[page].load(std::memory_order_acquire) & kEverGpuWritten) == 0;
  }
  bool ScaledCurrent(uint32_t address, uint32_t length) const {
    if (!length) return false;
    const uint32_t first = (address & 0x1FFFFFFFu) >> kPageShift;
    const uint32_t last = ((address & 0x1FFFFFFFu) + length - 1) >> kPageShift;
    for (uint32_t p = first; p <= last; ++p)
      if (!ScaledCurrent(p)) return false;
    return true;
  }
  uint64_t Callbacks() const { return callbacks_.load(std::memory_order_relaxed); }
  // Patch 0008: a CPU-store callback widens to the enclosing 4 MB block (the memory system may unprotect that much)
  // and bumps it all - including pages our GPU wrote (resolves, memexport) whose newest bytes live only in the mirror.
  // Those pages then stop being GPU-current although the CPU never touched them, and the next texture / vertex upload
  // copies the OLDER guest bytes over the mirror. GpuPagesLostToWidening() counts such pages (always on, diagnostic).
  // SetNarrowWidening(true) widens like the reference emulator's SharedMemory::MemoryInvalidationCallback instead:
  // only within the enclosing 64-page (256 KB) block, never across a GPU-current page, and returns that range to the
  // memory system as the most it may unprotect.
  uint64_t GpuPagesLostToWidening() const { return gpu_lost_.load(std::memory_order_relaxed); }
  void SetNarrowWidening(bool on) { narrow_ = on; }
  // Job 151 diagnostic: log every memory-system write callback that overlaps one of these physical ranges (start,
  // length): its exact range or page, the swap count and the guest thread (first 120). Set before the game runs.
  // Also loads the symbols the report's call stacks use, here and not inside a callback (which runs under the memory
  // system's global lock, where a first PDB load would stall the game for seconds and shift the very timing we watch).
  void SetReportRanges(std::vector<std::pair<uint32_t, uint32_t>> ranges);
  // Number of Bump calls so far (callbacks and resolves): while it is unchanged, no Version can have changed.
  uint64_t Epoch() const { return epoch_.load(std::memory_order_acquire); }
  // One 4 KB page's version (page = physical address >> kPageShift).
  uint32_t PageVersion(uint32_t page) const {
    return page < kPages ? uint32_t(pages_[page].load(std::memory_order_acquire)) : 0u;
  }

 private:
  static std::pair<uint32_t, uint32_t> Callback(void* context, uint32_t start, uint32_t length, bool exact_range);
  rex::memory::Memory* memory_;
  // Per page, and the same sums per 64 pages (256 KB) and per 1024 pages (4 MB), so a Version over a large range
  // reads O(blocks) counters instead of one per page (upl-n102).
  static constexpr uint32_t kBlockShift = 6, kSuperShift = 10;
  // 0.6.1 (review B1): one word per page - the version in the low 32 bits and, above it, who wrote that version. Before,
  // the version and "the version at our last GPU write" were two atomics written one after the other, so between
  // MarkGpuWritten's bump and its store a game thread's write callback saw the page as not GPU-current, widened across
  // it and bumped it: the resolve's result then counted as older than the guest's bytes (or the page stayed current
  // but unwatched). A CPU bump and a GPU write are now one compare-exchange each.
  static constexpr uint64_t kGpuCurrent = 1ull << 32;      // T2: our GPU wrote this version (the mirror holds it)
  static constexpr uint64_t kScaledCurrent = 1ull << 33;   // Stage 7: ... and its resolution-scaled copy
  static constexpr uint64_t kEverGpuWritten = 1ull << 34;  // job 193: some resolve wrote this page at some time
  std::unique_ptr<std::atomic<uint64_t>[]> pages_;
  // Bumps every page of the range: set = the flags the new version carries (0 for a CPU write).
  void BumpPages(uint32_t first, uint32_t last, uint64_t set);
  void BumpRange(uint32_t address, uint32_t length, uint64_t set);
  std::unique_ptr<std::atomic<uint64_t>[]> blocks_;
  std::unique_ptr<std::atomic<uint64_t>[]> supers_;
  std::atomic<uint64_t> epoch_{0};
  std::atomic<uint64_t> callbacks_{0};
  std::atomic<uint64_t> gpu_lost_{0};  // patch 0008
  bool narrow_ = false;                // patch 0008 (set before the game runs)
  std::vector<std::pair<uint32_t, uint32_t>> report_;
  std::atomic<uint32_t> report_lines_{0};
};

}  // namespace xenos::renderer
