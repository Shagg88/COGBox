# Speed plan (Shagg 16:56, 2026-10-21: start right after the Stage 6 sweep; "use as many cores as possible")

Order agreed in chat (16:50): extras worked in turns - speed, 4K / any resolution, AA, HDR, frame-rate unlock (game
logic first, light scenes), ultrawide. Speed comes first because high frame rates in heavy chapters need it. The rest of
Stage 6 (movies, menus, cutscenes, co-op/MP/Horde, window behaviour) still has to be signed off; it is fitted in between.

## 0. What we know [V]
- ch10: swap interval ~57 ms (~17.5 fps); our renderer's work on the GPU worker thread ~50 ms/frame ("observer
  callbacks", job 153). ch02: ~18-24 ms of 33 ms. So heavy scenes are capped by **one thread of ours**.
- Guest threads are not pinned: `ignore_thread_affinities` defaults to true in the SDK (xthread.cpp), so the game's
  threads already float over every host core. The game itself is as parallel as it was on the 360 (6 hardware threads).
- Canary/Edge and the fan port "Hollow" run Gears 2 far above 30 fps on this PC (notes/51): the work fits in a few ms.

## 1. Measure first: a built-in sampling profiler (job 156)
`--gow2_profile=S` (layer 0, dev tool, off by default): for S seconds after the first gameplay frames, a sampler thread
wakes every 1 ms, suspends each process thread in turn, reads its instruction pointer and unwinds up to 24 frames
(RtlLookupFunctionEntry/RtlVirtualUnwind, no allocation while the target is suspended), resumes it. Also per-thread CPU
time (QueryThreadCycleTime / GetThreadTimes) at start and end. At the end: symbols via DbgHelp (as the watch report),
then per thread, by CPU share: thread name, CPU %, top 25 functions by own samples and top 25 by inclusive samples.
Written to `profile.txt` in the run folder; summary in the log.
Answers: how busy each core is; what in our renderer costs the 50 ms; how much is the game's own code vs the runtime
(locks, timers, memory callbacks).
**Pass for job 156:** ch10 80 s, profile 30 s: the profile file exists with the GPU worker thread at the top and
symbol names (not raw addresses); the sampler's own cost < 5 % (frame time vs a run without it); no hang/crash.

## 2. Then, biggest cost first (each behind a switch, A/B measured like job 153)
Candidates, to be ranked by the profile [I]:
1. **Parallel recording**: the worker thread parses the ring and translates state; draws are recorded into D3D12
   command lists on worker threads (one list per chunk of draws), submitted in order. D3D12 is built for this.
2. **Caching**: pipeline state, root signature / descriptor tables, sampler and texture lookups keyed by what
   actually changed between draws; skip redundant state.
3. **Texture checks**: no rehash for textures nobody wrote; the defrag / unlock / mip-realloc hooks tell us when the
   game changes texture memory.
4. **Test overhead off in normal play**: anything still running by default that only serves tests.
5. **Game code / runtime** (if the profile shows it): compiler flags (LTO, -O3, AVX2 for vector code), lock and timer
   overhead in the runtime.
6. **Smoothness**: shader / pipeline warm-up at boot and loading (notes/28 section 10), hitch hunting (ch18 swap ~805
   defrag burst), frame pacing.
Target: ch10 renderer work from ~50 ms to under 10 ms per frame; then the frame-rate unlock work can show >30 fps
in heavy chapters.

