# 34 - Stage 0 plan, order and estimate vs actual (D19)

Stage 0 in `28` is estimated at 6-9 working days, plus 2-4 days for the layer split. This note splits it into items, records an
estimate for each **before** the item starts, and the actual time once it passes. After a few items the ratio actual/estimate is
used to recalibrate the 36-61 day total in `28`.

**Units.** Estimates are the roadmap's "working days" (d). Actuals are elapsed hours (h) from starting the item to the job
that proves it, overlapping with other work, so they are an upper bound on effort. The ratio is what matters, not the units.

**Labels.** [V] verified, [I] inferred, [~] approximate. "Retro" means the estimate was written after the fact; those rows
don't count for calibration.

## Estimate vs actual

| # | Item | Estimate | Set | Actual | Status | Proof |
|---|---|---|---|---|---|---|
| 0a | Layer split (D16) | 2-4 d | before (`28` §1b) | ~1 h [~] | done | job 030 |
| 0b | Tools downloaded and FLIP built | 0.25 d | retro | ~0.5 h [~] | done | jobs 031, 036 |
| 0c | Draw observer, SDK patch 2 | 0.5 d | retro | ~0.5 h [~] | done | job 034 |
| 0d | Per-swap frame readback, SDK patch 3, PNG frames | 0.5 d | retro | ~0.75 h [~] (incl. research brief) | done | job 035 |
| 1 | Repeatable runs: parse check, runtime fixed clock, RNG, chapter + scripted input + frame capture | 1-1.5 d | 2026-10-20 18:50 | ~2 h [~] (incl. the patch-4 dead end and the rain investigation) | done for dry routes (D20); rain routes parked | jobs 037-044 |
| 2 | Noise floor (FLIP, emulator vs itself) and its explanation if > 0.01-0.02 | 0.5-1 d | 2026-10-20 18:50 | | done for dry routes: ch2 max FLIP 0.0011, ch10 0.00015; thresholds set from the baseline runs (job 045) | jobs 039, 040 |
| 3 | Draw observer cost (heavy chapter, off vs on) | 0.25 d | 2026-10-20 18:50 | ~0.4 h [~] | done: 0.09 ms/frame (0.3%), passes | job 039 |
| 4 | Upstream InputSystem issue | 0.1 d | 2026-10-20 18:50 | | waiting for "post it" | |
| 5 | Baseline (frames, timings) before the SDK pin bumps, then the bumps | 0.5-1 d | 2026-10-20 18:50 | ~1 h [~] | done: pins bumped, frames unchanged within route noise | jobs 045-047 |
| 6 | Catch2 unit tests for the Xenos format values (`31`) | 0.5 d | 2026-10-20 18:50 | ~0.3 h [~] | job 052 (1,091 checks pass in a local pre-check) | job 052 |
| 7 | In-frame HDR buffers: render-target and resolve readback, EXR (tinyexr), HDR-FLIP | 1-1.5 d | 2026-10-20 18:50 | | | |
| 8 | Same-frame comparison groundwork: GPU frame traces (SDK patches 5-6, from Xenia Canary) | 1.5-2 d (trace route estimated 1-1.5 d at 20:33) | 2026-10-20 18:50 | ~2.5 h [~] | recording works; replay repeats exactly and draws the whole scene, but lit 3D scenes come out too dark - parked | jobs 049-059 |
| 9 | Report: first differing draw/resolve/buffer, FLIP error maps | 0.5-1 d | 2026-10-20 18:50 | | | |
| 10 | Separate diagnostic passes (RenderDoc, Aftermath, GPU-based validation) | 0.5 d | 2026-10-20 18:50 | | | |

Items 1-10 add up to 6.35-9.6 d, slightly above the roadmap's 6-9 d at the top end. I'm leaving it there rather than shaving
estimates to fit. The reference library (`28`) comes before Stage 5, not in Stage 0.

**Early reading (not yet a calibration):** the only row with an estimate made beforehand (0a) took about 1 h against 2-4 d.
The split was a clean start, not the untangling of an existing renderer the estimate assumed, so it says little about Stages
1-6. Calibration starts with items 1-3.

