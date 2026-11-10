#!/usr/bin/env python3
"""Apply Gears 3 patch 0017 (memexport copy check) to Gears2-Native shadow_renderer.cpp (e1ec04a; independent of 0016).

Gears 3 moves DXT textures with memexport copies (job 041). Job 086 [V]: with the emulator's drawing off (takeover, or
shadow + --gpu_skip_draws) the ch22 squad is a blotchy black / red pattern - wrong texture bytes [G]. This checks our
copies directly: for an export-only draw that reads exactly one vertex range of the same size as the range it exports
(a GPU memcpy), the source bytes are snapshotted at draw time and compared with what our export wrote when its
readback lands. A mismatch also tests simple byte-order transforms (16-bit / 32-bit / 64-bit swaps) to name the bug.
Diagnostic only, off by default: --gow2_renderer_export_copy_check=true (needs --gow2_renderer_memexport and the
default --gow2_renderer_memexport_readback).
Usage: make_patch_0017.py <in> <out>"""
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
     '''// Gears 3 patch 0017 (notes/15, ch22 black squad in takeover): check our memexport copies (GPU memcpy of a moved DXT
// texture) against their source bytes when the readback lands. Diagnostic only.
REXCVAR_DEFINE_BOOL(gow2_renderer_export_copy_check, false, "Gears2",
                    "With --gow2_renderer_memexport: an export-only draw that reads one vertex range of the size it "
                    "exports (a GPU copy) has its source bytes compared with our exported bytes when the readback lands; "
                    "mismatches are logged with the byte-order transform that would match (first 400 copies checked)");
REXCVAR_DEFINE_BOOL(gow2_renderer_resolve_through, false, "Gears2",''')

# Snapshot the source at the copy draw.
edit('''    ++stats_.memexport_only_draws;
    MarkExports(r, vsg);''',
     '''    ++stats_.memexport_only_draws;
    MarkExports(r, vsg);
    if (memexport_readback_ && copy_checks_started_ < 400 && REXCVAR_GET(gow2_renderer_export_copy_check))
      SnapshotExportCopy(r, vsg);  // patch 0017''')

# Compare when the readback lands, before it is written into guest memory.
edit('''      if (uint8_t* dst = data ? memory_->TranslatePhysical<uint8_t*>(e.base) : nullptr) {
        std::memcpy(dst, data, e.size);''',
     '''      if (uint8_t* dst = data ? memory_->TranslatePhysical<uint8_t*>(e.base) : nullptr) {
        if (!copy_checks_.empty()) CheckExportCopy(e.base, e.size, data);  // patch 0017
        std::memcpy(dst, data, e.size);''')

