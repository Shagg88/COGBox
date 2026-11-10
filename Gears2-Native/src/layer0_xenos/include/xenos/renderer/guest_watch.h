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
    return page < kPages && gpu_versions_[page].load(std::memory_order_acquire) ==
                                versions_[page].load(std::memory_order_acquire);
  }
  // Stage 7: true while the page's newest content came from a resolve that also wrote its scaled copy.
  bool ScaledCurrent(uint32_t page) const {
    return page < kPages && scaled_versions_[page].load(std::memory_order_acquire) ==
                                versions_[page].load(std::memory_order_acquire);
  }
  // Job 193: the page never held scaled data, nor did our GPU ever write it (no resolve touched it).
  bool NeverResolved(uint32_t page) const {
    return page < kPages && scaled_versions_[page].load(std::memory_order_acquire) == UINT32_MAX &&
           gpu_versions_[page].load(std::memory_order_acquire) == UINT32_MAX;
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
    return page < kPages ? versions_[page].load(std::memory_order_acquire) : 0u;
  }

 private:
  static std::pair<uint32_t, uint32_t> Callback(void* context, uint32_t start, uint32_t length, bool exact_range);
  rex::memory::Memory* memory_;
  // Per page, and the same sums per 64 pages (256 KB) and per 1024 pages (4 MB), so a Version over a large range
  // reads O(blocks) counters instead of one per page (upl-n102).
  static constexpr uint32_t kBlockShift = 6, kSuperShift = 10;
  std::unique_ptr<std::atomic<uint32_t>[]> versions_;
  std::unique_ptr<std::atomic<uint32_t>[]> gpu_versions_;  // T2: version at our last GPU write (UINT32_MAX = never)
  std::unique_ptr<std::atomic<uint32_t>[]> scaled_versions_;  // Stage 7: version at our last scaled resolve
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
