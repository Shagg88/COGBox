// Layer 0 (game-free): see xenos/renderer/guest_watch.h. Our code (MIT).
#include <xenos/renderer/guest_watch.h>

#include <xenos/dev_tools.h>

#include <rex/logging/macros.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace xenos::renderer {

namespace {
// Diagnostic only (the watch report): the host call stack of the write that raised a reported callback, with symbol
// names from the PDB where DbgHelp finds them (else module+offset, for llvm-symbolizer --relative-address) - shows
// which kernel call (file read, free, decommit, protect) and which game function caused it.
std::string HostStack() {
#if defined(_WIN32)
  static std::mutex mutex;
  static int ready = 0;  // 0 not tried, 1 ok, -1 failed
  std::lock_guard<std::mutex> lock(mutex);
  HANDLE process = GetCurrentProcess();
  if (ready == 0) {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    ready = SymInitialize(process, nullptr, TRUE) ? 1 : -1;
  }
  void* frames[28] = {};
  const USHORT n = CaptureStackBackTrace(1, 28, frames, nullptr);
  alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256] = {};
  auto* sym = reinterpret_cast<SYMBOL_INFO*>(storage);
  std::string out;
  for (USHORT i = 0; i < n; ++i) {
    if (!out.empty()) out += " < ";
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (ready == 1 && SymFromAddr(process, DWORD64(frames[i]), &disp, sym)) {
      out += std::string(sym->Name, sym->NameLen);
      continue;
    }
    HMODULE m = nullptr;
    char path[MAX_PATH] = {};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(frames[i]), &m) &&
        m && GetModuleFileNameA(m, path, MAX_PATH)) {
      const char* slash = strrchr(path, '\\');
      out += fmt::format("{}+{:X}", slash ? slash + 1 : path, uint64_t(uintptr_t(frames[i]) - uintptr_t(m)));
    } else {
      out += fmt::format("{:X}", uint64_t(uintptr_t(frames[i])));
    }
  }
  return out;
#else
  return "no stack on this platform";
#endif
}
}  // namespace

void GuestWatch::SetReportRanges(std::vector<std::pair<uint32_t, uint32_t>> ranges) {
  report_ = std::move(ranges);
  if (!report_.empty()) {
    const std::string warm = HostStack();
    REXLOG_INFO("gow2_renderer: watch report call stacks ready (sample: {})", warm.substr(0, 300));
  }
}

GuestWatch::GuestWatch(rex::memory::Memory* memory)
    : memory_(memory),
      versions_(new std::atomic<uint32_t>[kPages]),
      gpu_versions_(new std::atomic<uint32_t>[kPages]),
      scaled_versions_(new std::atomic<uint32_t>[kPages]),
      blocks_(new std::atomic<uint64_t>[kPages >> kBlockShift]),
      supers_(new std::atomic<uint64_t>[kPages >> kSuperShift]) {
  for (uint32_t i = 0; i < kPages; ++i) versions_[i].store(0, std::memory_order_relaxed);
  for (uint32_t i = 0; i < kPages; ++i) gpu_versions_[i].store(UINT32_MAX, std::memory_order_relaxed);
  for (uint32_t i = 0; i < kPages; ++i) scaled_versions_[i].store(UINT32_MAX, std::memory_order_relaxed);
  for (uint32_t i = 0; i < (kPages >> kBlockShift); ++i) blocks_[i].store(0, std::memory_order_relaxed);
  for (uint32_t i = 0; i < (kPages >> kSuperShift); ++i) supers_[i].store(0, std::memory_order_relaxed);
  if (memory_) memory_->RegisterPhysicalMemoryInvalidationCallback(&GuestWatch::Callback, this);
}

std::pair<uint32_t, uint32_t> GuestWatch::Callback(void* context, uint32_t start, uint32_t length, bool exact_range) {
  auto* self = static_cast<GuestWatch*>(context);
  self->callbacks_.fetch_add(1, std::memory_order_relaxed);
  start &= 0x1FFFFFFFu;
  if (!self->report_.empty()) {
    const uint64_t a = start, b = uint64_t(start) + std::max(length, 1u);
    for (const auto& [rs, rl] : self->report_) {
      if (a < uint64_t(rs) + rl && b > rs) {
        if (self->report_lines_.fetch_add(1, std::memory_order_relaxed) < 120) {
          auto* t = rex::system::XThread::GetCurrentThread();
          REXLOG_INFO("gow2_renderer: WATCH write {:08X}+{:X} ({}) into {:08X}+{:X} | swap {} | guest thread {}", start,
                      length, exact_range ? "exact range: a host-side write such as a file read" : "page: a CPU store",
                      rs, rl, XenosSwapCount(), t ? fmt::format("{:X}", t->thread_id()) : std::string("none (host)"));
          REXLOG_INFO("gow2_renderer: WATCH stack {}", HostStack());
        }
        break;
      }
    }
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

void GuestWatch::Bump(uint32_t address, uint32_t length) {
  address &= 0x1FFFFFFFu;
  if (!length) return;
  const uint32_t first = address >> kPageShift;
  const uint32_t last = uint32_t(std::min<uint64_t>((uint64_t(address) + length - 1) >> kPageShift, kPages - 1));
  for (uint32_t p = first; p <= last; ++p) versions_[p].fetch_add(1, std::memory_order_relaxed);
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

void GuestWatch::MarkGpuWritten(uint32_t address, uint32_t length, bool scaled) {
  address &= 0x1FFFFFFFu;
  if (!length) return;
  Bump(address, length);
  const uint32_t first = address >> kPageShift;
  const uint32_t last = uint32_t(std::min<uint64_t>((uint64_t(address) + length - 1) >> kPageShift, kPages - 1));
  for (uint32_t p = first; p <= last; ++p) {
    const uint32_t v = versions_[p].load(std::memory_order_acquire);
    gpu_versions_[p].store(v, std::memory_order_release);
    // An unscaled write leaves the scaled copy behind: UINT32_MAX never equals a version in practice.
    scaled_versions_[p].store(scaled ? v : UINT32_MAX, std::memory_order_release);
  }
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
      sum += versions_[p].load(std::memory_order_acquire);
      ++p;
    }
  }
  return sum;
}

void GuestWatch::Watch(uint32_t address, uint32_t length) {
  if (memory_ && length) memory_->EnablePhysicalMemoryAccessCallbacks(address & 0x1FFFFFFFu, length, true, false);
}

}  // namespace xenos::renderer
