#!/usr/bin/env python3
"""Apply Gears 3 patch 0015 (vertex log for one vertex shader) to Gears2-Native shadow_renderer.cpp.
Usage: make_patch_0015.py <in> <out>"""
import sys

src = open(sys.argv[1], encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in src else "\n"


def edit(old, new):
    global src
    old, new = old.replace("\n", nl), new.replace("\n", nl)
    n = src.count(old)
    if n != 1:
        sys.exit(f"anchor found {n} times: {old[:80]!r}")
    src = src.replace(old, new)


edit('''REXCVAR_DEFINE_STRING(gow2_renderer_compare_dump_frames,''',
     '''// Gears 3 patch 0015 (notes/15, ch2 rainbow: the tone-map draw is issued but rasterizes nothing; its VS takes the
// position straight from vf0): log the vertex bytes a chosen vertex shader's draws read, as uploaded. Diagnostic only.
REXCVAR_DEFINE_STRING(gow2_renderer_log_vs, "", "Gears2",
                      "Vertex shader hash (16 hex digits, e.g. A9C7FE3382D41904): log each of its draws' vertex fetch "
                      "constants and the first vertices (as floats, after the endian swap) at upload time (empty = off)");
REXCVAR_DEFINE_INT32(gow2_renderer_log_vs_max, 400, "Gears2", "Most draws --gow2_renderer_log_vs logs");
REXCVAR_DEFINE_STRING(gow2_renderer_compare_dump_frames,''')

edit('''  void UploadVertexSlot(const uint8_t* base, uint32_t i, uint32_t dw0, uint32_t dw1) {
    {
      const uint32_t address = dw0 & 0x1FFFFFFCu;
      uint64_t size = uint64_t((dw1 >> 2) & 0xFFFFFFu) * 4;
      if (!size) return;''',
     '''  void UploadVertexSlot(const uint8_t* base, uint32_t i, uint32_t dw0, uint32_t dw1) {
    {
      const uint32_t address = dw0 & 0x1FFFFFFCu;
      uint64_t size = uint64_t((dw1 >> 2) & 0xFFFFFFu) * 4;
      if (!size) return;
      if (log_vs_hash_ && vs_.hash == log_vs_hash_) LogVertexBytes(base, i, address, size, dw1 & 3u);''')

edit('''  // Comparison-frame self-check of the upload watch: a skipped range whose bytes changed anyway is "stale".''',
     '''  // Patch 0015: the bytes this draw's vertex fetch reads, now (what the upload copies), as up to 24 floats.
  void LogVertexBytes(const uint8_t* base, uint32_t slot, uint32_t address, uint64_t size, uint32_t endian) {
    if (log_vs_logged_ >= log_vs_max_ || address + size > (512ull << 20)) return;
    ++log_vs_logged_;
    const uint32_t words = uint32_t(std::min<uint64_t>(size / 4, 24));
    std::string text;
    for (uint32_t k = 0; k < words; ++k) {
      uint32_t w;
      std::memcpy(&w, base + address + 4 * k, 4);
      w = Swap32(w, endian);
      text += fmt::format("{}{:.6g}", k % 8 ? " " : (k ? " | " : ""), std::bit_cast<float>(w));
    }
    uint32_t gpu_pages = 0;
    for (uint32_t p = address >> rr::GuestWatch::kPageShift;
         p <= uint32_t((address + size - 1) >> rr::GuestWatch::kPageShift); ++p)
      gpu_pages += watch_->GpuCurrent(p) ? 1u : 0u;
    REXLOG_INFO("gow2_renderer: vs-log frame {} draw {}: vs {:016X} vf{} {:08X} size {} endian {} gpu-current pages {} "
                "| vertex words as floats: {}",
                frame_ + 1, draw_index_, vs_.hash, slot, address, size, endian, gpu_pages, text);
  }

  // Comparison-frame self-check of the upload watch: a skipped range whose bytes changed anyway is "stale".''')

edit('''  bool dump_parsed_ = false;                         // --gow2_renderer_compare_dump_frames''',
     '''  uint64_t log_vs_hash_ = 0;                         // patch 0015 (set with the other switches)
  int32_t log_vs_max_ = 0, log_vs_logged_ = 0;
  bool dump_parsed_ = false;                         // --gow2_renderer_compare_dump_frames''')

edit('''    vertex_direct_ = REXCVAR_GET(gow2_renderer_vertex_direct);''',
     '''    vertex_direct_ = REXCVAR_GET(gow2_renderer_vertex_direct);
    log_vs_hash_ = std::strtoull(REXCVAR_GET(gow2_renderer_log_vs).c_str(), nullptr, 16);  // patch 0015
    log_vs_max_ = REXCVAR_GET(gow2_renderer_log_vs_max);''')

edit('''    textures_part.Switch(kPartVertex);
    std::memcpy(fetch_, &r[kRegFetchConst], sizeof(fetch_));
    UploadVertexData(r, vsg);''',
     '''    textures_part.Switch(kPartVertex);
    std::memcpy(fetch_, &r[kRegFetchConst], sizeof(fetch_));
    UploadVertexData(r, vsg);
    if (log_vs_hash_ && vs_.hash == log_vs_hash_ && log_vs_logged_ <= log_vs_max_)  // patch 0015 v2: the draw's state
      REXLOG_INFO("gow2_renderer: vs-log frame {} draw {}: prim {} count {} | viewport {} {} {} {} z {} {} scale {} {} "
                  "offset {} {} xy_div_w {} clip_disabled {} | scissor {},{} {}x{} | alpha test func {}",
                  frame_ + 1, draw_index_, prim, index_count, ps.viewport.x, ps.viewport.y, ps.viewport.width,
                  ps.viewport.height, ps.viewport.z_min, ps.viewport.z_max, ps.viewport.ndc_scale[0],
                  ps.viewport.ndc_scale[1], ps.viewport.ndc_offset[0], ps.viewport.ndc_offset[1],
                  ps.viewport.xy_divided_by_w ? 1 : 0, ps.viewport.clip_disabled ? 1 : 0, ps.scissor.x, ps.scissor.y,
                  ps.scissor.width, ps.scissor.height, int(ps.alpha_test.func));''')

open(sys.argv[2], "w", encoding="utf-8", newline="").write(src)
print("ok")
