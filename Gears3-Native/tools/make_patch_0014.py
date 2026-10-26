#!/usr/bin/env python3
"""Apply patch 0014 (arm the write watch on resolve pages) to Gears2-Native shadow_renderer.cpp.
Usage: make_patch_0014.py <in> <out>"""
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

edit('''REXCVAR_DEFINE_BOOL(gow2_renderer_watch_narrow,''',
     '''// Patch 0014 (Gears 3 notes/15, 2026-10-23): our resolves marked their pages GPU-current without arming the write
// watch there (the memexport path arms first). A later CPU write to those pages - the game reusing the memory, e.g.
// streaming a texture in - then raised no callback: the pages stayed GPU-current, GPU-path texture loads kept reading
// our old resolve from the mirror, and CPU-path loads showed up as "CPU-path loads that read our pages (should be 0)"
// (ch22: 74 at load, 10-20 per 300 gameplay frames).
REXCVAR_DEFINE_BOOL(gow2_renderer_watch_resolves, false, "Gears2",
                    "Arm the CPU write watch on the pages each of our resolves writes, so a later CPU write there ends "
                    "their GPU-current state (use with --gow2_renderer_watch_narrow=true; the old 4 MB widening would "
                    "drop GPU-current pages the CPU did not write)");
REXCVAR_DEFINE_BOOL(gow2_renderer_watch_narrow,''')

edit('''    watch_->MarkGpuWritten(info.dest_extent_start, info.dest_extent_length, scaled);
    if (through_on) {''',
     '''    if (REXCVAR_GET(gow2_renderer_watch_resolves))  // patch 0014: as the memexport path, arm before marking
      watch_->Watch(info.dest_extent_start, info.dest_extent_length);
    watch_->MarkGpuWritten(info.dest_extent_start, info.dest_extent_length, scaled);
    if (through_on) {''')
open(sys.argv[2], "w", encoding="utf-8", newline="").write(src)
print("ok")