## 3. Idea parked (Shagg 16:58): optional load pacing
If more "PC loads too fast for the game" bugs turn up (like the ch18 title, notes/51 section 12), a switch could pace
file reads (e.g. cap read throughput or add per-read latency near the 360 DVD's) so the game's streaming runs in console
order. Also a diagnostic: if a bug disappears with pacing on, it is a timing race. Not built; the texture pool fix
covers the one case seen so far.

## 4. Job 156 (queued after the sweep)
Build + tests + commit (profiler.cpp, wiring, notes/51 + 52). Then ch10 60 s without the profiler and ch10 80 s with
`--gow2_profile=30 --gow2_profile_start=900`, both with frame times. Pass as section 1.

## 5. Job 156 result [V] and the first fixes (job 158)
ch10, 30 s profiled from swap 900: process CPU 2.25 cores of 16. Two threads at a full core each:
- **GPU Commands (ours)** 98.7%: 86% inclusive in our draw path. Own time: ReadIndices 11.4% (per-index switch),
  the NVIDIA driver ~30% (names like OpenAdapter10 / NVAPI_* / D3D12GetInterface are the nearest exports, not real
  functions), **NtGdiDdDDIGetDeviceState 11%** (GetDeviceRemovedReason, called through IsLost() on every draw and every
  ZPD), memmove 8% (vertex uploads), Retire 5.2% (ID3D12Fence::GetCompletedValue on every ring allocation),
  RtlQueryPerformanceCounter 4.8% (steady_clock in our per-draw part timers), heap alloc/free 5.5%.
- **The game's render thread (XThread42AC = guest thread F)** 99.2%: 43% own time in `829783A8` (D3D query GetData,
  notes/46) plus 25% in its register save/restore helpers = it spins polling occlusion queries until our GPU thread
  has processed the ring up to them. So it waits for us: **our GPU thread is the bottleneck**.
- Everything else is idle (Main XThread 8%, timer queue 7%). Sampler cost: frame time 56.6-56.9 ms with it vs
  56.7-57.3 ms without (no visible cost).
Job 158 fixes (no behaviour change intended): IsLost() asks the device at most every 50 ms; Retire polls the fence only
when an allocation would otherwise wait (stale value is conservative); part / per-draw timers use rdtsc; the index
conversion has its branches hoisted out of the loops (brute-force checked identical to the old loop on 20,000 random
cases, all 4 endian modes, 16/32-bit, restart on/off).
**Pass for job 158 (written before):** build + tests; ch10 60 s with the profiler: no crash, GPU-thread "our CPU" ms per
frame and the swap interval below job 156's (49.4-49.9 / 56.6-57.3 ms); shots look like the sweep's ch10; ch02 60 s:
still 33.3 ms and normal shots.

## 6. Queue lessons (jobs 157-159)
- The queue driver runs a script name once: a re-uploaded `157-*.ps1` went straight to `done` without running. Fixes
  get a **new job number**, never a re-upload under the old name.
- `Copy-Item` keeps the upload time on the copied sources. If another job built after the upload, ninja sees the
  objects as newer and skips the files (job 157's first run compiled in 1 s with the old renderer). Every job script
  now sets the copied files' LastWriteTime to the time of the copy before building.

## 7. Job 159 result [V] and direct vertex reads (job 160)
ch10 after the job 158 fixes: swap 55.5-56.0 ms (was 56.6-57.3), our CPU 48 ms/frame, but **15 ms of it blocked on our
own GPU** (upload ring 198 waits = 13.7 ms/frame; sampler tables 1.7 ms): the CPU is ahead and the GPU is behind by the
whole 256 MB upload ring. Per 300 frames: vertex 12.9 GB (43 MB/frame), index 4.7 GB, ~4,900 drawn draws per frame.
Cause [I, strong]: every small vertex range is copied into the guest mirror buffer per draw - CopyBufferRegion with a
COPY_DEST barrier before and an SRV barrier after, on the one buffer every draw reads: the GPU drains between draws.
Fix: `--gow2_renderer_vertex_direct` (default on): small (unwatched) vertex ranges go into the upload ring only and the
shader reads them there (root SRV t97); the renderer rewrites that fetch constant for the draw (address = ring offset,
dword 1 bit 31 = "ring", a bit the hardware ignores; cleared on every other used vertex fetch constant). Large watched
ranges keep the mirror. All vertex shaders recompile once (prelude changed; checked with DXC).
**Pass for job 160 (written before):** build + tests; ch10 on (warm-up), ch10 off, ch10 on again: no crash, no rejected
shaders, "blocked on our GPU" down sharply and the swap interval below 55 ms with it on vs off; ch02 still 33.3 ms with
normal shots; ch19 shots as job 159's.
**Job 160 result: pass [V].** ch10 direct on: 33.3 ms (the 30 fps cap; p99 36 ms) vs off 55.0 ms; our CPU 21 ms/frame
(was 48); GPU waits 0 (off: upload ring 180 waits = 12.9 ms/frame). Shots: ch10 frame 600 byte-identical on vs off;
ch02 frame 1200 byte-identical to the sweep's. No rejected shaders, no errors; ch02 / ch19 33.3 ms. Left in ch10 (ms per
frame): record 5.1, ownership 4.3, vertex 2.8, textures 2.1, constants 1.9, state 1.0, index 0.9, transfers 0.9.

