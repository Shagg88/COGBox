# 10 - D3D12 hot spots (for the D3D12 phase after the launcher; Shagg 20:29) - from existing runs, no new runs

Source [V]: job 025 run C, University f1800 area, warm caches, fixed step 30, uncapped, D3D12, 4 test tools on
(out\runs\20261024-191342). Cross-checked with jobs 023 / 027 / 031.

## Frame
- Frame interval 10.19 ms (p99 12.29, max 12.61) WITH tools; "observer callbacks" 9.34 ms/frame = what the test
  tools cost the frame (frame times, frame shots, gpu_by_target, stats). Real play has none of these, so the first
  D3D12 job must measure with ONLY --gow2_frame_times to get Shagg's real numbers (job 021 uncapped 1x: ~9.3 ms).
- Renderer CPU 8.81 ms/frame; GPU 7.46 ms/frame. Draws 2550/frame, dispatches 166, barriers 607 in 323 batches.

## CPU split (ms/frame, renderer thread)
record 2.05 | textures 1.53 | resolve 1.12 | vertex 0.91 | state 0.82 | ownership 0.69 | constants 0.59 | frame 0.52 |
index 0.32 | transfers 0.10
- [V] constants is 2.2 ms when --gow2_renderer_tex_post_constants=true (jobs 023/027/031 had it, 025 didn't): that
  diagnostic switch costs ~1.6 ms CPU. It is not in Play.bat / Play-Best.bat - keep it out of the launcher too.
- [G] record + state + ownership = 3.6 ms = per-draw cost x 2550 draws. Judgment draws ~2x Gears 2 (2550 vs ~1300?),
  so per-draw CPU work matters most here. Candidates: state-change dedupe, descriptor reuse, fewer root updates.
- [G] textures 1.53 ms: per-draw texture binding/lookup; check whether unchanged bindings are skipped.

## GPU by target (ms/frame)
- #5 7e3 HDR scene target: 3.54 ms (1241 draws) - the scene itself, expected.
- resolves 0.86, texture loads 0.49, EDRAM transfers 0.30, depth #4 D24FS8 0.46 (1039 draws).
- Depth transfer pairs every frame [V]: colour->D24S8 aliases at base 720 (1.84 + 1.12 + 0.73 Mpx/frame), all of
  them "clear" transfers (the destination is cleared right after) - [G] a transfer whose target is fully cleared next
  is wasted work; layer 0 could skip it (0.3 ms GPU + its CPU share).
- D24FS8 <-> D24S8 at base 1440 (0.24 Mpx, 2 runs each way) - small.

## Stutter
- First visits: ~325 ms per new pipeline in the driver (DXBC), ~240 pipelines on University cold = the 11.4 s worst
  freeze (job 017) with wait_ms 0; warm 107 ms. The NVIDIA driver cache makes second visits cheap; a shipped
  warm-up (our own cache dir, filled in the background at the menu) is the fix [G], not a pipeline list (never ship).
- Mid-play: play_hold_draws 8 (default) - Island had 77 holds at 1.

## Order I'd propose
1. Real frame time without tools (one short job per game) - baseline for everything else.
2. Skip transfers into targets that are cleared next (layer 0, Gears 2's code; mail them the numbers above).
3. Per-draw CPU (record/state/ownership/textures) - layer 0 again; Judgment is the heaviest test case.
4. Background pipeline warm-up at the menu (all three games).