## Order and reasons
1. **Repeatable runs (item 1).** Everything that compares frames depends on it.
2. **Noise floor (item 2).** Sets the FLIP thresholds, which go into `00-decisions.md`.
3. **Observer cost (item 3).** Cheap, and it decides whether later per-draw copying must be selective.
4. **Upstream issue (item 4).** Ready apart from the user's go-ahead.
5. **Baseline, then pin bumps (item 5).** Uses the repeatable frames, so the bump is checked against a real baseline.
6. **Unit tests (item 6),** then **HDR buffers (7)**, **same-frame groundwork (8)**, **report (9)**, **diagnostic passes (10).**

## Item 1: repeatable runs

**Does the retail build parse the switches? Checked in the image dump (2026-10-20 18:48).**
- These strings exist and code references them [V]:

  | String | Code reference |
  |---|---|
  | `BENCHMARK` | 0x82840570 |
  | `FIXEDSEED` | 0x82350020 and 0x824594A0 |
  | `FPS=` | 0x82840A48 |
  | `SECONDS=` | 0x828409E4 |
  | `UNATTENDED` | 0x8227E788 |
  | `DUMPMOVIE` | 0x82840514 |
  | `nostartupmovies` | 0x822F27F4 |

- `BENCHMARK`, `FIXEDSEED`, `UNATTENDED`, `DUMPMOVIE` and `nostartupmovies` are all passed to the same function, 0x8227A878 [V].
  `-nostartupmovies` is known to work [V, job 033], so 0x8227A878 is very likely UE3's `ParseParam` [I].
- `SECONDS=` goes to a different function, 0x82278A38, likely `Parse(..., float)` [I]. `FPS=` follows the same pattern [I].
- So the switches are parsed. Whether they **do** anything in this build (a fixed time step, seed 0) is what job 037 measures.

**Runtime-level option (planned whatever job 037 shows).** UE3's switches can't cover everything: they don't fix the time that
loading threads, audio and streaming see. So we add a deterministic mode in layer 0. It contains no game code, so Gears 1 and 3
can reuse it.
- **Fixed-step guest clock.** Every guest time source advances by exactly one frame (1/30 s, or the `FPS=` value) per swap,
  instead of by wall time:
  - the `mftb` time base read by recompiled code;
  - `KeQueryPerformanceCounter`;
  - `KeQuerySystemTime` / `NtQuerySystemTime`;
  - the kernel's `KeTimeStampBundle` (interrupt time and tick count);
  - `XamGetTickCount`-style tick counts, if present.

  First step: find which source UE3's `appSeconds` uses here. Likely `QueryPerformanceCounter` → `KeQueryPerformanceCounter` or
  `mftb` [I]. That source is the one that must be fixed for game time to be repeatable; the others matter for timeouts and
  streaming.
- **RNG.** `-FIXEDSEED` seeds `appRand` with 0, and its two code references sit before calls that look like seed set-up
  (0x82AA7B78, 0x82A8E468) [I]. Beyond that, find and pin any other seeds: the CRT `srand(time)`, XeCrypt random, and particle
  seeds taken from the clock. The cheapest route is to fix the clock first and see what still differs.