## 8. Job 161 profile (after job 160) and the uncapped measurement (job 162)
ch10, 20 s from swap 901: our GPU thread 86.6% of a core, of which ~12% is sleep_for (waiting for the ring: the game is
at its 30 fps cap now). Own time: memmove 16.6% (vertex 43 MB + index 16 MB per frame into the upload ring), heap
alloc/free ~8% (per-draw vectors: BuildRuns etc.), OnDraw 5.5%, PM4 parsing ~10%, MarkWritten 2.8%. The game's render
thread is 98.8% (spinning on query polls / the swap wait, as before). Nothing else is busy.
So in ch10 we are no longer the limit at 30 fps. Job 162: `--gow2_gpu_vsync=false` (vblank at 1000 Hz) on ch02 and
ch10, measurement only: how fast does it run uncapped, what limits it then, and does anything look or play wrong.
**Job 162 result [V]:** uncapped (vsync off, -BENCHMARK fixed step): **ch02 12.8 ms/frame (~78 fps), ch10 27.3 ms
(~37 fps)**, no crash, no errors. Our GPU thread is at 98.7% in both = the limit. Own time, ch02: heap alloc 13.1% +
free 6.9%, memmove 8.9%, RenderTargetCache::MarkWritten 8.5%, BuildRuns 4.7%, TilePixels 4.1% (the EDRAM ownership
books ~40% with the allocations); ch10: memmove 19%, OnDraw 7.1%, heap ~10%, PM4 parsing ~13%.
Job 163: the profiler also lists the callers of the hottest own-time functions (who allocates / copies); ch02 uncapped.

## 9. Job 164 (callers) and the BuildRuns scratch (job 165)
ch02 uncapped, our GPU thread: heap alloc 11.8% + free 6.4% + memmove 2.8% are all called from
`ShadowRenderer::BuildRuns` (EDRAM tile runs to transfer, per draw from PrepareDepthTarget / PrepareColourTarget), plus
its own 4.4%: ~25% of the thread. Next: MarkWritten 6.5% (from PrepareDepthTarget), NVIDIA driver ~4%.
Job 165: BuildRuns fills scratch kept between draws (runs and their member lists reuse their memory; same output).
**Pass (written before):** build + tests; ch02 and ch10 uncapped: no crash; frame time below job 162's (12.8 / 27.3 ms);
shots at the same frame numbers byte-identical to job 162's (fixed time step), otherwise visually the same.
**Job 165 result [V]:** uncapped ch02 12.8 -> **10.1 ms (~99 fps)**, ch10 27.3 -> **25.1 ms (~40 fps)**. ch02 shots 7/7
byte-identical to job 162. ch10: 1 identical, 4 differ - only on moving objects (Marcus' armour, a distant flyer;
scenery pixel-identical): animation timing varies between uncapped runs in ch10 [I, from the diff images].
Next (job 166): `RenderTargetCache::MarkWritten` remembers the last FindById (a linear search over all targets, per
tile changing hands); the translator records how many float constants a shader can read (1 + highest index, 256 if any
read is relative) and only those are compared / uploaded (`--gow2_renderer_float_constants_trim`, default on): ch10's
memmove from SetConstants was 5.8%.
**Pass for job 166 (written before):** build + tests; ch02 and ch10 uncapped faster than job 165; ch02 uncapped shots
byte-identical to job 162's; ch10 capped (vsync on) shots byte-identical to job 160's ch10-on (deterministic there).
**Job 166 result [V]:** no crash / errors; ch02 uncapped shots 7/7 identical to job 162; speed within noise (ch02 ~10.5,
ch10 ~25.9 ms uncapped): the constant trim does not pay (most of SetConstants' cost is real changes between draws);
kept on (harmless). ch10 capped frame 600 differed from job 160's: only character poses (Dom, Marcus, a soldier, the
drone); scenery identical, characters render correctly - ch10 animation is not exactly repeatable even capped [V].

