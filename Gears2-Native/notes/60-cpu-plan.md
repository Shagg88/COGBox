# 60. CPU optimisation (high priority, user 2026-10-22 06:27)

At 1280x720 the GPU is no longer the limit (ch10 GPU ~8 ms of a ~15 ms frame at job 185); the CPU side is.

## Where we stand
- Job 185 (the end of the speed work, -BENCHMARK, vsync off): ch02 6.49 ms/frame, ch10 14.67 ms.
- Job 199 (normal clock, frame limit off): ch02 8.6-11 ms. Not the same kind of run, so not a regression by itself.
- [G] Every CPU part of the renderer looked ~1.5x slower from job 187 on with GPU work unchanged; job 192 found
  Windows Defender (~1.2 cores) and Search busy on the PC. Could be the PC, could be Stage 7 code that also runs at
  scale 1 (scaled-memory bookkeeping, GuestWatch scaled versions).

## Job 217 - pass criteria written first
- ch02 and ch10 uncapped, run exactly as job 185 (BENCHMARK, vsync off, the same input), with the sampling profiler
  (10 s from swap 900, top 25 per thread) and the CPU time of every other process during each run.
- Report: frame time vs job 185; whether another process used a large share of the CPU during the runs; the top
  functions per thread (game threads = recompiled sub_XXXXXXXX, our renderer / PM4 / recording threads).
- Then the plan: the 2-3 biggest items, each with a switch and an A/B job, stop when a step gives under 3%.

## Standing rule (user, 2026-10-22 06:33)
Every job's first purpose comes first; every game run in every job is also a CPU sample: the sampling profiler (10 s
from swap 600), the CPU time of every other process during the run, the renderer's CPU split - via
`tools/cpu-report.ps1` (full profile kept in jobs/queue/logs/<job>-<run>-profile.log). Each job's CPU findings go here.
Also (user, 2026-10-22 08:32): long jobs are fine, but each one tests several things - no 30-minute job for one
check. Anything that pays for a rebuild carries every other ready test with it.

## Desk findings (no PC time) [V from the code; gains are guesses until measured]
- The game's busiest thread (XThread5864, ~99% of a core in job 183's ch10 profile) spends 2.8% in
  `rex::perf::IncrementCounter`: every indirect call in the recompiled code (`REX_CALL_INDIRECT_FUNC`) bumps one shared
  atomic counter because `REXGLUE_ENABLE_PROFILING` is on in non-Release SDK configs (our RelWithDebInfo build). All
  game threads hit the same cache line. Nothing of ours reads that counter. Candidate 1: undefine it for the
  recompiled code.
- Neither the recompiled code nor ours is built for a newer CPU than x86-64 baseline (no `-march`): no SSSE3 `pshufb`
  for the VMX permutes, no SSE4.1, AVX2, FMA, BMI2 or MOVBE (load + byte swap in one instruction - every guest load and
  store is a byte swap). Candidate 2: `-march=x86-64-v3` (Haswell / Zen and newer), with `-ffp-contract=off` so no
  multiply-add gets fused that the PowerPC code kept separate. Needs the PC's CPU checked first.
- Every guest load and store goes through a `volatile` pointer (`REX_LOAD_*` / `REX_STORE_*` in the generated pch),
  which stops the compiler combining or reordering them. Candidate 3, riskier (spin-waits on guest memory rely on
  it): a switch to try without, with careful sweeps.
- Our renderer thread: `memmove` 15% and `MarkWritten` 6.6% of its time (job 183) - candidates for later.