# Helpers, before MemexportOnlyDraw.
edit('''  void MemexportOnlyDraw(const uint32_t* r, const rr::GuestShader& vsg, uint32_t prim, uint32_t index_count,
                         const ObservedIndexBuffer* ib) {''',
     '''  // Patch 0017: a GPU copy = an export-only draw whose single exported range has the size of its single vertex range.
  void SnapshotExportCopy(const uint32_t* r, const rr::GuestShader& s) {
    uint32_t ebase = 0, esize = 0, efmt = 0, eendian = 0, streams = 0;
    for (uint32_t c : s.memexport_stream_constants) {
      if (c >= 256) continue;
      const uint32_t* w = &r[kRegFloatConstVs + 4 * c];
      if ((w[0] >> 30) != 1u || (w[2] >> 20) != 0x4B0u || (w[3] >> 23) != 0x96u) continue;
      const uint32_t fmt = (w[2] >> 8) & 63u;
      const uint32_t bytes_per = (fmt == 26 || fmt == 32 || fmt == 37) ? 8u : fmt == 38 ? 16u
                                 : (fmt == 6 || fmt == 7 || fmt == 25 || fmt == 31 || fmt == 36) ? 4u : 0u;
      ++streams;
      ebase = (w[0] & 0x3FFFFFFFu) << 2;
      esize = (w[3] & 0x7FFFFFu) * bytes_per;
      efmt = fmt;
      eendian = w[2] & 7u;
    }
    if (streams != 1 || !esize) return;
    uint32_t src = 0, ssize = 0, sendian = 0, ranges = 0;
    for (uint32_t i = 0; i < 96; ++i) {
      if (!((s.vfetch_constants[i >> 5] >> (i & 31)) & 1) || (r[kRegFetchConst + 2 * i] & 3) != 3) continue;
      ++ranges;
      src = r[kRegFetchConst + 2 * i] & 0x1FFFFFFCu;
      ssize = ((r[kRegFetchConst + 2 * i + 1] >> 2) & 0xFFFFFFu) * 4;
      sendian = r[kRegFetchConst + 2 * i + 1] & 3u;
    }
    if (ranges != 1 || ssize != esize || (ebase & 0x1FFFFFFFu) + uint64_t(esize) > (512ull << 20) ||
        uint64_t(src) + ssize > (512ull << 20))
      return;
    const uint8_t* mem = memory_->TranslatePhysical<const uint8_t*>(src);
    CopyCheck c;
    c.base = ebase & 0x1FFFFFFFu;
    c.size = esize;
    c.src = src;
    c.frame = frame_ + 1;
    c.draw = draw_index_;
    c.desc = std::format("vs {:016X} export fmt {} endian {} <- vf endian {}", vs_.hash, efmt, eendian, sendian);
    c.bytes.assign(mem, mem + ssize);
    copy_checks_.push_back(std::move(c));
    ++copy_checks_started_;
  }

  void CheckExportCopy(uint32_t base, uint32_t size, const uint8_t* ours) {
    for (auto it = copy_checks_.begin(); it != copy_checks_.end(); ++it) {
      if (it->base != (base & 0x1FFFFFFFu) || it->size != size) continue;
      const std::vector<uint8_t>& s = it->bytes;
      auto differ = [&](auto map) {
        uint32_t n = 0;
        for (uint32_t i = 0; i < size; ++i) n += ours[i] != s[map(i)] ? 1u : 0u;
        return n;
      };
      const uint32_t plain = differ([](uint32_t i) { return i; });
      ++copy_stats_[plain ? 1 : 0];
      if (plain && copy_logged_ < 60) {
        ++copy_logged_;
        const uint32_t sw16 = differ([](uint32_t i) { return i ^ 1u; });
        const uint32_t sw32 = differ([](uint32_t i) { return i ^ 3u; });
        const uint32_t sw64 = differ([](uint32_t i) { return i ^ 7u; });
        const uint32_t half32 = differ([](uint32_t i) { return i ^ 2u; });
        const uint32_t half64 = differ([](uint32_t i) { return i ^ 4u; });
        uint32_t first = 0;
        while (first < size && ours[first] == s[first]) ++first;
        const uint32_t at = first & ~7u;
        std::string ow, sw;
        for (uint32_t k = at; k < std::min(size, at + 16); ++k) {
          ow += std::format("{:02X}", ours[k]);
          sw += std::format("{:02X}", s[k]);
        }
        REXLOG_INFO("gow2_renderer: export copy MISMATCH frame {} draw {}: {:08X} <- {:08X} size {} ({}) | bytes differing "
                    "{} | if swapped: 8in16 {} 8in32 {} 8in64 {} 16in32 {} 32in64 | first at +{:X}: ours {} source {}",
                    it->frame, it->draw, it->base, it->src, size, it->desc, plain, sw16, sw32, sw64, half32, half64,
                    first, ow, sw);
      }
      copy_checks_.erase(it);
      return;
    }
  }

  void MemexportOnlyDraw(const uint32_t* r, const rr::GuestShader& vsg, uint32_t prim, uint32_t index_count,
                         const ObservedIndexBuffer* ib) {''')

# Members + a summary line with the readback summary.
edit('''  std::deque<ExportReadback> export_readbacks_;''',
     '''  std::deque<ExportReadback> export_readbacks_;
  struct CopyCheck {  // patch 0017
    uint32_t base = 0, size = 0, src = 0, draw = 0;
    uint64_t frame = 0;
    std::string desc;
    std::vector<uint8_t> bytes;
  };
  std::deque<CopyCheck> copy_checks_;
  uint32_t copy_checks_started_ = 0, copy_logged_ = 0;
  uint64_t copy_stats_[2] = {};  // patch 0017: copies that matched / mismatched''')

edit('''                  stats_.memexport_readback_waits);
    }''',
     '''                  stats_.memexport_readback_waits);
    }
    if (copy_checks_started_)  // patch 0017 (totals since start)
      REXLOG_INFO("gow2_renderer: export copy check: copies checked {} matched {} MISMATCHED {} (still pending {})",
                  copy_stats_[0] + copy_stats_[1], copy_stats_[0], copy_stats_[1], copy_checks_.size());''')

open(sys.argv[2], "w", encoding="utf-8", newline="").write(src)
print("ok")