- **Route.** Load with `?chapter=`, scripted input timed in **frames, not seconds** (the current script pad uses wall-clock
  seconds, which isn't repeatable), and capture by frame number.
- **Known risk: asynchronous loading.** Streaming completes on other threads. Even with a fixed clock, the frame on which a mip or
  level arrives can vary [I]. Options, in order:
  1. Start counting frames only after loading is idle.
  2. Compare only frames after streaming has settled.
  3. Make the loader wait, as a deterministic-mode option.

**Then the noise floor (item 2).** Run the same route twice and compare frame N with frame N using FLIP. If the mean is above
about 0.01-0.02, find the cause (timing, particles, streaming) before setting any thresholds. The thresholds and how they were
measured go into `00-decisions.md`.

## Item 3: draw observer cost
- **Route:** chapter 10 (index 9, `sp_rescue_p`), a heavy firefight route, or whichever sweep chapter shows the most draws per
  frame.
- **Three configurations,** run one after the other:
  1. no observer registered;
  2. observer registered with draw stats (counting only);
  3. observer plus frame readback every frame.
- **Measured:** mean and 99th-percentile frame time, taken from swap timestamps logged by a minimal swap timer that runs in all
  three configurations.
- **Rule:** if configuration 2 costs more than ~3% of frame time, the observer copies only what the active test asked for
  (per-test masks), and nothing at all when no test is running.

## Final frames vs in-frame buffers
- **Final frames:** per-swap PNG, 8-bit, converted from the 10-bit guest output with gamma already applied (`33`). These are for
  final-frame comparisons with LDR-FLIP. They stay as they are.
- **In-frame HDR buffers:** the lit scene (7e3/FP16), AO, bloom, and the DOF / motion-blur inputs go to **EXR** via tinyexr and
  are scored with HDR-FLIP. They are read at the render-target point (just after `render_target_cache_->Update` in `IssueDraw`)
  and the resolve point (just after `Resolve` in `IssueCopy`), as mapped out in `33` §2-3.

## Item 4: upstream issue
The `InputSystem` race (patch 1) is reported to rexglue/rexglue-sdk with the patch offered. Once it's fixed upstream, patch 1
is dropped (D15). Posting waits for the user's explicit "post it" at the moment of posting.

## Results so far (2026-10-20 19:05)

**Repeatability (item 1), jobs 037 and 039 [V].**
- **Chapter 1, no input (job 037).** Every captured frame was the intro movie (2-3 draws). With the switches, 20 of 20 frames were
  identical; without them, 1 of 20. So `-BENCHMARK` changes movie timing in this build, but that run said nothing about gameplay.
- **Chapter 2 gameplay (job 039).** Run with `-BENCHMARK -FIXEDSEED` and A presses on frames 300-1800 (`--gow2_input_frames`).
  Frame 1500 shows the player in control.
  - Frames: 17 of 20 gameplay frames are byte-identical, and draw counts are identical on every frame.
  - The 3 that differ:

    | Frame | FLIP | Pixels that differ | Max channel difference | Where |
    |---|---|---|---|---|
    | 450 | 0.0011 | 4,905 | 106 | the two ceiling light panels, plus thin slivers on the railing posts below |
    | 1200 | 0.0003 | 4,674 | 52 | a background area near the DANGER sign, plus a small patch on a wall screen |
    | 1350 | 0.00001 | 84 | 2 | scattered single pixels |

  - Most likely cause: flickering or animated lights and materials driven by real time rather than game time [I]. That is
    exactly what the fixed-step clock (patch 4) covers. So patch 4 stays planned, as a follow-up rather than a blocker.
- **Where all guest time comes from [V, SDK source].** Every guest time source goes through one function, `UpdateGuestClock()` in
  `src/core/clock.cpp`:
  - `mftb` is generated as `REX_QUERY_TIMEBASE()`, which calls `Clock::QueryGuestTickCount()`;
  - guest system time, uptime and `KeTimeStampBundle` are derived from the same clock.

  Patch 4 is therefore one small change there. Planned design: advance the guest tick count by exactly one frame per swap. If no
  swap arrives within ~200 ms of host time, a watchdog advances it anyway, so code that waits by spinning on the clock can't
  hang. The watchdog never fires on a normal 33 ms frame, so it doesn't affect repeatability.
- The game reads `mftb` in 17 places. Most are in the D3D library (0x8295-0x8299); one is 0x82AA7B78, which the `-FIXEDSEED` code
  calls, probably the clock-based seed when the switch is off [I].

**Observer cost (item 3), job 039 [V].** Chapter 10, 8,500-9,100 draws per frame, swap interval steady at 33.3 ms (30 fps cap)
in all three setups:

| Setup | Callback time per frame | Frame interval |
|---|---|---|
| Timing only | 0 | 33.3 ms |
| Draw stats | 0.09 ms mean, 0.21 ms max | unchanged |
| Draw stats + PNG every 30 frames | 0.12 ms mean, ~1.2 ms max (on captured frames) | unchanged |

Well under the 3% (~1 ms) line, so no change is needed. Revisit when per-draw state copying for item 8 is added.

## Results 2026-10-20 19:45 (jobs 040-044)

- **Chapter 10 [V]:** 17 of 18 gameplay frames identical; the one that differs scores FLIP 0.00015.
- **Chapter 13, heavy rain [V]:** 0 of 18 identical, FLIP 0.07-0.24. Characters and level match; rain streaks, splashes and wet
  sheen differ, and the draw count drifts by 1-5 out of ~3,850.
- **Patch 4, the fixed-step clock (job 041) [V]: made things worse.** Chapter 2 went from 17/20 identical to 0/67, and chapter 13
  was still not identical. Two causes [I]:
  - time advanced on the thread that hands frames to the GPU, while the game thread reads time on its own schedule;
  - the watchdog stepped 22-55 times per run during loading, where no frames arrive for seconds.

  `-BENCHMARK` already gives the game loop a fixed step. Patch 4 stays in the diff, **off by default**
  (`--clock_fixed_step_fps=0`), and gets stripped before any upstream submission. A better design, if ever needed, would step
  time when the game thread starts a frame.
- **Occlusion queries off (job 042) [V]:** no effect on chapter 13 (FLIP 0.07-0.21). That theory, from research brief 7, is out.
- **Draw-by-draw logs (jobs 043-044):** `--gow2_draw_log_frames` hashes each draw's state and constants. Whole-bank constant hashes
  mark every draw as different once any single constant differs, so job 044 dumps the registers of the first 60 draws, with
  chapter 2 as a control.

**D20 (user, 19:43): rain chapters are skipped for tests that need two runs to match.**
- Repeatable-run tests (the pre-pin-bump baseline, before/after checks) use dry routes.
- Rain routes are covered later by the same-frame comparison, where both renderers draw one captured frame, so run-to-run
  weather differences don't matter.
- The rain investigation is parked.

## Results 2026-10-20 22:00 (jobs 045-051)

**Baseline and library updates (item 5) [V].**
- **Baseline (job 045):** six routes, each run twice.
- **Pin bump:** first try (job 046) failed at the SDK build. SDL 3.4.18 adds a C++ file, and CMake's C++20 module scanning in the
  Ninja Multi-Config generator declared its `.modmap` output twice. The job restored the old pins as designed. Job 047 set
  `CMAKE_CXX_SCAN_FOR_MODULES=OFF` in the SDK build tree and the bump built.
- **Result:**
  - SDL3 3.4.18, ImGui 1.93.0, VMA 3.4.0, Tracy 0.14.1, Catch2 3.16.1.
  - Exact routes against the baseline: menu 19/19 identical, chapter 23 19/19, chapter 2 17/19 (FLIP 0.003), chapter 20 14/19 (FLIP
    0.0009). Frame timing unchanged.
  - Pins live in `patches/sdk-pins.txt`.

**Chapter sweep (job 048, user's chapter numbers; index = chapter - 1) [V].** Every chapter was run twice for 65 s.
- **Live regression set** (calm, every draw count equal):

  | Chapter | Identical frames |
  |---|---|
  | 23 | 9/9 |
  | 20 | 8/9 |
  | 5 | 8/11, up to 6,600 draws |
  | 14 | 7/9 |
  | 11 | 6/9 |
  | 24 | 6/9 |
  | 6 | 5/10 |
  | 10 | 3/9, about 9,000 draws |
  | 9 | 4/4 |

  The menu is in the set too.
- **Not in the set:**
  - FLIP calm but draw counts mismatched: 19, 21.
  - Near: 2, 12, 18, 22.
  - Drifting (vehicles, combat, weather): 4, 7, 8, 13, 15, 16, 17, 25, 27, 28, 29.
  - No gameplay within 65 s (cinematics): 1, 3, 26.
- **Rule (user, D21):** no repeat sweeps. Routes gain confidence from runs other jobs make anyway and drop out if they wobble. Busy
  chapters go to trace replay.

**GPU frame traces (item 8), SDK patches 5 (record) and 6 (replay) [V].**
- **Recording:** chapter 2 frame 1500 gives a 39 MB `.xtr`:
  - 1,609 draws, about 32,000 packets;
  - 27 MB of memory reads (18.9 MB of it the GPU-written ranges at trace start);
  - the 10 MB EDRAM snapshot.

  The container parser (`xtr_stats.py`) reads it cleanly. Fixed in our port: Canary's `InitializeTraceCompleteDownloads` never
  advances its download offset, writing the first block's data for every range. That's an upstream candidate.
- **Replay** (`--trace_gpu_replay`, game held with `--gow2_hold_guest`): every play issues exactly the live frame's 1,581 draws, 23
  copies and 3,177,795 indices. Output is byte-identical between plays and between two processes.
- **But the replayed picture is wrong:** almost black, FLIP 0.52 against the live frame.
  - With async shader compilation on, play 1 was blacker still, because draws are skipped while pipelines compile. Xenia's
    trace-dump turns async off; so does our replay test now (`--async_shader_compilation=false`).
  - The rest remains. Suspects: texture data not captured (a rough offline check found most texture base pages the draws use
    never appear in the trace, but that check is noisy), and the EDRAM snapshot restore, not ported yet.
  - **Parked (user, 21:55).** First step when resumed: count texture reads live vs in the trace.
  - The D21 Stage 1 gate depends on this being fixed.

**Upstream candidates (for when GitHub is connected; each needs an explicit "post it"):**
1. rexglue: the InputSystem race fix (patch 1; issue drafted).
2. Xenia Canary/Edge: the trace download offset bug above.
3. rexglue: the trace writer/player port (patches 5-6) once replay is correct; the draw observer and frame readback (patches 2-3).
4. rexglue: the SDL 3.4.18 / module-scanning build fix.

## Results 2026-10-20 22:45 (jobs 052-059)

- **Unit tests (item 6, job 052) [V]:** `gn_tests` passes 1,091 assertions in 7 test cases (7e3, 20e4, 2_10_10_10, Tiled2D).
- **Replay (item 8), more findings [V]:**
  - **Trace-start cache clear (job 053).** The SDK now empties the host texture cache and vertex-buffer residency when a trace
    starts, so the traced frame requests, and records, everything it reads. The chapter 2 trace grew from 39 MB to 97.7 MB. The
    replay now draws the whole scene (textures, characters), but too dark (FLIP 0.43).
  - **EDRAM snapshot restore (job 054):** ported from Canary. No change to the output.
  - **Draw-by-draw (job 055):** all 1,604 replayed draws match the live frame in type, count, index buffer, render state, ALU and
    bool constants and surfaces.
  - **Other scenes (job 056):**

    | Scene | FLIP vs live | What differs |
    |---|---|---|
    | Main menu | 0.05 | Only fire and vertical bands |
    | Chapter 23 frame 1200 | 0.17 | Dark, with darker side bands |

  - **Untiled (job 059):** the live black shading is fixed (see D23), but the untiled replay is still dark (FLIP 0.43). So tiling is
    not the replay's cause.
  - **Parked (user).** What's known: inputs identical; the menu nearly right; lit 3D scenes dark tiled or untiled. Next idea when
    resumed: per-draw EDRAM dumps (Edge's stop-command idea) to find the first draw whose output departs.
- **Untiled works (job 059) [V]:**
  - The first manifest attempt (job 057) put `[[midasm_hook]]` at top level and was silently ignored; rexglue reads
    `[[entrypoint.midasm_hook]]`.
  - With the hook emitted: the tiling flag is cleared at startup, draws per frame drop 1,581 to 1,206, resolves 23 to 20, and
    character armour is lit correctly (live tiled vs untiled FLIP about 0.06 throughout).

## Stage 1 (resources): estimate vs actual

Estimates written before starting (D22: continue through the stages). The roadmap's Stage 1 total is 7-11 d.

| # | Item | Estimate | Set | Actual | Status | Proof |
|---|---|---|---|---|---|---|
| S1.1 | Watch the resource calls: mid-asm hooks at the XG header setters, XGOffset* binders, lock core, unlock (+2 inlined), defrag, mip realloc, deferred free; per-frame counts | 0.25 d | 2026-10-20 22:47 | | job 060 | |
| S1.2 | Resource table keyed by memory range, re-keyed at XGOffset*; dirty at unlock / defrag / realloc / resolve | 1.5-2 d | 2026-10-20 22:47 | 2026-10-20 23:19 | jobs 061-063 | ranges from the draw's shaders (SDK OnDrawShaders); events from the watch hooks, resolves and the page watch |
| S1.3 | Comparison mode: at each draw, hash what the bound resources hold vs our table's view; log mismatches by caller | 1.5-2 d | 2026-10-20 22:47 | 2026-10-20 23:19 | jobs 061-063 | `--gow2_res_check=N` (layer 1 resource_check.cpp); S1.2+S1.3 took ~0.5 h together |
| S1.4 | Pass on the regression routes (no unexplained mismatches) | 0.5-1 d | 2026-10-20 22:47 | 2026-10-20 23:40 | job 064 | see results below |

## Stage 1 results (jobs 060-064, 2026-10-20 23:45)

**Question:** does memory the GPU reads change without an event we can hook? (roadmap: the Stage 1 stop-and-rethink point)

**Tool:** `--gow2_res_check=N` (layer 1 `resource_check.cpp`). Every N frames it hashes (XXH3) the memory behind every
vertex fetch, texture (base level and first mip, size from the fetch constant) and index buffer that the draw's shaders
actually use (SDK patch 2 now passes the active shaders, `OnDrawShaders`). A changed range is classed: D3D event (unlock,
XGOffset* rebind, mip realloc / defrag via the job-060 hooks, or a resolve to that address), page watch (the emulator's
write-protect callbacks: CPU writes and kernel file reads into GPU-read pages), page-watch area (the up-to-4 MB block the
emulator unwatches and invalidates around a hit page), or UNEXPLAINED. Cost: observer callbacks 0.07 ms/frame at a check
every 60 frames.

| Job | Version | Result (chapter 2, index 1, 95 s) |
|---|---|---|
| 061 | v1: 64 KB texture window, resolves only on check frames | steady state ~1 unexplained per check (one resolve target), 14-23 during streaming |
| 062 | v2: real texture sizes, resolves every frame, page-watch class | steady state 0; 190 unexplained, all textures/mips during the three streaming bursts; page watch caught ~40% of streaming changes |
| 063 | v3: + page-watch 4 MB area | **0 unexplained**: 113 D3D event, 136 page watch, 189 watch area |

S1.4 (job 064), one 85 s run per live route (human chapter numbers), pass = 0 unexplained. **10 of 10 pass.**

| Route | Checks | Ranges hashed | Unchanged | D3D event | Page watch | Watch area | Unexplained |
|---|---|---|---|---|---|---|---|
| 23 | 39 | 66,536 | 59,240 | 79 | 168 | 146 | 0 |
| 20 | 39 | 49,967 | 41,951 | 42 | 129 | 103 | 0 |
| 5 | 39 | 46,076 | 35,997 | 72 | 230 | 204 | 0 |
| 14 | 39 | 39,841 | 34,683 | 59 | 83 | 46 | 0 |
| 11 | 39 | 28,811 | 22,859 | 53 | 45 | 133 | 0 |
| 24 | 39 | 39,089 | 30,828 | 121 | 438 | 506 | 0 |
| 6 | 40 | 35,675 | 29,602 | 59 | 52 | 35 | 0 |
| 10 | 39 | 111,617 | 104,182 | 11 | 112 | 57 | 0 |
| 9 | 40 | 15,099 | 11,690 | 20 | 4 | 8 | 0 |
| menu | 41 | 11,405 | 4,917 | 29 | 60 | 77 | 0 |

(The rest of each "hashed" count is ranges seen for the first time, which have nothing to compare against.)
Stage 1 actual: S1.1-S1.4 in about 1 hour of work plus 30 minutes of PC time, against an estimate of 3.75-5.25 days.

**What this means (verified on these routes):**
- Vertex and index data: every change comes with a D3D event or a page-watch hit (dynamic per-frame buffers are CPU
  writes the page watch sees). No unhooked write path.
- Textures: steady play changes only through resolves. Streaming writes (file reads into texture memory, and CPU
  copies) are seen by the page watch, partly only as the 4 MB invalidated area.
- So Stage 1's design is: resource table + D3D-call hooks + a page-write watch (as the emulator has). The 4 MB area is a
  coarse class: right for the emulator (it reloads the whole area), but our renderer should watch at page granularity
  so it knows exactly which resource changed. Not a blocker.
- Limits: hashes every 60 frames, not every draw, so a change and change-back inside 60 frames is not seen; CPU writes
  racing the GPU thread can make a hash differ (none showed up as unexplained); the 4 MB class may hide a real miss inside
  a streaming burst.

**Busy chapters (job 072):** the check works inside one run, so it does not need repeatable routes. One 85 s run each,
pass = 0 unexplained. **5 of 5 pass:** chapter 4 (39,696 ranges), 7 (34,405), 13 rain (55,749), 16 (32,021), 17 (33,332),
40 checks each, 0 unexplained. With job 064 that is 15 routes and the menu.

## Status at 2026-10-21 00:20 (overnight, D22)

- Stage 1 (resources): done for tracking (D24). Comparison mode `--gow2_res_check`; 15 routes + menu pass.
- Stage 2 (clears, resolves, tiling): inventory done (`35`): resolve and pass tables for chapters 2 and 23, clears
  identified (one shader pair, half-pitch 4x MSAA rect), aliasing rules, buffer dump tool (`--gow2_buffer_dump_frame`),
  pass log (`--gow2_resolve_log_frames`). Tiling: untiled on by default (D23).
- Stopped before Stage 3 (our draws): needs the D21 gate and the user's calls on P1 (graphics API) and P3 (untiled as
  the decision), see D25.
- Also written: `tools/nightly.ps1` (D21 item 3; not run yet).