## 10. Threaded command recording (job 167)
`--gow2_renderer_threaded_recording` (default off until it passes): every command-list call in d3d12_backend.cpp (49
call sites) goes through `CommandListProxy` (same method names). Off: forwards at once (as before). On: the call is
encoded into 32 KB blocks that a recording thread ("GPU Recording (ours)") replays onto the real list; Close, Reset
(allocator + list) and Execute+Signal are ops too, so the real list, allocators and queue submissions live on that
thread. Submit drains (waits for the thread to have submitted), so presentation and fence waits see the same order.
Everything else (state tracking, ring allocation, descriptor writes, CPU copies) stays on the GPU worker thread.
**Pass (written before):** build + tests; ch10 uncapped off vs on, ch02 uncapped on, ch10 capped on: no crash, no
errors, no device loss; ch02 uncapped shots byte-identical to job 162's; uncapped frame time lower with it on.
**Job 167 result [V]:** threaded recording on: ch10 uncapped 24.7 -> 23.4 ms (-5%), ch02 ~10.1 -> 9.65 ms; no crash, no
errors, no device loss. The recording thread uses only 8.6% of a core: the driver's recording work was small, most of
the cost is our translation on the GPU worker thread. ch02 shots differed from job 162's (6 of 7) - only moving things
(the swaying hanging sign, the characters, a lamp): thread timing shifts real-time physics/animation [V, diff images];
nothing mis-drawn. Kept **off by default** (small gain; a new thread wants more mileage first).

