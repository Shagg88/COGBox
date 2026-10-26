#!/usr/bin/env python3
"""Apply Gears 3 patch 0016 (draw trace for --gow2_renderer_log_vs) to Gears2-Native shadow_renderer.cpp (e1ec04a).

Job 085 [V]: the ch2 tone-map draw (vs A9C7FE3382D41904) has the same vertices, viewport, scissor and alpha test on
rainbow and clean frames, yet writes 0 px on rainbow frames. This logs, for each draw of the --gow2_renderer_log_vs
shader, the colour-target decision (EDRAM mode, pixel shader present / state, targets written, write mask, the
colour key, drawable) and WHERE the draw ends: drawn, or which skip / return. Diagnostic only; nothing changes
unless --gow2_renderer_log_vs is set.
Usage: make_patch_0016.py <in> <out>"""
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


# 1. Skip helpers report the traced draw.
edit('''  void Skip(const char* reason) { ++stats_.skipped[reason]; }''',
     '''  void Skip(const char* reason) {
    ++stats_.skipped[reason];
    TraceDraw(reason);  // patch 0016
  }

  // Patch 0016: where a --gow2_renderer_log_vs draw ends (drawn, or the skip / return that stopped it).
  void TraceDraw(const char* outcome) {
    if (!trace_draw_) return;
    trace_draw_ = false;  // one outcome per draw
    REXLOG_INFO("gow2_renderer: vs-trace frame {} draw {}: {}", frame_ + 1, draw_index_, outcome);
  }''')

edit('''  void SkipForShaders(bool pending) {
    if (pending) {''',
     '''  void SkipForShaders(bool pending) {
    TraceDraw(pending ? "skipped: shader / pipeline pending" : "skipped: shader / pipeline failed");  // patch 0016
    if (pending) {''')

# 2. Arm the trace at the start of Draw.
edit('''    ++stats_.draws;
    const uint64_t vs_hash = vs_.dwords ? vs_.hash : 0, ps_hash = ps_.dwords ? ps_.hash : 0;''',
     '''    ++stats_.draws;
    const uint64_t vs_hash = vs_.dwords ? vs_.hash : 0, ps_hash = ps_.dwords ? ps_.hash : 0;
    trace_draw_ = log_vs_hash_ && vs_hash == log_vs_hash_ && trace_logged_ < log_vs_max_;  // patch 0016
    if (trace_draw_) ++trace_logged_;''')

# 3. The colour decision, just before the "nothing we draw" branch.
edit('''    const bool query_proxy = !depth_writes && !draw_colour && occ_open_.open && occ_open_.handle != 0;''',
     '''    if (trace_draw_) {  // patch 0016
      const rr::RenderTargetKey k0 = colour_count ? colour_keys[0] : rr::RenderTargetKey{};
      REXLOG_INFO("gow2_renderer: vs-trace frame {} draw {}: ps {:016X} edram mode {} psg {} (state {}) targets "
                  "written {:X} mask0 {:X} | colour targets {} key0 base {} class {} pitch {} msaa {} | draw colour {} | "
                  "depth bound {} writes {} | drawn rect {},{}-{},{} | surface msaa {} pitch {}",
                  frame_ + 1, draw_index_, ps_hash, edram_mode, psg ? 1 : 0, psg ? int(psg->state) : -1, ps_colour,
                  ps.blend.rt[0].write_mask, colour_count, k0.base_tiles, k0.storage_class, k0.pitch_pixels, k0.msaa,
                  draw_colour ? 1 : 0, depth_bound ? 1 : 0, depth_writes ? 1 : 0, own.drawn.x0, own.drawn.y0,
                  own.drawn.x1, own.drawn.y1, msaa, pitch);
    }
    const bool query_proxy = !depth_writes && !draw_colour && occ_open_.open && occ_open_.handle != 0;''')

edit('''      ++stats_.colour_only;  // colour into formats not modelled yet (or colour switched off)''',
     '''      TraceDraw("skipped: nothing we draw (no colour target taken, no depth write)");  // patch 0016
      ++stats_.colour_only;  // colour into formats not modelled yet (or colour switched off)''')

edit('''    if (own.drawn.empty()) {
      ++stats_.empty;''',
     '''    if (own.drawn.empty()) {
      TraceDraw("skipped: empty drawn rectangle");  // patch 0016
      ++stats_.empty;''')

edit('''      target = rts_->Get(key);
      if (!target) {
        ++stats_.other_failures;''',
     '''      target = rts_->Get(key);
      if (!target) {
        TraceDraw("skipped: no depth target");  // patch 0016
        ++stats_.other_failures;''')

edit('''    if (!pipeline) {
      ++stats_.pipeline_failures;
      ++pipeline_failures_this_frame_;''',
     '''    if (!pipeline) {
      TraceDraw("skipped: pipeline failed");  // patch 0016
      ++stats_.pipeline_failures;
      ++pipeline_failures_this_frame_;''')

edit('''      if (!idx_hit && !ReadIndices(*ib, index_count, reset_index, use_restart)) {
        ++stats_.other_failures;''',
     '''      if (!idx_hit && !ReadIndices(*ib, index_count, reset_index, use_restart)) {
        TraceDraw("skipped: index read failed");  // patch 0016
        ++stats_.other_failures;''')

edit('''    ++stats_.drawn;
    ++drawn_since_copy_;''',
     '''    TraceDraw("DRAWN");  // patch 0016
    ++stats_.drawn;
    ++drawn_since_copy_;''')

# 4. Members.
edit('''  int32_t log_vs_max_ = 0, log_vs_logged_ = 0;''',
     '''  int32_t log_vs_max_ = 0, log_vs_logged_ = 0;
  bool trace_draw_ = false;                          // patch 0016: this draw is traced
  int32_t trace_logged_ = 0;''')

open(sys.argv[2], "w", encoding="utf-8", newline="").write(src)
print("ok")
