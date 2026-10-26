# Stage 6 - a build without the SDK's GPU emulation (plan, 2026-10-21)

## 0. Where Stage 5 left us (job 136)

**5C-2 passed (job 136):** our backend on the presenter's device and queue (RTX 3080 Ti); XE_SWAP ours; 2,100 frames,
0 errors; presented 2,097 (the first 3 boot swaps had no front-buffer copy yet, so nothing was presented for them),
refresh failed 0; handed back 2,100 (INTERRUPT only); 7 of 7 screen checks: the presenter's image (what the window
paints) equals ours, 100 % of pixels exact. Frame-600 shot: the same scene, a slightly different moment.

Per frame the SDK's GPU side does only: the ring-buffer worker thread (wait for the write pointer, call our
`OnPrimaryRange`, write the read pointer back), the register file storage and its write side effects (COHER_STATUS_HOST
dirty bit, scratch-register write-back), INTERRUPT packets (the title's interrupt callback), the vblank thread (swap
counter + interrupt source 0), the GPU MMIO range (CP_RB_WPTR writes, a few fixed register reads), and the provider +
presenter (window plumbing). Its command processor, texture cache, render-target cache, pipeline cache, shared memory
and swap path are loaded and initialised but no longer used.

## 1. The seam

The SDK takes a custom graphics system by design: `rex::system::IGraphicsSystem` (include/rex/system/interfaces/
graphics.h; "abstract graphics system interface for dependency injection"), chosen in `ReXApp::OnPreSetup` through
`RuntimeConfig::graphics`. So Stage 6 needs no SDK patch: our own `IGraphicsSystem`.

## 2. Gow2GraphicsSystem (layer 0, `--gow2_gpu=ours`; default stays the SDK's until it passes)

- `SetupPresentation`: the SDK's D3D12Provider + D3D12Presenter (window plumbing, kept; presenter created on the UI
  thread as the SDK does).
- `SetupGuestGpu`: the GPU MMIO range 0x7FC80000 (port of GraphicsSystem::ReadRegister/WriteRegister: CP_RB_WPTR ->
  our worker, fixed reads RB_EDRAM_TIMING / RB_BC_CONTROL / D1MODE_V_COUNTER / interrupt status / viewport size); the
  vblank thread (XHostThread, refresh rate from VdQueryVideoMode, counter + interrupt 0); our GPU worker thread
  (XHostThread: wait on the write pointer, run our PM4 processor over [read, write), write the read pointer back).
- The register file is ours (a `rex::graphics::RegisterFile` we own, so the observers keep their interface); write
  side effects as the SDK: COHER_STATUS_HOST dirty bit, scratch write-back, gamma (ours already).
- INTERRUPT packets: our processor calls the title's callback (port of DispatchInterruptCallback: SetActiveCpu,
  FunctionDispatcher::ExecuteInterrupt) on our worker thread.
- `swap_done` / counter, `make_coherent` (clear the COHER status; our GuestWatch already sees CPU writes), `vsync`.
- Renderer: unchanged; it already presents through `IGraphicsSystem::presenter()`.

## 3. Pass criteria (written before building)

- **6A (job 137)**: ch02 80 s with `--gow2_gpu=ours` (takeover + execute + own draws + own ZPD + present ours):
  no hang, ~2,100 frames, 0 processor errors, screen checks exact, frame-600 shot visually the same as job 136's,
  frames/s within 5 % of job 136's. The log must show the SDK command processor was never created.
- **6B**: ch10 (busier) and the menu, same checks. Then the default switches to ours.
- **6C**: the SDK GPU code is no longer linked (or provably dead), and CPU time per frame is measured against job 136.

## 4. 6A RESULT (job 137) and 6B (built 2026-10-21, job 138)

