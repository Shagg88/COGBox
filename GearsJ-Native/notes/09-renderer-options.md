# 09 - Renderer options on Judgment (jobs 016-019, 2026-10-24)

Marks: [V] verified in our logs / shots, [G] guess. SP_01_University_P chapter boot unless said.

## Frame rate
- 60 fps (--gow2_gpu_vsync=false): steady 62 fps (16.1 ms), game speed right, GPU 7.4 ms [V, job 016 run 1].
- 60 fps + 2x resolution + SMAA: ~56 fps, GPU-bound at 17.5 ms [V, job 016 run 2] (run tools add some cost [G]).
- Uncapped: still 62 fps with vsync off + --gow2_max_fps=-1 + bSmoothFrameRate=FALSE (the edit applied) [V, job 019 E].
  GPU 7.1 ms, so ~140 fps is possible. Judgment's own code caps at 62 [G]: Gears 3 lifted the same cap with a hook
  overriding the max tick rate (--gow3_max_fps); Judgment took that hook out under D7. Shagg's call.

## First-visit freezes (job 017; defaults since 16:15, D11 16:48)
- Before: 41.5 s (wait_ms -1). DXBC + wait_ms 0: worst 11.4 s cold, 107 ms warm.
- Sweep with the defaults (job 019 A, 13 maps): worst hitch <= 0.7 s on 12 maps. Island had 51 stalls (worst 1.0 s,
  10.8 s total) from mid-play holds with play_hold_draws 1 -> now 8 (D11).

## Upscalers (job 019 C; Gears 3's constants: objects c7, camera c5, scene formats 3) [V]
- DLSS DLAA, FSR 3.1.5 native AA, XeSS 2.0.2 native AA: every frame upscaled, 0 failed, camera found every frame,
  object vote agreed. Shots clean against the baseline (no black triangles, no smear). Only a standing-still scene:
  ghosting while moving is untested.
- To do: make c7 / c5 / scene formats Judgment's game_default (as Gears 3 does) once Shagg has seen it in play.

## The game's own graphics settings (job 019 D) [V]
- bUseMaxQualityMode, shadows x2 (MaxShadowResolution 2048, dominant 2688, texels x2), draw distance x2: the edits
  applied (log), but the shots differ only as much as two baseline runs do, GPU time unchanged, and the shadow
  targets stay 1120 wide. The 360 build ignores them -> our own renderer-side versions instead (D10).
- FOV x1.15 (GearCamera.ini FOVAngle): the edit applied; the shot looks a little narrower, but the camera wasn't in the
  same spot - inconclusive. Better: our own Hor+ FOV from the monitor aspect (Gears 3's --gow3_aspect) [G].

## Vulkan (job 022, 18:10-18:14) [V]
Runs (after the repeated-switch fix). University warm: CPU 13.0 ms, GPU 6.36 ms, 145 render passes and 384 full
barriers per frame, 2548 draws. The picture is fine (frames\frame-003000.png, University street) - the black shot-*.png files
are only the desktop capture of a Vulkan window, and the "stage V2 ... no picture" log line is stale (Gears 2 mail 18:32,
patch 0120). The "skipped: export mode is multipass" draws are memexport-only draws that still run. So Vulkan GPU
6.36 ms includes resolves + present and is below D3D12's 7.18 ms; CPU 13.0 vs 8.24 ms is the real gap (per-draw cost
at 2550 draws/frame). Job 023: fixed-step-30 frame pair, D3D12 vs Vulkan, for a picture comparison.

## Uncapped (job 021) [V]
gowj_max_fps -1 (MaxSmoothedFrameRate=1000): 1x ~9.3 ms (~108 fps); 2x + SMAA ~19.5 ms (~51 fps); the test tools cost ~8.7 ms/frame.

## D3D12 vs Vulkan picture pair (job 023, fixed step 30) [V]
Gameplay frames 1200-2640, level start: no black blocks; Vulkan ~17% darker, only on sun-lit surfaces (cobbles, lit
walls, haze) - shadows match. Looks like Gears 3's "missing light spill" (layer 0, Gears 2's to fix; mail 18:45).
Same-settings timing: interval 11.83 vs 14.41 ms, CPU 10.75 vs 13.21 ms, GPU 6.06 vs 6.28 ms. D3D12 stays.

## Vulkan darker lighting - target stats f1800 (job 025, Gears 2 patch 0121) [V]
First difference: the 7e3 HDR target (id 5) right after the opaque/light draws: Vulkan 0.787x D3D12; everything before
it matches. Vulkan also has big negatives in that target after a post pass (-16.9 vs -0.25). Sent to Gears 2 (19:24).

## Vulkan darkening - try 1 (job 031, Gears 2 0125) [V]
The mask texture the dark draws read (t0, 1280x720 8888 resolve) is the same on both APIs. Draw 1180 (a 12-vertex
light quad, same shader/textures/blend) writes its 11.9 highlight on D3D12 and nothing at all on Vulkan. [G] Rejected
fragments - suspect the depth test (func 6) against the D24FS8 float depth (the format Judgment and Gears 3 share and
Gears 2 lacks). Sent to Gears 2 for try 2 (20:25).