## Desk measurement: one generated file compiled locally (clang -O2, gow2_recomp.78.cpp) [V for code size only]
| Build | Lines of assembly (whole file) | Hot function sub_8272DF48 (instructions) |
|---|---|---|
| As now (x86-64 baseline) | 91,202 | 10,763 |
| `-march=x86-64-v3` | 85,376 (-6%) | 9,157 (-15%) - MOVBE does load + swap in one |
| no `volatile` | - | 10,601 (-1.5%) - not worth the risk; dropped |
| v3 + stack accesses without the physical-view check | 78,598 (-14%) | 8,671 (-19%) |
- Every guest load/store does `cmp addr, 0xE0000000; setae; shl 12` (REX_PHYS_HOST_OFFSET: on Windows the
  0xE0000000+ physical views sit 4 KB higher). 35% of all loads/stores in the code are based on r1 (the stack pointer),
  which never points there. SDK patch 28 (codegen + pch template): r1-based loads/stores (D-form, with update, lfs/lfd,
  stfs/stfd) use REX_SLOAD_* / REX_SSTORE_* without the check.
- Fewer instructions is not the same as faster; job 218 measures it (same runs as job 217) and checks correctness
  (capped ch10 / ch18 shots against known variants, a short chapter sweep).

## Desk finding 2: the recompiler's register options are all off [V from config.cpp / our manifest]
- rexglue's codegen has the XenonRecomp-family options (`skip_lr`, `skip_msr`, `ctr_as_local`, `xer_as_local`,
  `reserved_as_local`, `cr_as_local`, `non_argument_as_local`, `non_volatile_as_local`), read from `[entrypoint]`. Our
  manifest sets none, so every PowerPC register lives in the shared context in memory: every ctx.rN write is a store
  the compiler must keep (seen in the assembly: `movq %rax, 96(%rdi)` for each register update).
- Job 219 (after 218): the safe set on - skip_msr, ctr / xer / reserved / cr / non-argument registers as locals. Kept
  off: skip_lr and non_volatile_as_local (the game's catch funclets and any unwinding read saved registers from the
  context) - a later step, alone, with a full sweep.
- Pass for each step: same frame time measures as jobs 217/218, capped shots identical to known variants, no new
  error lines or crashes; then a 29-chapter sweep before a default changes.

## Job 217 result - the baseline [V]
- PC: AMD Ryzen 7 3700X (Zen 2, 8 cores / 16 threads, 3.6 GHz): has AVX2, FMA, BMI2, MOVBE - x86-64-v3 is fine.
- ch02 uncapped (BENCHMARK, vsync off): 5.8-6.6 ms/frame (job 185: 6.49) - no regression. ch10: 15.0-16.0 ms (185:
  14.67) - ~7% slower [G: noise from other load, or Stage 7 bookkeeping].
- Other CPU during each 60 s run: the Claude desktop app ~10-14 s (two processes), System 7-8 s, Defender (MsMpEng)
  4-6.5 s, NVIDIA container ~4 s, NordVPN 3-5 s: about 0.5-0.6 of a core in total.
- Busy threads (ch10 uncapped): the game's render thread (XThread F80000A8) 99%, our renderer 75%, our PM4 thread 46%,
  the game's main thread 29%. ch02: also the game's main thread at 90%. The game threads are the limit.
- Capped runs: the game's render thread still sits at ~99% - it spins in the D3D query poll (sub_829783A8, 15%) and
  sub_82977688 (16%) waiting for the GPU. Not a frame-rate cost (job 178's yield made no difference) but a full core of
  heat/power at 30 fps; worth a look later.
- ch10 capped shots: 2 of 6 identical to job 200/201, the other 4 the usual moving-character timing variants (not
  checked by diff image this time).

## Jobs 218 / 219 - built, measured nothing [V]
- Both built cleanly with the new set-up and passed the unit tests (815,854 assertions, 82 cases): patch 28 is in use
  (1,529 stack macros in file 78), -march=x86-64-v3 on (Ryzen 7 3700X, AVX2). The exe is 76 MB with the 218 set-up and
  64 MB with 219's register options.
- Then both stopped at the first game run: my job scripts never loaded tools\cpu-report.ps1 (Get-CpuProfileArgs not
  found), and 218 also missed its pending tools folder. The lesson: a job that uses a helper checks it loaded before
  taking the lock (job 220 does).
- Job 220 makes the measurements: A = the 219 build as it is (4 runs), B = register options off (218 set-up), 2 runs.
  The manifest is left with the register options off until they pass a chapter sweep.

## Job 220 result [V]
- A (219 build: register options on): all 4 runs stop ~3 ms after launch with "Unhandled guest access violation: read
  of guest 0x00000034" on thread F8000028 - the register options as set break the game. They are off again (manifest
  committed that way) and stay off. [G] Why: one of cr/ctr/xer/reserved/non-argument as locals or skip_msr changes
  something a function relies on across a call (setjmp / exception paths); would need one option at a time.