**6A passed (job 137, ch02 80 s):** run B (`--gow2_gpu=ours`): our system created the provider/presenter, the GPU
plugin was not loaded, the ring buffer (1FC9D000, 32 KB) and interrupt callback arrived through our system;
2,100 frames, 0 processor errors, 7 of 7 screen checks exact; frame interval mean 33.50 / 33.34 ms (frames 301-600 /
1801-2100) vs run A (SDK plugin) 33.50 / 33.33; frame-600 shot pixel-identical to job 136's. SDK log lines in the
`[gpu]` category: 2 in B (the D3D12 provider's adapter and device-feature report, from the UI library) vs 5 in A
(those plus the plugin's shader storage).

**6B:** `--gow2_gpu=ours` now presets the whole chain (gow2_renderer=takeover, gow2_pm4_execute, own draws, own ZPD,
gow2_present=ours), so it is the one switch. Job 138 runs ch10 (chapter 9, 150 s) on the SDK plugin (A, job 136's
flags) and on ours (B, only --gow2_gpu=ours), and the menu (no launch map, 60 s) on ours.
Pass: B and the menu: no hang, 0 processor errors, every screen check exact, frames presented = swaps after the first
few boot swaps; B's frame interval mean within 5 % of A's; shots visually the same scene as A's.
Then the default becomes ours (6C removes the plugin from the build).

## 5. 6B RESULT (job 138) and the flicker check (job 139)

**6B passed (job 138):** ch10 150 s: A (SDK plugin) and B (ours, only --gow2_gpu=ours) 3,000 frames each, 0 processor
errors, 5 of 5 screen checks exact in each; frame interval means B 54.8/51.5/51.2/48.9/46.3/46.7/46.4/46.7/47.9 ms vs
A 55.0/51.7/51.6/50.0/47.3/47.5/46.8/46.2/48.9 (B about 1 % faster). Menu 60 s on ours: 1,500 frames, 0 errors,
2 of 2 screen checks exact. Shots: the same scenes. (The job's "lines with errors" count was my filter matching
"errors 0" case-insensitively - no real errors.) ch10 runs at ~20 fps: our observer callbacks take ~40-45 ms/frame
there (CPU) - a speed item for later.

**Flicker (reported by Shagg watching job 138's ch10):** a few distant surfaces (mountain sides, rubble) flicker or
vanish briefly, every so often (not every second). Suspects: (a) our test tools (the screen check stalls a frame and
consumes the presenter's guest output; frame shots), (b) our occlusion answers (an object judged hidden from an old
count). The screen checks cannot see it (they compare our image with the presenter's - both would lack the object).
Job 139: ch10 150 s on ours with no screen check and no frame shots (1), then the same with
`--gow2_renderer_queries=shadow` (2: our renderer writes no answers, the reports keep the fake "visible" count, as the
emulator did). Takeover no longer overrides an explicit --gow2_renderer_queries. Watched by Shagg.

## 6. Flicker RESULT (job 139) and the fix (job 140)

**Job 139, watched by Shagg:** run 1 (our occlusion answers, no test tools touching frames): the flicker is there - so
it is real, not the screen check. Run 2 (`--gow2_renderer_queries=shadow`, no answers written, everything "visible"):
no flicker at all. Cause: our immediate occlusion answers. Sequence Shagg saw: a distant surface absent, appears
blurry, flickers, later sharp - the blurry-then-sharp part is the game's texture streaming (slower at ch10's ~20 fps,
a separate item: check our texture cache notices each new mip promptly).

Why: the immediate answer is the last real count for that report; a zero there means "hidden". Two ways that is wrong
for a frame: the object came into view since (one-frame-old zero), or the query object was reused for another
primitive (its zero belongs to someone else). A wrong "hidden" = a visible object skipped for a frame = flicker; a
wrong "visible" only costs a draw.

**Fix (`queries.h` AnswerCache, `--gow2_renderer_queries_hidden_after`, default 3):** "hidden" only after 3 real zero
counts in a row for the same query; "visible" at once. The estimate is keyed by report address + the number of draws in
the query (a reused query object for another primitive doesn't inherit). The correction never writes a zero that is
not trusted yet. New counters: answered hidden, WRONGLY hidden (answered 0, our count > 0 - the flicker), zeros not
yet trusted. Unit test (proven to fail with the old rule).
Pass for job 140 (ch10 150 s on ours, watched): no flicker seen; WRONGLY hidden near 0 (run 2 = old rule, for the
number); draws/frame and frame time within a few % of run 2 (culling mostly kept).

## 7. Job 140 (watched): the "3 zeros" rule did not fix it -> no answers by default (job 141)

Shagg watched job 140: run 1 (the fix) and run 2 (old rule) both flicker; distant surfaces flicker until they
"solidify", sharper. My diagnosis was wrong: the problem is latency, not stale zeros - our real counts land ~3 frames
after the query (job 139: "after 2.9 frames"), so an object coming into view stays "hidden" until its first real count
arrives, whatever rule picks the estimate; the game also drives texture streaming from visibility, so it can sharpen
later. Job 139 run 2 (nothing written, everything "visible", as the emulator) had no flicker at all.
Decision (Shagg: "copy what that was"): takeover no longer sets --gow2_renderer_queries=answer; the default is off, so
the game sees the fake "visible" count for every query exactly as in job 139 run 2 (off and shadow write nothing; shadow
only adds our host queries for the log). Cost: ch10 ~13 % slower (draws/frame ~4,900 vs ~4,300). Our answers come back
as a speed item once our results land within about a frame. The hidden_after rule (default 3) stays for that work.
Job 141: ch10 150 s on ours with the new default, watched.

## 8. Job 141 RESULT and 6C (job 142)

**Job 141 (watched by Shagg): no flicker** with the new default (no occlusion answers). A few distant textures are
still blurry for a while (separate item: check our texture cache picks up each new mip promptly; ch10's ~20 fps also
slows the game's own streaming). Job 140 numbers for the record: wrongly hidden per 300 frames 462-630 with the
"3 zeros" rule vs 1,237-1,698 with the old rule (both flicker; the cause is our ~3-frame result latency).

**6C:** `--gow2_gpu` defaults to ours. The game exe never linked the SDK's GPU code (it is the `xenos` plugin DLL,
loaded only through --gpu_plugin when no graphics system was given); with ours it is never loaded (job 137: no plugin
`[gpu]` lines, e.g. its shader storage). The plugin stays in developer builds as the reference (`--gow2_gpu=sdk`, D1/
D32); SDK-dependent dev tools (resolve/query logs, shadow comparisons) need --gow2_gpu=sdk now.
Pass for job 142: plain runs (no renderer flags): ch02 80 s and the menu 60 s on the default: our graphics system
created with the preset, no occlusion answers, 0 errors, screen checks exact, ~2,100 / ~1,500 frames.

## 9. 6C RESULT (job 142), normal-run smoothness (job 143), sharpening check (job 144)

**6C passed (job 142):** plain runs, no renderer flags: ch02 2,100 frames, menu 1,500, 0 errors, 7/7 and 5/5 screen
checks exact, our graphics system created by default with the preset, no occlusion answers. One ~55-60 ms frame per
300-frame window = the screen check's own stall. **Stage 6 is complete.**

**Job 143 (watched):** normal runs, no test tools. Shagg: ch02 "looks just right for 30 fps"; ch10 under 30 fps
("not bad, a little slow") - the known CPU cost of our renderer there; Shagg: optimisation, later.
Some distant textures take 10-15 s to reach full sharpness.

**Job 144 (one ch10 run, watched):** does our texture cache hold sharpening back? File reads into guest memory already
notify our watch (XFile::Read triggers the physical-memory callbacks), so the remaining suspects are the cache's own
shortcuts: the unchanged-bytes check (`--gow2_renderer_texture_always_reload=true` skips it) and the 512 MB host cap
(`--gow2_renderer_texture_cache_mb=0`: no eviction). Both off. If textures still take 10-15 s, it is the game's
streaming at ch10's frame rate; if clearly faster, our cache was the cause.

## 10. Job 146 (sharpening A/B) and tessellation: the chapter 18 water (job 148)

**Job 146 (watched):** A (normal cache) and B (shortcuts off) looked exactly the same: the same few distant textures
take up to ~1 minute to sharpen in both. Our cache is not the cause (job 144's "faster" was the warm file cache).
Left for the speed work: the game's streaming pace at our frame rate (same few textures, <5-10 % of the screen).

**Job 147:** ch18 and ch20, emulator (reference) vs ours. Ours skips exactly one tessellated draw per frame in both
(300 per 300 frames). In ch18 it is the water surface: missing in ours (a bright void where the emulator draws dark
water; Shagg: "a deep hole"). (The emulator showed the ch18 chapter title in rainbow colours; ours didn't.)

**Prior work (Gears2-Recomp, jobs 120-127, and its research report in this chat's history):** the water is UE3's
FluidSurface, drawn by XDK DrawTessellatedPrimitive (0x82980F70): DRAW_INDX auto-indexed, prim 0x12 (quad patch),
explicit major mode, count = patch count (15,876 = 126 x 126), VGT_INDX_OFFSET = first patch, no index buffer;
VGT_HOS_CNTL mode 1 (continuous), MAX_TESS_LEVEL 3 -> edge factor 4. The vertex shader (3AAB2C89B66705F3) has no
vfetch: it reads r0.x (patch index), r0.y/z (u, v), r1.x (must be 0) and samples the ripple height texture (fetch 16).
Xenia feeds a domain shader exactly those (dxbc_shader_translator.cc 602-639). The old project drew a uniform grid of
patch_count * 6 * N^2 vertices with the shader building (patch, u, v) from SV_VertexID - equivalent to the hardware
when every edge factor is the same and even (continuous, factor 4 here); it defaulted to twice the console's detail.

**Ours (job 148):** `HlslOptions::tess_quad_segments` (the vertex shader variant's prologue: r0 = (patch + VGT_INDX_OFFSET
masked to 24 bits, u, v, 0), r1.x = 0; corners (0,0)(1,0)(0,1)(1,0)(1,1)(0,1); u = i/N exact at the edges);
`ShaderCache::Get(..., tess_quad_segments)` (own key and file name); the renderer draws prim 0x12 auto-indexed patches
in discrete/continuous mode as that grid (N = MAX+1, next even for continuous, x `--gow2_renderer_tess_scale` = 1 by
default: the console's level), cull off on those draws (`--gow2_renderer_tess_cull`); adaptive mode and indexed patch
lists are counted as "not drawn". Checked here: the water shader's variant compiles with DXC (vs_6_6) and reads
xe_tex_2d_16; the grid covers each patch exactly once with one winding (script check).
Pass for job 148 (ch18 80 s, emulator vs ours, watched): ours draws the water (tessellated = patches, not drawn 0;
the first-draw log line shows mode 1, level 3, 4 segments); the water area in our frame-900 shot looks like the
emulator's; 0 errors.

## 11. Job 148 RESULT (the water works) and the ch18 title (job 150)

**Job 148 passed:** ch18 on ours: first quad-patch draw logged as 15,876 patches, mode 1 (continuous), max/min level
3.0 -> 4 segments per edge (1,524,096 vertices), VS 3AAB2C89B66705F3; every window "tessellated 300, not drawn 0".
Frame 900: the water surface is there where the emulator draws it (Shagg: "the water looks perfect").

**The chapter title** (Shagg): in ch18, ours and the emulator alike, "GATHERING STORM / BRACKISH WATERS" draws right
for about a second, then turns into a rainbow mess. Job 149 (ours, water on / off, a shot every 30 frames): the same with
the water off. Frames 510-540 show the title correctly; frame 570 shows a pink/white block pattern of the title's size
in its place - the title texture's bytes were overwritten, not a shading error. Shagg recalls the title being fine in
the Gears2-Recomp project (which used the SDK's resolves). Suspect: one of our (and the SDK's) resolves writes more than
the hardware would, over the texture next to it; or a CPU write.
Job 150 diagnostic (`--gow2_renderer_texture_stale_log=60`): every texture that stayed unchanged for 60 frames and then
gets new bytes is logged, with whether its pages were last written by our resolves and the resolves (frame, extent,
base, format, size) from our last 256 that overlap it.
**Job 150 result:** at frame 558 (the title breaks between shots 540 and 570) three textures stable until then got new
bytes at once: 1C84E000+20000 (fmt 18 = DXT1, 512x512, since frame 316), 168E9000+40000 and 168A9000+40000 (fmt 20 =
DXT4/5, 512x512; the latter loaded at frame 4 - very likely the HUD/title font). Pages by our resolves: no; no
overlapping resolve in our last 256 - the bytes came from the CPU side (game code or a file read), not from our GPU
work. Both renderers then draw the new bytes faithfully. Suspect: a load/streaming write landing in memory that still
holds the font while the title shows (on the console's slow disc it would land after the title has faded).
Job 151 diagnostic (`--gow2_renderer_watch_report=168A9000:40000;168E9000:40000;1C84E000:30000`): logs every
memory-system write callback into those ranges: exact range (host-side, e.g. a file read) or page (CPU store), the
swap count and the guest thread.
**Job 151 result:** one memory-system callback at swap 560, guest thread F: `16810000+8C00000` (140 MB), exact range
(host side: in this SDK only a file read (`XFile::Read`), `PhysicalHeap::Release`, `Decommit` or `Protect` to writable
fire one) - it covers the font (168A9000) and every texture reloaded in jobs 150/151 (also at 810, 932, 975). Our
cache's page hashes confirm the bytes really changed. 140 MB at once looks like a free of a big region, not a read [I].
The user saw the text last ~3 s instead of ~1 s this run: timing-dependent [V by eye, one run].
Job 152 diagnostic: the watch report also logs the host call stack (DbgHelp names from the PDB, else module+offset),
symbols loaded when the report is set up, not inside the callback (which holds the global lock).

## 12. The ch18 title: the texture pool defragmenter (jobs 152-153)

**Job 152 result [V]:** the 140 MB callback is `MmSetAddressProtect` (writable) on the whole texture pool, from
`8280D848` (pool defrag) < `8280DB18` (pool allocator, on a failed allocation) < `8280C490` < `8280C570` (cube texture
create) < `8235A4A8` (streaming/loading) on guest thread F, at swap 557. The pool is 140 MB (`16810000-1F410000`, ini
PoolSize 140). After the defrag, the title's font texture at 168A9000 (still bound by its draws) holds other data. Same
in our renderer and the emulator's, so it is the game's memory, not drawing. Why the font keeps the old address after
the move is not known [?]; why the console doesn't show it is a guess: its slow disc fills the pool later, after the
title (the text lasting ~1 s vs ~3 s between runs says timing matters) [I].
**Canary's answer** (game-patches `4D53082D - Gears of War 2.patch.toml`, "Texture Streaming Fix", boma, off by default
there): PoolSize 196 at `8280D534`, and RHIGetTextureMemoryStats `8280DDC8` -> return FALSE (streaming budget never
scaled down). Ported as two switches, off by default: `--gow2_patch_texture_pool_mb=196`,
`--gow2_patch_texture_stats_off`; plus `--gow2_log_texture_defrag` (entry of 8280D848, with the swap).
**Job 153 pass criteria (written before the run):** (1) ch18 with both: no defrag before swap 700 and the title text
intact in the shots at 540-660; (2) ch18 control (both off, log on): a defrag near swap ~557 (confirms the log);
(3) ch10 80 s off, then ch10 80 s on, then ch02 80 s on: no crash, no error/allocation-failure lines that the off
run lacks, ch10 frame time within ~10 % of the off run, ch02 still ~33 ms (job 143), shots look normal. Only if all pass does it become the
default (D32: better than the console's behaviour on PC, behind a switch).
**Job 153 result: all pass [V].** (1) ch18 on: no defrag until swap 805 (after the title), title intact in the shots and
by eye (Shagg). (2) ch18 off: defrag at swap 557, title broke after ~2 s (Shagg). (3) ch10 off 57.2 ms vs on 58.0 ms
(+1.4 %); ch02 on 33.3 ms, p99 36.5; no crash; the only error line in all five runs is the usual `cache:\` one.
Bonus [V by eye]: ch10 on had every texture sharp from the start (off: the same few slow ones, up to ~1 min) - the
slow sharpening was the streamer scaling its budget down for a full 140 MB pool. Watch item: ch18 on had a burst of 36
defrags at swaps 805-~810 and one slow frame (max 204 ms in swaps 601-900); shots after it look normal.
**Job 154:** both switches on by default (`--gow2_patch_texture_pool_mb=0` / `--gow2_patch_texture_stats_off=false`
give the game's own behaviour back).
From the Gears 3 port (2026-10-22): Gears 3's pool set-up reads `-POOLSIZE=` and `-REDUCEPOOLSIZE=` from the launch line
instead of the ini's PoolSize; Gears 2's has no such check, which is why we patch 0x8280D534.

## 13. Stage 6 sign-off: the full chapter sweep (job 155)

The roadmap's Stage 6 pass criterion (notes/28 section 2) is a full sweep of all 29 chapters with the emulator absent;
so far ours has run ch02, ch10, ch18, ch20 and the menu. Job 155: every chapter index 0-28 once, 60 s, on the default
build (our graphics system, texture fix on), scripted A presses (job 048's), frame times, defrag log, a shot every 300
frames. **Pass (written before the run):** no crash or hang in any chapter (each run reaches its time limit and exits
normally); no error lines beyond the usual `cache:\` one; every chapter's shots show the scene, no garbage or missing
geometry/text (my review of every shot, plus whatever Shagg watches); frame times reported for every chapter. Anything
odd gets one targeted follow-up, then the roadmap's other Stage 6 items (movies, menus, cutscenes, co-op/MP/Horde,
window behaviour) - still the base, not Stage 7.
**Option for later (Shagg 16:46: not now):** an emulator comparison (`--gow2_gpu=sdk` run + FLIP on matching shots) is
done per chapter, only for chapters where the sweep or a play-test shows a distinct bug - not a second full sweep.
**Speed references (Shagg 16:51):** Canary/Edge (with its Unlock FPS patch) and "Gears of War 2 Hollow" (a fan-made PC
port of the unreleased Gears 2 PC game) both reach very high frame rates on Shagg's PC. For us: proof the per-frame
work fits in a few ms and that the game logic mostly copes above 30 fps. Hollow (xHeliosx, ModDB/Nexus, July 2026) is built
from a leaked early-2008 developer build with Windows binaries - a different, unfinished codebase, not the retail 360
game; it shows 4K at ~180-200 fps and has lighting glitches the press blames on the build. A frame-rate / feel reference
only (not inspected; no code or assets from it - leaked build).

## 14. The Hive water (sweep ch20, Shagg 17:04)
Shagg: early in the chapter before Hive "Answers" (sweep ch20, chapter=19) the water surface is missing and some
textures where it should be flicker. Guess [I]: a tessellation case we count but don't draw (triangle patches,
indexed patches or adaptive mode - shadow_renderer.cpp skips them). Job 157: each kind of undrawn patch draw is logged
once (prim, mode, indexed, count, levels, shaders); ch20 60 s with shots every 60 frames and the renderer summary.
**Correction (sweep logs):** sweep ch20 is "Hive: Answers" (its one quad-patch water draw per frame is drawn); the
chapter Shagg means is ch19 "Hive: Priorities" (chapter=18). Its sweep log has **no patch draws at all**; its shots
show the water as flat white/grey. The log's only texture stand-in reason there (and in ch20): "cube fetch of a non-cube
texture", 1 per frame - our stand-in is opaque white, while Canary binds a null view (reads 0) when the shader's
fetch dimension doesn't match the texture (texture_cache.cc AreDimensionsCompatible). Lead [I]: the water shader's
reflection fetch. New switch `--gow2_renderer_texture_mismatch_zero` (default true, Canary's behaviour): such fetches
read zero. Job 157 now: ch19 40 s with it on and 40 s off, shots every 60 frames, plus the tessellation logs.
**Job 159 result [V, shots]:** ch19 with `texture_mismatch_zero` on: the flat white water is gone, the water area reads as
a dark surface; off: white as before. Same frame times (33.3 ms). Whether it now matches the console needs Shagg's
eyes (and, if in doubt, the per-chapter Canary comparison - notes/51 section 13).
