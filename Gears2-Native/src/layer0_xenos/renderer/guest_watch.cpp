// Layer 0 (game-free): see xenos/renderer/guest_watch.h. Our code (MIT).
#include <xenos/renderer/guest_watch.h>

#include <rex/system/xmemory.h>

#include <algorithm>
#include <utility>

namespace xenos::renderer {

GuestWatch::GuestWatch(rex::memory::Memory* memory)
    : memory_(memory),
      pages_(new std::atomic<uint64_t>[kPages]),
      blocks_(new std::atomic<uint64_t>[kPages >> kBlockShift]),
      supers_(new std::atomic<uint64_t>[kPages >> kSuperShift]) {
  for (uint32_t i = 0; i < kPages; ++i) pages_[i].store(0, std::memory_order_relaxed);
  for (uint32_t i = 0; i < (kPages >> kBlockShift); ++i) blocks_[i].store(0, std::memory_order_relaxed);
  for (uint32_t i = 0; i < (kPages >> kSuperShift); ++i) supers_[i].store(0, std::memory_order_relaxed);
  if (memory_) memory_->RegisterPhysicalMemoryInvalidationCallback(&GuestWatch::Callback, this);
}

std::pair<uint32_t, uint32_t> GuestWatch::Callback(void* context, uint32_t start, uint32_t length, bool exact_range) {
  auto* self = static_cast<GuestWatch*>(context);
  self->callbacks_.fetch_add(1, std::memory_order_relaxed);
  start &= 0x1FFFFFFFu;
  if (exact_range && length) {  // green-vine review (diagnostic): GPU-current pages this exact bump ends
    const uint32_t first = start >> kPageShift;
    const uint32_t last = uint32_t(std::min<uint64_t>((uint64_t(start) + length - 1) >> kPageShift, kPages - 1));
    uint64_t n = 0;
    for (uint32_t p = first; p <= last; ++p) n += self->GpuCurrent(p) ? 1u : 0u;
    if (n) self->gpu_exact_.fetch_add(n, std::memory_order_relaxed);
  }
  if (!exact_range) {
    const uint32_t ex_first = start >> kPageShift;
    const uint32_t ex_last =
        uint32_t(std::min<uint64_t>((uint64_t(start) + std::max(length, 1u) - 1) >> kPageShift, kPages - 1));
    if (self->narrow_) {
      // Patch 0008, as the reference emulator's shared memory: widen within the enclosing 64-page block only, and stop
      // before any page our GPU wrote (the mirror holds its newest bytes; guest memory does not). The memory system
      // unprotects no more than the range we return, so writes beyond it still raise their own callbacks.
      uint32_t first = ex_first, last = ex_last;
      const uint32_t block_first = first & ~63u, block_last = std::min<uint32_t>(last | 63u, kPages - 1);
      while (first > block_first && !self->GpuCurrent(first - 1)) --first;
      while (last < block_last && !self->GpuCurrent(last + 1)) ++last;
      start = first << kPageShift;
      length = (last - first + 1) << kPageShift;
      self->Bump(start, length);
      return {start, length};
    }
    // The memory system may unwatch up to the enclosing 4 MB block (intersected with every handler's wish; we wish
    // for everything), and later writes there raise no callback: treat the whole block as changed.
    constexpr uint64_t kBlock = 4u << 20;
    const uint64_t end = std::min<uint64_t>((uint64_t(start) + std::max(length, 1u) + kBlock - 1) & ~(kBlock - 1),
                                            512ull << 20);
    start &= ~uint32_t(kBlock - 1);
    length = uint32_t(end - start);
    // Patch 0008 diagnostic: GPU-written pages this widening is about to mark as CPU-written though the CPU wrote
    // outside them.
    const uint32_t w_first = start >> kPageShift, w_last = uint32_t(std::min<uint64_t>((end - 1) >> kPageShift, kPages - 1));
    uint64_t lost = 0;
    for (uint32_t p = w_first; p <= w_last; ++p)
      if ((p < ex_first || p > ex_last) && self->GpuCurrent(p)) ++lost;
    if (lost) self->gpu_lost_.fetch_add(lost, std::memory_order_relaxed);
  }
  self->Bump(start, length);
  return {0, UINT32_MAX};  // no wish of our own about how much gets unwatched
}

void GuestWatch::BumpPages(uint32_t first, uint32_t last, uint64_t set) {
  for (uint32_t p = first; p <= last; ++p) {
    uint64_t old = pages_[p].load(std::memory_order_relaxed);
    for (;;) {
      // version + 1 (wrapping within its 32 bits); "ever written by the GPU" stays, the current-flags are replaced
      const uint64_t next = uint64_t(uint32_t(old) + 1u) | (old & kEverGpuWritten) | set;
      if (pages_[p].compare_exchange_weak(old, next, std::memory_order_release, std::memory_order_relaxed)) break;
    }
  }
}

void GuestWatch::BumpRange(uint32_t address, uint32_t length, uint64_t set) {
  address &= 0x1FFFFFFFu;
  if (!length) return;
  const uint32_t first = address >> kPageShift;
  const uint32_t last = uint32_t(std::min<uint64_t>((uint64_t(address) + length - 1) >> kPageShift, kPages - 1));
  BumpPages(first, last, set);
  // A 256 KB block's (4 MB super-block's) counter is the sum of its page versions, so it goes up by the number of its
  // pages bumped here: one add per block instead of one per page (a 4 MB widening: 1024 + 16 + 1 adds, was 3072).
  // Readers see the same final sums; mid-update they could already see a page ahead of its block before, too.
  for (uint32_t p = first; p <= last;) {
    const uint32_t end = std::min(last + 1, ((p >> kBlockShift) + 1) << kBlockShift);
    blocks_[p >> kBlockShift].fetch_add(end - p, std::memory_order_relaxed);
    p = end;
  }
  for (uint32_t p = first; p <= last;) {
    const uint32_t end = std::min(last + 1, ((p >> kSuperShift) + 1) << kSuperShift);
    supers_[p >> kSuperShift].fetch_add(end - p, std::memory_order_relaxed);
    p = end;
  }
  epoch_.fetch_add(1, std::memory_order_release);
}

void GuestWatch::Bump(uint32_t address, uint32_t length) { BumpRange(address, length, 0); }

// An unscaled write leaves the scaled copy behind: its new version carries kGpuCurrent only.
void GuestWatch::MarkGpuWritten(uint32_t address, uint32_t length, bool scaled) {
  BumpRange(address, length, kGpuCurrent | kEverGpuWritten | (scaled ? kScaledCurrent : 0));
}

bool GuestWatch::SettleGpuPage(uint32_t page, uint32_t version) {
  if (page >= kPages) return false;
  uint64_t old = pages_[page].load(std::memory_order_relaxed);
  for (;;) {
    if (uint32_t(old) != version || !(old & kGpuCurrent)) return false;  // written since (CPU or GPU): leave it
    const uint64_t next = uint64_t(uint32_t(old) + 1u) | (old & kEverGpuWritten);
    if (pages_[page].compare_exchange_weak(old, next, std::memory_order_release, std::memory_order_relaxed)) break;
  }
  // The block sums follow the page (as BumpRange), then the epoch.
  blocks_[page >> kBlockShift].fetch_add(1, std::memory_order_relaxed);
  supers_[page >> kSuperShift].fetch_add(1, std::memory_order_relaxed);
  epoch_.fetch_add(1, std::memory_order_release);
  return true;
}

uint32_t GuestWatch::UnwatchablePages(uint32_t address, uint32_t length) const {
  address &= 0x1FFFFFFFu;
  if (!memory_ || !length) return 0;
  const uint32_t first = address >> kPageShift;
  const uint32_t last = uint32_t(std::min<uint64_t>((uint64_t(address) + length - 1) >> kPageShift, kPages - 1));
  uint32_t n = 0;
  for (uint32_t p = first; p <= last; ++p) {
    const uint32_t phys = p << kPageShift;
    bool writable = false;
    // The three physical views (xmemory.cpp PhysicalHeap::GetPhysicalAddress: E0000000 is offset by 0x1000).
    const uint32_t views[3] = {0xA0000000u + phys, 0xC0000000u + phys, phys >= 0x1000u ? 0xE0000000u + phys - 0x1000u : 0u};
    for (uint32_t v : views) {
      if (!v) continue;
      rex::memory::BaseHeap* heap = memory_->LookupHeap(v);
      uint32_t protect = 0;
      if (heap && heap->heap_type() == rex::memory::HeapType::kGuestPhysical && heap->QueryProtect(v, &protect) &&
          (protect & rex::memory::kMemoryProtectWrite)) {
        writable = true;
        break;
      }
    }
    if (!writable) ++n;
  }
  return n;
}

uint64_t GuestWatch::Version(uint32_t address, uint32_t length) const {
  address &= 0x1FFFFFFFu;
  if (!length) return 0;
  const uint32_t first = address >> kPageShift;
  const uint32_t last = uint32_t(std::min<uint64_t>((uint64_t(address) + length - 1) >> kPageShift, kPages - 1));
  // Same value as the plain sum of page versions, read through whole 4 MB / 256 KB blocks where the range covers them.
  uint64_t sum = 0;
  constexpr uint32_t kBlock = 1u << kBlockShift, kSuper = 1u << kSuperShift;
  uint32_t p = first;
  const uint32_t end = last + 1;
  while (p < end) {
    if ((p & (kSuper - 1)) == 0 && p + kSuper <= end) {
      sum += supers_[p >> kSuperShift].load(std::memory_order_acquire);
      p += kSuper;
    } else if ((p & (kBlock - 1)) == 0 && p + kBlock <= end) {
      sum += blocks_[p >> kBlockShift].load(std::memory_order_acquire);
      p += kBlock;
    } else {
      sum += uint32_t(pages_[p].load(std::memory_order_acquire));
      ++p;
    }
  }
  return sum;
}

void GuestWatch::Watch(uint32_t address, uint32_t length) {
  if (memory_ && length) memory_->EnablePhysicalMemoryAccessCallbacks(address & 0x1FFFFFFFu, length, true, false);
}

}  // namespace xenos::renderer