## 11. PM4 constant writes (job 168)
Every register write went Processor::WriteRegister -> std::function (gamma tracker) -> std::function (our graphics
system's WriteRegister: volatile store + scratch/COHER checks) -> plus the processor's own copy and change list. In ch10
the two lambdas + WriteRegister + Packet were ~18% of the GPU thread; most writes are the constant files (float constants
for every draw). `--gow2_pm4_fast_constants` (default on): writes to 0x4000-0x4927 (ALU / fetch / bool / loop constants,
no side effects on the host) go straight into the host register file; the change list (read only by the shadow
comparison) is skipped in execute mode.
**Pass (written before):** build + tests; ch02 / ch10 uncapped faster than job 166 (10.5 / 25.9 ms); ch02 uncapped shots
byte-identical to job 162's; no crash, no errors.
**Job 168 result: pass [V].** ch02 uncapped ~9.9 ms (shots 7/7 identical to job 162), ch10 ~23.4 ms (was 25-26; one shot
differs on moving characters as before); no crash, no errors. ch10 GPU thread now: memmove 21.4% (UploadTransient 8.2,
SetConstants 5.3, PrepareDraw's constant copy 3.6, DrawIndexed 2.5), PM4 Packet 9.1%, OnDraw 7.7%, MarkWritten 4.1%,
ReadIndices 3.0%, NVIDIA driver ~5%, RtlQueryPerformanceCounter 2.2% (the backend's per-draw timers).
Job 169: the backend's PrepareDraw / ApplyTextures / UploadConstants timers use rdtsc; the ring reader's per-dword
`% dwords` becomes a compare (pm4 tests cover it). Same pass rule as job 168.
**Job 169 result: pass [V].** ch10 uncapped **22.3 ms (~45 fps)** (was 23.4); ch02 ~10.1 ms (same); no crash / errors.
ch02 shots differed from job 162's (6 of 7) - only the swinging sign, Marcus, Dom and a lamp; and frame 1200 is
**byte-identical to job 167's** (threaded recording run): uncapped ch02 has at least two repeatable variants, chosen by
small timing shifts (real-time physics). Shot checks now compare against every known variant (162, 167/169).

## 12. Vertex upload dedupe (job 170, measurement first)
`--gow2_renderer_vertex_dedupe` (default off): a direct vertex range uploaded again with the same bytes (XXH3) in the
same command list reuses its ring copy. The summary now prints vertex MB (direct, reused). Job 170: ch10 uncapped
dedupe on; dedupe on + threaded recording on; ch02 dedupe on. Default only if it is faster and shots match a variant.
**Job 170 result [V]: no gain - stays off.** 85% of the direct vertex bytes were repeats (ch10: 10.7 of 12.6 GB), but
hashing them cost as much as copying (ch10 profile: memmove 23.6% -> 13.4%, XXH3 +12.6%; vertex part 4.2 ms either
way). Both read the same guest bytes, and reading them is the cost: ~1.2 GB/s, far below a cached memcpy. Checked
and ruled out [V]: the SDK maps guest pages as plain read/write host memory whatever the title's NOCACHE /
WRITECOMBINE flags (ToPageAccess) - not uncached memory. Likely [G]: the title's CPU threads just wrote these bytes,
so they come from other cores' caches line by line. Several cores reading at once overlap those waits.

## 13. Copy threads (job 171)
`--gow2_renderer_copy_threads=N` (default 0 = inline): UploadGuestMemory / UploadTransient reserve ring space and
queue the copy; N workers do them (>= 1 KB; split at 64 KB; batches of 96 KB / 48 copies). Every deferred copy is
finished (the GPU thread helps) at: each Submit (before the list runs), and each PM4 *release* point - memory
writes, EVENT_WRITE_EXT, scratch register writes (may be written back), packets handed back (interrupts), the end of
each ring kick - plus our display write. Those are how the title learns the GPU got further, so it never reuses a
buffer while a worker still reads it (same rule as the real GPU, which reads even later). PM4 summary counts
releases; renderer summary counts deferred MB / copies / flushes. Unit-tested off the build (8-thread stress under
ThreadSanitizer, random sizes and flushes). Job 171: ch10 uncapped 0 / 4 / 4+recording thread / 8+recording thread,
ch10 capped (deterministic - shots must match 166/167 capped), ch02 uncapped.
**Job 171 result [V]: copy threads gave nothing yet; recording thread ~7%.** ch10 uncapped: copy 0 22.6 ms, copy 4
22.9, copy 4 + recording thread 21.1, copy 8 + recording thread 21.1; ch02 copy 4 + recording 9.4-9.7 (was ~10).
The vertex part fell 2.8 -> 1.9 ms but "record" rose 4.5 -> 5.2 (waiting at flushes lands there), and waking a worker
per batch (condition variable signal) cost the GPU thread 4.8%. Flushes: ~28 per frame; PM4 release points ~69 per
frame (every ~70 draws). ch10 is ~5000 draws a frame (the summary counts are per 300-frame window, not cumulative -
I had misread them): ~3.5 us per draw, so there is no single big waste left - the wins now are parallel work and
trimming per-draw passes. Capped ch10 shot 600 matched no earlier capped run (shot 1200 did) - checked next job
against a capped run without copy threads.

## 14. Job 172: three trims + copy threads without wake-ups
1. Copy workers spin ~100 us for work before sleeping and are only signalled when asleep; Flush spins before
   sleeping too. (TSan stress test again clean.)
2. SetConstants: one copy pass (was zero-fill of the whole slot + copy).
3. PM4 IM_LOAD cache: ~2800 shader loads a frame in ch10, almost all the same bytes again. Keyed by (address,
   size) with the guest bytes; unchanged bytes (memcmp) reuse the decoded ShaderRef - no byte-swap copy, allocation
   or hash. New unit test (reuse, then rewritten in place -> new hash). PM4 summary shows "(cached N)".
Runs: ch10 uncapped recording thread with copy 0 / 4 / 8; ch10 capped recording thread copy 0 and copy 4
(determinism); ch02 uncapped recording thread copy 4.
**Job 172 result [V]:** ch10 uncapped (recording thread) **20.0 ms** (was 21.1): the shader-load cache hits 97%
(7.32 of 7.54 M loads) and the one-pass constants trim the constants part 1.8 -> 1.5 ms. Copy threads still a
little worse even without wake-ups (20.4 ms with 4 or 8) - **they stay off**; reading guest bytes on another core
does not beat reading them here when the GPU thread must wait for them ~70 times a frame. Capped determinism: ch10
capped with copy 4 matched the same build's copy 0 run on all 7 shots; across builds, capped shots 900-1800 differ
from job 171's only on Marcus and a flying creature (diff image: 0.3% of pixels, animation), scenery identical.

## 15. The PM4 / renderer split (job 173)
`--gow2_pm4_split` (default off; needs own draws). The GPU thread parses the ring and queues items; a new "GPU
Render (ours)" thread (highest priority) runs them in order with its own register file copy:
- draw (initiator, index base/size, the two shader pointers - cheap now with the job 172 cache) and the register
  changes since the previous item (TakeChangedIndices; values read from the live file at queue time);
- ZPD dispatch (the report is still written at parse time, as before); swap (+ a drain: the GPU thread waits until
  presented, then the swap counter); deferred guest writes (MEM_WRITE / EVENT_WRITE_SHD fences, EVENT_WRITE_EXT, the
  ring read pointer) - the renderer finishes its copies, then writes;
- drains: the GPU thread waits for the renderer to run everything queued. Drain points: interrupts, scratch
  register writes (written back to memory by our graphics system), a read of an address with a pending deferred
  write, the first failure of any WAIT_REG_MEM. Swaps and drains also refresh the renderer's whole register copy from
  the live one (the GPU thread is waiting then) - so registers only the title's CPU writes reach it each frame.
Why deferring the writes is right: the title learns "the GPU got here" only from those writes (and the read
pointer), and a real GPU makes them when it gets there, after the draws before them - so the title never reuses a
buffer the renderer has not read yet. Tested off the build: processor unit test (fence queued, wait on it releases
first, failing wait releases once) and a ThreadSanitizer stress test of the pipe (300k draws with random writes,
drains and swaps: all in order, each draw saw exactly its own register values, no races).
Runs: ch10 uncapped split off / on (+ profile), ch10 capped split on (shots vs 172 capped), ch02 uncapped split on,
ch18 capped split on (vs 153-ch18-fix). All with the recording thread.
**Job 173 result [V]: the split hung after the first frame** (all 4 split runs: one swap, then nothing; no crash,
no errors; split-off ch10 20.6 ms as before). Cause [V by reading the code; fix in job 174]: deferred writes waited
in the producer's current block until it filled (24 items) or a drain - after a fence at the end of the ring with the
GPU thread idle, the title waited for a fence that sat unpublished. Fix: a deferred write publishes at once (fences
and the read pointer: ~70 a frame), so everything before it goes out too.

## 16. Job 174: split retry + vertex reuse by release epoch
`--gow2_renderer_vertex_reuse` (default off): a direct vertex range drawn again in the same command list *and* the
same release epoch reuses its ring copy with no read of the guest bytes (job 170 found 85% repeats, but hashing them
cost as much as copying). Release epochs end at every release point (fence write, read pointer, interrupt, scratch
write, failing wait) - the title cannot legally rewrite memory an unfinished draw reads before it has seen one of
those (a real GPU reads later still). Runs: ch10 uncapped split on (+profile), reuse on (split off), split + reuse;
ch10 capped split + reuse (vs 172 capped); ch02 uncapped split + reuse; ch18 capped split + reuse (vs 153). A run
with no frames prints the end of its log.
**Job 174 result [V]: the split works.** ch10 uncapped: split off 20.6 ms; split 18.3; vertex reuse alone 19.0;
**split + reuse 16.7 ms (~60 fps)**. Threads (ch10 profile, split without reuse): GPU Render (ours) 96%, GPU
Commands 41% (mostly waiting), recording thread 11%, the title's render thread 98% (its query spin), process 3.0
cores. Shots: ch10 capped split+reuse identical to 172 capped on all 7; ch02 uncapped identical on all 9; ch18
capped 3 of 4 identical to 153, frame 900 differs only on Marcus and an animated light shaft (diff image) [G: timing
variant]. Split counters (ch10, 3300 frames): drains 23k (~7/frame), deferred writes 203k (~62/frame), releases on
reads 188, on failing waits 99. Vertex reuse: 10.28 of 12.57 GB not copied (82%).

## 17. Job 175: defaults on + two trims + full sweep
Defaults now: `gow2_pm4_split`, `gow2_renderer_threaded_recording`, `gow2_renderer_vertex_reuse` = true.
Trims: ShaderCache::Poll returns at once when nothing is compiling (no lock per draw); per-slot vertex memo - a fetch
slot with the same fetch constant as at its last use in the same list and release epoch gets the same result
(no map / watch lookups; a list change mid-draw invalidates it). Runs: ch10 uncapped (profile), ch10 capped (vs 174
capped), ch02 uncapped; then all 29 chapters (60 s, capped, as job 155) into out\harness\speed-sweep, shots hashed
against the job 155 sweep.
**Job 175 first runs [V]:** ch10 uncapped **15.4 ms (~65 fps)**; poll part 0.9 -> 0.05 ms; vertex memo did little
(vertex part 2.0 -> 1.95: the remaining vertex cost is not the lookups). ch10 capped identical to 174 on all 7 shots.
ch02 uncapped 9.5 ms and now **GPU-bound**: our thread waits 2.7 ms/frame for the GPU (frame part 3.1 ms) with only
~6 ms of CPU work - an RTX 3080 Ti spending ~9 ms on a 720p 360 frame means our GPU work itself is heavy.
The sweep (couldn't be stopped once the job ran) continues; ch02/ch03 shots 600-1500 differ from job 155's -
diff images to check (timing variant vs change).

## 18. Job 176: GPU timestamps
Each command list gets a timestamp at its start and end (resolved into a readback buffer, read when its fence
passes); the CPU-split line now shows "GPU x ms/frame (n lists timed)". This is the GPU-side profile we have been
missing: if GPU ms ~ frame time, the GPU is the limit and the work goes into our GPU passes (EDRAM transfers,
resolves, shader cost); if well below, the CPU is. Runs: ch02 and ch10 uncapped, ch18 capped.
Research from the user's research chat (Canary e6cd2e4bc / Edge 8fb2772, both BSD-3; ideas only): neither fork records
on more than one thread; their wins are stall and redundancy removal. Taken up: constants uploaded only when needed
and shared where equal; EDRAM transfer cost (merge, NOT_EQUAL depth, stencil-ref export); all pipelines built up
front; GPU results copied to the CPU only when read (Edge). Sweep ch02/ch03 diffs vs job 155 checked: only moving
things (ch02: Marcus, Dom, sign, bot) and a cutscene camera a moment apart (ch03) [V diff images].
ch02's GPU wait is at the sampler-table site: NextFrame rotates 3 sampler heaps and waits for the one 3 frames back -
i.e. the CPU is 3 frames ahead and waits for the GPU: GPU-bound [V by code]. ch10 resolves-to-guest: 27 MB/frame.

## 19. Job 177: GPU time by category + shared constants
- Timestamps when GPU work of a new category starts (draws, EDRAM transfers, resolves, texture loads, frame/present)
  - set from the renderer's parts (record / transfer / resolve / frame) and the texture cache's load dispatch; the
  CPU-split line shows GPU ms/frame split by category.
- SetConstantsBoth: system, bool/loop and fetch constants are identical for the vertex and pixel shader - one compare
  and one upload, both root slots point at it (the pixel slot un-shares itself if set alone, e.g. transfer passes).
Runs: ch02 / ch10 uncapped, ch10 capped (shots vs 175), ch18 capped.

## 20. Job 178: index reuse + the title's GPU-poll spin
Shagg 20:12: keep working CPU/GPU within this architecture, then Stage 7 (no separate "hook the game's D3D calls"
stage).
- `--gow2_renderer_index_reuse` (default off for the A/B): small index ranges drawn again in the same command list and
  release epoch draw from their converted ring copy (DrawIndexedKeep / DrawIndexedAt) - no read, conversion or copy.
  ch10 converts ~12 MB of indices a frame; draws repeat (depth, shadow, colour passes) as vertices do (82%).
- `--gow2_patch_query_poll_yield` (default 0): the title's render thread spins on a D3D query poll (sub_829783A8, ~45%
  of a core in the call, 98% thread). Hook at its entry: 1 = 32 pauses per poll (frees the physical core's other
  hardware thread, which may be our renderer), 2 = give up the slice per poll. Semantics unchanged (the poll itself
  runs as before).
Runs: ch10 uncapped base / index reuse / + yield 1 / + yield 2; ch10 capped index reuse + yield 1 (shots vs 175);
ch02 uncapped index reuse + yield 1.

**Job 177 result [V]:** GPU time by category (ms/frame): ch02 GPU 9.30 = draws 1.60 + **EDRAM transfers 7.11** +
resolves 0.20 + texture loads 0.22 + frame 0.17; ch10 9.19 = 3.72 + **4.73** + 0.26 + 0.28 + 0.20; ch18 capped 11.41 =
1.89 + **8.08** + 0.50 + 0.76 + 0.18. ch02 is GPU-bound and 77% of its GPU time is EDRAM transfers. Startup log: "Pixel-
shader-specified stencil reference: no" (NVIDIA) -> every depth transfer = a stencil clear + 1 depth pass + 8 stencil
bit passes over the rectangle. Constants part 1.43 -> 1.20 ms (shared VS/PS). Capped ch10 shots differ from 175's on
characters only (diff images, frames 600/1200) - animation variants; scenery identical [V].

## 21. Job 179: transfer kinds + stencil bits
- GPU categories per transfer kind (colour same format / colour word / depth->depth / colour->depth) and Mpx/frame each.
- Stencil usage: bits the title can set to 1 (draw write masks x ops, REPLACE -> ref bits), bits set by clears/fills,
  bits it tests (read masks where the func is not ALWAYS).
- `--gow2_renderer_stencil_transfer_bits=1`: bit passes only for those bits (others stay 0 from the pre-clear). Exact
  for depth->depth (no write ever sets the other bits); colour->depth sources carry arbitrary low bytes, whose untested
  bits become 0 - never seen by a stencil test, visible only if depth words are reinterpreted as colour. Switch, must
  pass the capped screenshot checks. The transfer self-test always uses all bits.
Runs: ch02 uncapped off / on; ch10 uncapped on; ch10 capped on; ch18 capped off / on.