- B (218 set-up: patch 28, no perf counter, x86-64-v3): ch02 uncapped 5.12 / 5.93 ms (217: 5.8-6.6), ch10 13.93 /
  15.12 ms (217: 15.0-16.0) - about 10% and 6% faster [G until a clean rerun]. Both B runs then crashed on the
  profiler's own thread inside RtlVirtualUnwind (read of 0x8, ~2 s into sampling) - fixed in job 221 (SEH-guarded
  unwind step, stack must move up each frame).
- Other CPU during the runs: 1.0-1.5 cores (System, Memory Compression, Defender, the Claude app) - more than job 217's
  0.5-0.6. [G] Memory Compression is new: the PC was short of free memory then.

## Job 221 result - CPU step 1 measured [V]
- Build: patch 28, no perf counter, x86-64-v3, register options off, + 17 callback hints (all 17 now function starts);
  unit tests pass; the SEH-guarded profiler ran all 4 runs to the end (no crash files, no error lines).
- Same frame windows as job 217 (swap 601 on, 300-frame means, BENCHMARK, vsync off):
  ch02 5.30 -> 4.62 ms (-13%), steady part 5.08 -> 4.40 ms; ch10 14.47 -> 13.23 ms (-8.6%), steady 14.05 -> 12.72 ms.
- Capped shots: ch10 7 of 7 and ch02 7 of 7 identical to a known variant.
- Other CPU 0.6-0.8 cores (back to the job 217 level). The game's render thread F80000A8 is still at 99% and the
  limit; sub_8272DF48 is still its top function (10.6% in ch10).
- Next for CPU: the 29-chapter sweep (job 222); then one register option at a time (job 220: all six together crash
  at boot), starting with the least risky (skip_msr, then ctr_as_local).

## Job 222 result - the map sweep [V]
- tools\chapter_sweep -Cpu: all 25 SP_*_P maps (by map file; the chapter-index sweeps 155/186 cover 29) ran their 75 s
  to the limit on the CPU step 1 build + 17 hints: no crash, no unregistered function, SP_Maria_P included.
- CPU per map: the game's render thread 96-99% everywhere (capped 30 fps): the GPU wait spins (sub_82977688 /
  sub_8298C2A0 33-53% of its samples) - job 225's hook. Other CPU 1.0-1.2 cores, dwm ~30 s per 80 s run [G: the
  sweep's window screenshots every 25 s, and the user's desktop].
- User (08:45): no more sweeps for a while unless truly necessary - PC time goes to the Gears 3 chat.

## Desk notes (2026-10-22 08:50) [V from the code and the job 221 profile]
- sub_8272DF48 (the game's render thread's top function, 10-12% in ch10): ~1,960 instructions of nested loops - UE3
  per-frame scene work (vector dot products = plane tests, 11 memcpy calls, 68 calls in all). Nothing wasteful in the
  translation to target on its own; it gains from the general steps (patch 28, v3, register options).
