#!/usr/bin/env python3
"""Apply Gears 3 patch 0018 (CPU-path texture loads see what our GPU wrote - the Canary way) to Gears2-Native
shadow_renderer.cpp (e1ec04a; independent of 0016 / 0017).

Xenia Canary keeps one GPU-side copy of guest memory (its shared memory): resolves and memexport write there, and every
texture is loaded from there, so a texture always sees what the GPU last wrote. Our renderer loads most textures on the
GPU from its mirror the same way, but some formats (DXT/BC blocks among them) are untiled on the CPU from guest memory -
which, in takeover, never receives our resolves (T2 writes the mirror only) and gets our memexport copies late. Layer 0
already counts the case: "CPU-path loads that read our pages (this range, should be 0)" - ch22 Gears 3: 3-78 per 300
frames. Job 087 Z1 [V]: shadow mode with the emulator drawing but its resolves kept out of guest memory
(--readback_resolve=none) gives the same blotchy black-red squad as takeover.

This patch: before a CPU-path texture load reads pages that are still GPU-current (last written by our resolve or
memexport into the mirror), those pages are read back from the mirror into guest memory (the GPU work so far is
finished first), so the CPU load reads the same bytes a GPU load would - as in Canary. Only GPU-current page runs are
copied, so newer CPU writes elsewhere are never overwritten. --gow2_renderer_cpu_loads_from_mirror (default ON - Shagg
2026-10-23 07:18: "do it the Canary way"); off = as before.
Usage: make_patch_0018.py <in> <out>"""
import sys

src = open(sys.argv[1], encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in src else "\n"


def edit(old, new):
    global src
    old, new = old.replace("\n", nl), new.replace("\n", nl)
    n = src.count(old)
    if n != 1:
        sys.exit(f"anchor found {n} times: {old[:90]!r}")
    src = src.replace(old, new)


edit('''REXCVAR_DEFINE_BOOL(gow2_renderer_resolve_through, false, "Gears2",''',
     '''// Gears 3 patch 0018 (notes/15, ch22 black squad in takeover; the Canary way): a CPU-path texture load over pages our
// GPU wrote (resolve / memexport into the mirror) first copies those pages from the mirror into guest memory.
REXCVAR_DEFINE_BOOL(gow2_renderer_cpu_loads_from_mirror, true, "Gears2",
                    "Before a CPU-path texture load (DXT/BC and other CPU-untiled formats) reads pages our resolves or "
                    "exports wrote into the GPU mirror, read those pages back into guest memory, so the texture gets the "
                    "bytes the GPU wrote (as Xenia Canary, which loads every texture from its GPU copy). false = the CPU "
                    "load reads guest memory as it is (stale in takeover)");
REXCVAR_DEFINE_BOOL(gow2_renderer_resolve_through, false, "Gears2",''')

edit('''    if (memexport_readback_)  // job 042: moved textures load their new bytes, not the old ones for a frame
      textures_->SetBeforeCpuLoadOfGpuPages([this](uint32_t start, uint32_t length) { FlushExportReadbacksFor(start, length); });''',
     '''    cpu_loads_from_mirror_ = REXCVAR_GET(gow2_renderer_cpu_loads_from_mirror);  // patch 0018
    if (memexport_readback_ || cpu_loads_from_mirror_)  // job 042: moved textures load their new bytes, not the old ones
      textures_->SetBeforeCpuLoadOfGpuPages([this](uint32_t start, uint32_t length) {
        if (memexport_readback_) FlushExportReadbacksFor(start, length);
        if (cpu_loads_from_mirror_) CopyGpuPagesToGuest(start, length);  // patch 0018
      });''')

edit('''  void PollExportReadbacks() {''',
     '''  // Patch 0018: the GPU-current page runs of [start, start + length) go from the mirror into guest memory before a
  // CPU-path texture load reads them. The GPU work recorded so far (the resolve / export that wrote them) is finished
  // first. Both copies then hold the same bytes, so the pages stay GPU-current (the physical-mapping copy raises no
  // watch callback - see the job 041 note at PollExportReadbacks).
  void CopyGpuPagesToGuest(uint32_t start, uint32_t length) {
    if (!length) return;
    constexpr uint32_t kShift = rr::GuestWatch::kPageShift;
    const uint32_t first = (start & 0x1FFFFFFFu) >> kShift;
    const uint32_t last = uint32_t(std::min<uint64_t>(((uint64_t(start & 0x1FFFFFFFu) + length - 1) >> kShift),
                                                      rr::GuestWatch::kPages - 1));
    struct Run {
      gpu::ReadbackHandle handle;
      uint32_t base, size;
    };
    std::vector<Run> runs;
    for (uint32_t p = first; p <= last;) {
      if (!watch_->GpuCurrent(p)) {
        ++p;
        continue;
      }
      uint32_t q = p;
      while (q + 1 <= last && watch_->GpuCurrent(q + 1)) ++q;
      const uint32_t base = p << kShift, size = (q - p + 1) << kShift;
      if (const gpu::ReadbackHandle h = backend_->ReadbackGuest(base, size)) runs.push_back({h, base, size});
      else ++mirror_copy_stats_.failed;
      p = q + 1;
    }
    if (runs.empty()) return;
    backend_->WaitForFence(backend_->Submit());
    for (const Run& run : runs) {
      uint32_t pitch = 0;
      const uint8_t* data = backend_->ReadbackData(run.handle, pitch);
      uint8_t* dst = data ? memory_->TranslatePhysical<uint8_t*>(run.base) : nullptr;
      if (dst) {
        std::memcpy(dst, data, run.size);
        ++mirror_copy_stats_.runs;
        mirror_copy_stats_.bytes += run.size;
      } else {
        ++mirror_copy_stats_.failed;
      }
      backend_->ReleaseReadback(run.handle);
    }
    ++mirror_copy_stats_.loads;
  }

  void PollExportReadbacks() {''')

edit('''  std::deque<ExportReadback> export_readbacks_;''',
     '''  std::deque<ExportReadback> export_readbacks_;
  bool cpu_loads_from_mirror_ = false;  // patch 0018
  struct {
    uint64_t loads = 0, runs = 0, bytes = 0, failed = 0;
  } mirror_copy_stats_;  // patch 0018 (totals since start)''')

edit('''    if (stats_.memexport_draws || stats_.memexport_only_draws) {  // notes/11 (this summary's frames)''',
     '''    if (mirror_copy_stats_.loads || mirror_copy_stats_.failed)  // patch 0018 (totals since start)
      REXLOG_INFO("gow2_renderer: CPU-path loads fed from the mirror: {} loads, {} page runs ({:.2f} MB), {} failed",
                  mirror_copy_stats_.loads, mirror_copy_stats_.runs, double(mirror_copy_stats_.bytes) / 1048576.0,
                  mirror_copy_stats_.failed);
    if (stats_.memexport_draws || stats_.memexport_only_draws) {  // notes/11 (this summary's frames)''')

open(sys.argv[2], "w", encoding="utf-8", newline="").write(src)
print("ok")
