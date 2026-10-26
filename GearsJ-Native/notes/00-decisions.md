# Judgment decisions

Marks: [V] verified, [S] read in source, [G] guess.

D1 (2026-10-23) Fresh Judgment project, ours, on the shared Gears 2 renderer (layer 0),
   mailbox and job queue. Borrow manifest hints from OverkillLabs3/gears-judgement-recomp
   (BSD-3-Clause, notice kept).
D2 (2026-10-23) Prep only. No codegen, builds or PC jobs until Shagg says go.
D3 (2026-10-23 23:34) Release: USA/Europe disc (media 3528321A, 0.0.0.3) + Title Update 4. Shagg chose TU4. Remap OverkillLabs3's base-disc addresses once, first, before any hook work.
D4 (2026-10-24 08:17) PC lock: Judgment uses the shared lock in place, Gears3-Native\tools\sharedpc\pclock.ps1,
   Owner 'judgment' (Shagg approved; Gears 3 agreed 06:10). Our only writes there: pc.lock, status.txt,
   waiting-judgment.txt, last-take/last-release-judgment.txt. Running PC jobs still waits for Shagg's go.
D5 (2026-10-24 08:27, Shagg) The outside-code rule (04:22) covers projects that are NOT Shagg's. Gears 2, Gears 3
   and Judgment are all his own projects: code from any of them may be copied freely between them. If we spot
   something they did wrong, we suggest the fix to that chat. Other people's projects (OverkillLabs3,
   santiquiroz, KiltedKernel, Canary, ...) stay reference only.
D6 (2026-10-24 09:21, Shagg) Every run carries as many fixes and diagnostics as possible. In practice: each build
   takes every ready hint/table batch; tools\run.ps1 adds frame times, hitch report, draw stats, frame shots and an
   image dump to every run (unknown switches are ignored by the SDK); boots run both renderers.
M1 (2026-10-24 09:49) Milestone: TITLE SCREEN on both renderers (job 007), 0 errors in 180 s each, steady 30 fps,
   attract-mode cinematic after ~2.5 min idle. Our renderer's title matches the SDK's by eye. 187 hints, 5 jump tables.
D7 (2026-10-24 10:11, Shagg) No hack patches or settings, especially where we can build it better ourselves. For
   Judgment: engine settings go through the game's own ini (MaxAnisotropy, PoolSize, MaxSmoothedFrameRate /
   bSmoothFrameRate, TEXTUREGROUP MipFilter, MipFade speeds) - no register overrides; code hooks only put back what
   recompiling removed (the empty cctpl/cctpm GPU-wait step, the query-poll spin) or read timings; no memory pokes
   (the community show-FPS byte is not used; layer 0's FPS bar is ours).
D8 (2026-10-24 10:38/10:46, Shagg) Short test runs: end each run ~30-40 s after gameplay appears, no comparison runs
   unless needed. Menus route reaches gameplay at ~105 s (140 s run); chapter boot at ~175 s (210 s run).
D9 (2026-10-24 14:58, Shagg) Don't spend PC jobs re-proving options that already work in Gears 2/3 on the same layer 0
   (60 fps, 2x, SMAA, ...): assume they work on Judgment and let Shagg's play test show otherwise. Jobs are for
   Judgment-specific unknowns.
D10 (2026-10-24 15:31, Shagg) Hidden graphics options: keep the ones that visibly help without costing too much; where
   we can build our own version that beats the game's option, build ours instead (job 020 picks the candidates).
   Ideas [G]: shadows - render the game's shadow maps at the render scale in the renderer (today the 1120 threshold
   keeps them 1x) instead of only raising the ini caps; FOV - our own Hor+ FOV from the monitor's aspect (Gears 3's
   --gow3_aspect) instead of a flat multiplier; anti-aliasing - SMAA / DLAA / FSR native already beat the engine's TAA.
   Renderer-side pieces go to Gears 2 as proposals (layer 0 is theirs).
D11 (2026-10-24 16:48, Shagg) Mid-play pop-in vs pause: pause only for big batches - play_hold_draws 8 (Gears 2's launcher /
   layer 0 default). With 1, Island's first visit stalled 77 times (10.8 s, job 019). Loading screens still wait for all.
D12 (2026-10-24 16:16/17:20, Shagg) Bundle tests: one job per PC-lock turn wherever the tests can share a build - each
   separate job waits its own turn behind Gears 2 / Gears 3 (often 20-40 min).

D13 (Shagg 21:46-22:07, via Gears 2 mail 22:08): the stutter rules are lifted - "brief pop-in", never wait on the driver
(layer 0 patch 0132 defaults: no load hold, play_hold_draws 0, pipeline/shader wait 0, no 0068 prebuild). Supersedes D11:
Judgment no longer sets play_hold_draws / pipeline_wait_ms. No DXBC shader-shrinking work (Shagg 22:07).