- Our renderer thread's memmove (16% in ch10): UploadTransient 5.6%, SetConstants 5.2%, PrepareDraw 3.6%. Constants
  are already trimmed (job 166) and skipped when unchanged; a changed block is copied twice (shadow, then the upload
  ring) - copying once straight into the ring would save ~2-3% of that thread. Later item: that thread is ~78% busy
  and not the limit (the game's render thread is), so it lowers CPU use without raising the frame rate.

## Job 223 result - register options one at a time [V]
ch10 uncapped, the same runs, means over swaps 601-4200; the base (all off) measured last in the same job: 13.82 ms.
| Option alone | ch10 ms | |
|---|---|---|
| skip_msr | 13.73 | ~0 |
| ctr_as_local | 13.67 | ~0 |
| xer_as_local | 13.81 | 0 |
| reserved_as_local | 13.76 | ~0 |
| cr_as_local | 12.47 | -10% |
| non_argument_as_local | - | crashes ~3 ms after launch (guest read of 0x34 on the main thread) - job 220's crash |
- cr_as_local goes into job 225's build (checked there: shots, capped, 2x); non_argument_as_local stays off [G: it
  keeps r0/r11/r12-style scratch registers local, and some XDK helper reads one of them from the context].

## Job 225 result - cr_as_local, the GPU-wait sleep, 2x [V]
One build: cr_as_local on + the GPU-wait hook (`--gow2_patch_gpu_wait`). Means over swaps 601-4200.
| Run | wait off | wait on |
|---|---|---|
| ch10 capped: render thread / process | 98.7% / 2.02 cores | 67.0% / 1.71 cores |
| ch02 capped: render thread / process | 98.1% / 1.61 cores | 20.9% / 0.91 cores |
| ch10 uncapped ms | 12.88 | 12.53 |
| ch02 uncapped ms | 4.75 | 4.82 |
- Capped frame times 33.33 ms either way (p99 36.3-36.8); all 28 capped shots identical to a known variant.
- cr_as_local: ch10 13.82 (job 223 base) -> 12.88 ms (-7%); shots identical -> stays on.
- GPU-wait sleep: frees up to ~0.7 of a core at 30 fps, no cost uncapped -> default on in job 227.
- 2x (2560x1440) uncapped: ch10 21.56 ms (46 fps), ch02 9.19 ms (109 fps). ch10 is limited by OUR renderer thread
  (CPU 19.3 ms/frame at 2x vs 10.3 at 1x; GPU only 10.7 ms): 45% of that thread was the job 189 scaled-miss
  diagnostic (TextureCache::LookupScaled's `miss` walks every page of each missed texture, every bind). Off by default
  from job 227 (`--gow2_renderer_scaled_miss_log`).
- dwm (job 226 snapshot, 09:23): with no game running dwm uses ~1% of a core; during our runs since ~07:50 it used
  ~0.45 core. Windows open on the PC then: Settings, Windows Security (picker), NVIDIA Overlay, Claude, a terminal.
  [G] Settings' blurred backdrop recomposed for every game frame; asked the user to close those windows.
- Job 227 (b) [V]: query-poll mode 3 (sleep after 64 polls < 200 us apart) FAILS - capped and uncapped runs didn't
  reach swap 600 in 75 s. [G] sub_829783A8 is called back to back for many different queries each frame (each poll
  usually answered at once), so the streak rule takes that for one long wait and sleeps 1 ms per query. Mode 3 stays
  off (default 0); a working version would need to know which query is being waited on.

## Job 227 result [V]
- GPU-wait sleep on by default: ch10 capped render thread 63%, 33.33 ms, shots 7/7.
- The 2x fix (scaled-miss diagnostic off): ch10 2x 21.56 -> 12.68 ms (46 -> 79 fps), ch02 2x 9.19 -> 5.24 ms (109 ->
  191 fps); at 2x our renderer's CPU is back to 10.2 ms (ch10). ch10 3x (4K): 21.0 ms (48 fps) - GPU-bound (20.7 ms).
- Query-poll mode 3 fails (above).

## Next CPU / GPU steps (2026-10-22 10:30)
- 4K (ch10, job 227): GPU 20.7 ms/frame = draws 14.8, texture loads 2.5 (scaled re-loads), resolves 2.3, EDRAM
  transfers 0.9 - mostly the game's own shading at 9x the pixels; the loads + resolves (~4.8 ms) are the only
  candidates, a bigger piece of work.
- Job 231: the query wait done right - sub_82807588 (UE3's wait for one query) loops GetData(FLUSH) on the same query;
  the hook on its loop head (0x828075BC) sleeps on the guest-write signal (`--gow2_patch_query_wait=1`, off until
  measured).

## Job 231 result - the query wait [V]
| Run | off | on |
|---|---|---|
| ch10 capped: render thread / process | ~64% / 1.64 cores | 38.1% / 1.30 cores |
| ch02 capped: render thread / process | 21.4% / 0.91 | 16.1% / 0.80 |
| ch10 uncapped ms | 12.17 | 12.43 (+2.1%) |
| ch02 uncapped ms | - | 4.45 |
- Capped: frame times 33.33 ms, p99 unchanged, all 28 shots identical to a known variant; no errors.
- Uncapped ch10 +2.1% (just over the 2% bar) [G: noise, or a short sleep when the answer is microseconds away] -> stays
  off by default. Next: turn it on only when a frame cap is active (vsync on or --gow2_max_fps > 0) - where the CPU
  saving matters - leaving uncapped play as it is.

## Job 232 result - the query wait on by default when capped [V]
`--gow2_patch_query_wait` default 2: on when vsync is on or `--gow2_max_fps` is not -1, off otherwise.
| Run | result |
|---|---|
| ch10 capped | 33.33 ms (p99 38.6), render thread 37.2%, process 1.38 cores, hook on, shots 7/7 identical |
| ch02 capped | 33.33 ms (p99 36.3), render thread 15.0%, process 0.85 cores, hook on, shots 7/7 identical |
| ch10 uncapped (vsync off, max_fps -1) | 12.23 ms (job 231 off: 12.17, +0.5%), hook off as intended |
- No errors, no crash files, 82 tests pass. Mode 2 stays the default.
- Where the CPU now goes, capped ch10: our GPU Render thread 37% (65% of it waiting), the game's render thread 37%,
  our GPU Commands thread 17%. Uncapped ch10: the game's render thread 98% (real work: sub_8272DF48 11%), GPU Render
  90%, GPU Commands 50% - uncapped is bound by our renderer's CPU side (9.9 ms/frame) more than by the GPU (4.3 ms).

## Desk finding 3 (2026-10-22): the uncapped limit at 1x is the game's render thread; non_volatile_as_local
- Job 232's uncapped ch10 profile [V]: the game's render thread (F80000A8) is busy 97.8% of the time on recompiled
  code (UE3's render loop sub_8272FD20 96.5% inclusive; sub_8272DF48 31.7% inclusive / 10.8% self - 1,872 PowerPC
  instructions, dot products (vmsum) and prefetches, [G] scene/visibility work; the XDK's D3D calls 0x8297-0x8298xxxx a
  few % each). Our renderer thread is at 90%, so the game thread sets the frame time; making our renderer cheaper
  alone would not raise uncapped fps at 1x yet.
- The codegen option we never tried: `non_volatile_as_local` (r14-r31, f14-f31, v14-v31, v64-v127 as host locals). With
  it the codegen drops the __savegprlr / __restgprlr / __savefpr / __restfpr / vmx helper calls, and it hands SEH
  funclets (auto-marked shares_registers from the scope tables) their owner's registers through ctx [V in our pinned
  SDK's context.cpp / function_graph.cpp, the same as master c94f5eb].
- Job 223's non_argument_as_local crash, desk theory [G]: r0/r2/r11/r12 become zero-initialised locals, but
  `addi r12,r1,-152; bl __savefpr_26` (and the vmx helpers, r11/r12) pass the save area's address in r12 - the helper
  reads its own zeroed r12 and writes the FPRs to address 0-0x90 (and restores garbage). With non_volatile_as_local
  those calls are gone, so the two together may work. Catch funclets still read r12 (their frame pointer from the
  unwinder) - a risk for B only.
- Job 233 measures A = cr + non_volatile and B = A + non_argument, report only (a default change needs a sweep).

## Job 233 result - non_volatile_as_local [V]
| Run | base (cr only) | A (cr + non_volatile) |
|---|---|---|
| ch10 uncapped ms | 12.21 | 11.61 (-4.9%) |
| ch02 uncapped ms | 4.61 | 4.42 (-4.1%) |
| ch10 capped: game render thread / process | (job 232: 37.2% / 1.38) | 32.7% / 1.31 cores |
| ch02 capped: game render thread / process | (job 232: 15.0% / 0.85) | 14.7% / 0.72 cores |
- A: every run to the time limit, no errors, no crash files; capped 33.33 ms, all 14 shots identical to a known variant.
  Exe: base 67 MB, A 62 MB, B 58 MB; in A 543 functions keep r14 local; no __savegprlr_14 calls left (base: 538).
- B (A + non_argument_as_local): crashes 8 s in with job 223's error (guest read of 0x34 on the main thread). So the
  save/restore-helper theory was WRONG - the helpers are gone in B and it still crashes. non_argument_as_local stays off.
- Why A gains only ~5%: with A the game's render thread now spends 11.4% in UE3's query wait (sub_82807588, inclusive)
  - it waits for our side - and our GPU Render thread is at 94% (9.9 ms CPU/frame). The limit has moved to our
  renderer's CPU work: memmove 18.6% of that thread (UploadTransient 6.8, SetConstants 5.6, PrepareDraw 4.2).
- The query-wait hook's mode-2 check called rex::cvar::Query<bool>("gow2_gpu_vsync") on every loop pass:
  GetFlagByName 4.3% + Query 2.1% of the render thread in A (spin time, but it slows each poll). Fixed: read once per
  1024 calls (game_patches.cpp).
- A is not the default yet: it needs the sweep (notes/60 rule) when PC time allows. Only the uncapped frame rate gains;
  capped play saves a little CPU.

## Job 234 (2026-10-22): where the 4K GPU time goes; the cvar lookup fix
- 4K ch10 (job 227): GPU 20.7 ms = draws 14.8, texture loads 2.5, resolves 2.3, transfers 0.9. The draws are the big
  part, but we only know their total. `--gow2_renderer_gpu_by_target` (diagnostic, off by default): the draws carry
  their render target's slot as a GPU category (0x20 + id mod 32, still summed into "draws"); every CPU-split window
  logs the 12 targets whose draws took the most GPU time (format, EDRAM base, pitch, host size, draws/frame).
- [G] Candidates: the shadow maps (800x800 x 3 = 2400x2400 each at 4K), the game's half-res passes, the scene.
- Resolves at scale run 4 passes (1x words, scaled words, scaled words -> 32 MB window, 1x words -> guest mirror);
  fusing the 2nd and 3rd is the next resolve step if the resolves stay a big share [desk only].
- The query-wait cvar lookup fix goes in with this job (uncapped ch10 1x compared with job 233's base 12.21 ms).

## Job 234 result [V]
- ch10 capped 33.33 ms, 7/7 shots identical; the GetFlagByName lookup is gone from the profiles.
- ch10 1x uncapped 12.47 ms vs job 233's base 12.21 (+2.1%, just over the bar) - [G] noise: other processes used 0.9
  cores (0.7 in job 233), and the change only removes work. Re-measured in job 235.
- 4K ch10 (GPU 20.6 ms): draws 15.1 = the HDR scene colour target (7e3, base 720) 12.35 ms / 4020 draws, depth pre-pass
  1.2, the rest < 1 ms each; shadow maps 0.05 ms (the [G] above was wrong). Resolves 2.4, texture loads 1.9-2.5.
  The by-target timing costs nothing measurable (20.68 vs 20.78 ms).
- 4K ch18 (GPU 17.0 ms): draws only 4.3 - resolves 9.4 ms for 27 resolves a frame. Cause [V from the code + log]: the
  scaled-memory windows (32 MB, placed on 1 MB steps where a resolve first needs one) overlap - 6 windows at 10300000,
  10F00000, 11000000, 11100000, 11C00000, 1E000000 - and ScaledToMemory writes each scaled resolve into every window that
  touches it (one full-size dispatch each): the resolves at 111D0000 / 11568000 go into 4 windows.
- Job 235: `--gow2_renderer_scaled_window_align_mb=16` - new windows only at 16 MB steps, so any extent of up to 16 MB fits
  one window and an address is in at most two; the log counts window writes. ch18 / ch10 at 4K with 1 vs 16, shots
  compared between the two (same data, so they must be identical).

## Job 235 result [V]
| Run | align 1 (today) | align 16 |
|---|---|---|
| ch18 4K uncapped | 18.55 ms, GPU 16.4, resolves 9.23 | 15.11 ms (-18.5%), GPU 12.8, resolves 5.55 |
| ch18 windows / window writes per 300 frames | 6 / 31,800 | 3 / 15,900 |
| ch10 4K uncapped | (job 234: 20.78) | 20.67 ms, resolves 2.52 |
| ch10 windows / writes (capped) | 3 / 8,100 | 3 / 9,220 |
- Capped 4K shots: ch18 7/7 and ch10 7/7 identical between align 1 and 16. No errors, no crash files.
- Fewer windows also means less video memory: each window is 302 MB at 4K (6 -> 3 in ch18).
- ch10 1x uncapped 12.10 ms (job 233 base 12.21): job 234's 12.47 was noise; the cvar-lookup fix stands.
- Still ~2 writes per scaled resolve with 16 (neighbouring windows overlap by 16 MB). Job 236: align 32 = a 32 MB
  boundary first (windows that never overlap), 16 / 8 / ... only for an extent crossing a 32 MB boundary.

## Job 236 result [V] - align 32 is not better than 16
| 4K uncapped | align 1 | align 16 (job 235) | align 32 |
|---|---|---|---|
| ch18 | 18.55 ms (235) | 15.11 | 15.30, 3 windows, writes 15,900 |
| ch10 | 20.78 (234) | 20.67, 3 windows, writes 9,000 | 21.25 (+2.8%), 4 windows, writes 15,000 |
| ch05 | 16.17, 4 windows, writes 13,200 | - | 17.41 (GPU 17.1 vs 17.25), 4 windows, writes 13,800 |
- All capped 4K shots identical: ch18 / ch10 align 32 vs job 235's align 1, ch05 32 vs 1 (7/7 each).
- Why 32 does not reach one write: an extent crossing a 32 MB boundary still needs an in-between window, and every
  address in it is then in two windows again (ch10: 0E000000 + 0F000000 + 10000000).
- Keep 16. Job 237: ch05 and ch02 at 4K with 16 vs 1 (no build), then 16 can become the default for scales above 1.

## Job 237 result [V] - align 16 on ch05 / ch02
- ch02 4K capped: 7/7 shots identical (16 vs 1). Uncapped ch02 4K align 1: 7.87 ms.
- ch05 4K capped: 6/7 identical, frame 1800 differs (250k of 8.3M pixels, max 31, mean 5.9): the blurred background
  between Dom's shoulder and head (looks like a depth-of-field / bloom texture read). In job 236 align 1 and align 32
  gave identical ch05 shots (7/7), so [G] align 16 reads one resolve-written texture from another window that holds
  different data - or the run is not repeatable there. Not yet explained: the default stays 1 until job 238 repeats
  ch05 (1 and 16 again).
- ch05 uncapped timings are not comparable run to run: the same 300-frame windows show draws 3.7 vs 7.3 ms (the scene
  differs between uncapped runs), so only the capped (fixed 30 fps) ch05 runs are repeatable.

## Job 238: fused scaled resolve (--gow2_renderer_fused_scaled_resolve, off)
- Today a scaled resolve is 4 passes: 1x words, scaled words into the scaled destination texture, that texture into
  each window (ScaledToMemory), 1x words to guest memory. Fused: the resolve shaders' scaled pass writes straight into
  each window (kHlslScaledChunk: the kScaledWordsToMemCs addressing and endian swap), so the scaled destination
  texture and its reads are gone. Destinations the presenter has shown (front buffers) keep the old path, since the
  presenter reads the scaled destination texture (the first frame of a new front buffer is presented at 1x).
- Checks: the shots with fused must be identical to the non-fused ones (same words, same addresses).

## Job 238 result [V]
- ch05 4K capped repeated: align 1 and align 16 both 7/7 identical to job 236's align 1; both differ from job 237's
  align-16 run at frame 1800 only -> job 237's difference was that run, not the alignment. Align 16 becomes the default
  (user 12:14) in job 239.
- Fused scaled resolve (align 1, capped shots): ch18 and ch10 identical except frame 0 (the first frame of a new front
  buffer is presented at 1x - expected); ch05 differs on all 7 frames by a little everywhere (mean 5, same sharpness,
  looks like an exposure shift) [G: a small resolve the eye adaptation reads, through the fused path]. Not explained;
  job 239 splits it (fused colour only / depth only).
- Fused timings: ch18 4K uncapped align 16 15.37 -> 14.44 ms (-6%, resolves 5.73 -> 4.67); capped ch10 resolves
  2.36 -> 1.90 ms; ch18 with align 1 no gain (4 windows, each a full resolve pass). Fused also drops the 87 scaled
  destination textures in ch18 (only the front buffer keeps one).
- ch10 1x capped: 7/7 identical to jobs 232-234 (the shader changes do not touch 1x).

## Job 239: Gears 3's MSAA work ported; fill rect lists; align 16 default
- From Gears3-Native shadow_renderer.cpp (three-way merge against our job 229 base, which is what theirs started from):
  --gow2_renderer_msaa_colour_resolves (Texture2DMS colour resolve, samples per copy_sample_select, host sample map
  0,3,2,1 for 4x), --gow2_renderer_msaa_transfers (MsaaUpTransfer: 1x -> 4x per sample), --gow2_renderer_clear_msaa_twins,
  and their diagnostics (--gow2_renderer_log_depth_bias_base, the fill-why log). All default on; Gears 2 has no real
  MSAA targets, so none should fire here.
- --gow2_renderer_fill_rect_lists (Gears 3 job 032, their version kept): fill draws of 9-48 vertices bound ownership by
  the union of their rectangles instead of the scissor. The CPU split line counts them: "(multi-rect N)".
- Merge detail: their MSAA resolve shader takes the sample count / map in c2.z / c2.w, which the fused path also uses,
  so the fused path skips MSAA sources; the MSAA shader source gets the kHlslScaledChunk helpers too.

## Job 240: float constants - skip when nothing wrote them (--gow2_renderer_const_gen_skip, off)
- Our renderer thread (job 232 uncapped ch10): memmove 18.6% - UploadTransient 6.8 (vertex data into the ring: one
  copy per byte, needed), SetConstants 5.6 (copy into the slot's shadow when the data differs), PrepareDraw 4.2 (the
  shadow into the ring). Every draw also memcmps the VS / PS float constants (up to 4 KB each) against the shadow.
- The render pipe already applies the register changes per draw on the renderer thread, so it now counts them per
  constant range (VS floats 0x4000-0x43FF, PS floats 0x4400-0x47FF, bool/loop 0x4900-0x4927; dev_tools.h
  XenosConstGens; a swap or drain reloads the whole file and moves all three). When a range's counter has not moved
  since that slot's last upload and the size is the same, the draw skips SetConstants: no compare, no copy - the
  backend slot still holds that data. Only valid while the pipe feeds the renderer (--gow2_pm4_split).
- Job 240: ch10 / ch02 1x uncapped off vs on (constants ms in the CPU split, skips per frame), capped shots with it on.
