# 14 - Upscalers: ch8 cutscene black triangles (PARKED by Shagg, 2026-10-22 23:33)

"Let's forget the upscaler for now" - DLSS / FSR / XeSS stay "experimental" for Gears 3 in the launcher (Gears 2's
tooltip: close-up cutscenes, chapter 8, black triangles). Everything below is so the work can resume cleanly.

## Facts [V]
- ch8 cutscene close-ups: black triangles on the characters with every upscaler; perfect without one, and perfect
  with the upscaler on but --gow2_upscale_jitter_scale=0 (job 059). So the per-frame sub-pixel jitter is the cause.
- Not fixed by: --gow2_upscale_dejitter_depth (patch 0001, job 059), --gow2_upscale_jitter_scope=1/2 (job 060, 0 more
  draws), =3 (patch 0002, job 062, 0 more draws). All stay in the shared renderer, default off.
- The job 058 probe: every perspective draw into the scene depth already shares the camera c0-c3 and is jittered.
- ch8 also draws a half-resolution 4x MSAA target (7e3, pitch 640, base 720) and per-object shadow depth targets
  (1120x688); pitch <= 1120 targets stay 1x under the resolution scale.

## Leads not yet tested
1. [S] Xenia Canary src/xenia/gpu/draw_util.h: UE3 adds its own sub-pixel viewport offsets ((-0.25, 0.25) for 2x,
   (-0.125, -0.125) for 4x tiling / MSAA) to align sample 0 with non-MSAA depth when it restores depth; "restored depth
   can fail subsequent depth tests and leave parts of the scene unshaded". [G] Our jitter never reaches the half-res
   4x MSAA target (other depth key), while the depth it restores from is jittered -> per-triangle depth failures.
   Test: "scope 4" - also jitter camera-c0-c3 draws into other depth targets, scaled by target width / scene width
   (half-res 640-pitch 1x target vs 2560 host scene = 1/4 of the host-pixel jitter). Draft started, not finished.
2. [S] softsoundd's dxvk-remix-mirrorsedge (UE3 DLSS): classifies UE3 passes and skips jitter for depth prepass,
   shadow depth, SceneCapture, depth-test-disabled translucency and fullscreen post; camera / object transforms from
   UE3's reserved shader constants via CTAB parsing. Read documentation/UE3Compatibility.md
   (https://github.com/softsoundd/dxvk-remix-mirrorsedge) before the next test.
3. Diagnostics ready in the shared renderer: --gow2_upscale_passthrough (patch 0003, Gears 2 job 283): jitter on,
   upscaler skipped - shows whether the triangles are already in the jittered scene.
4. [S] UE4 applies jitter in the projection matrix (TemporalAAProjectionJitter), not the viewport.

## 2026-10-24 09:5x - research resumed (Shagg: "more research on the fsr and DLSS")
- Lead 2 ruled out for us [S]: softsoundd's dxvk-remix-mirrorsedge is RTX Remix (path-traced). Its UE3Compatibility.md
  only classifies passes to keep them OUT of the ray-traced scene. DLSS runs on Remix's own render, so there is no
  raster jitter scheme in it to borrow.
- Lead 1 strengthened [S]: Canary cc4981a "Rasterize at Xenos MSAA sample positions" (draw_util.h) says UE3 titles add
  (-0.25, 0.25) / (-0.125, -0.125) to the viewport for 2x / 4x to align sample 0 when RESTORING DEPTH into MSAA
  targets; with PC sample positions "restored depth can fail subsequent depth tests and leave parts of the scene
  unshaded". ch8 draws a half-size 4x MSAA target, and the upscaler jitter widens any misalignment [G].
- Gears 2's patch 0089 (--gow2_renderer_msaa_sample_positions=xenos) is in play build 116b (Gears 2 107e968).
- Test, Shagg plays, no scripted run: Test-DLSS-ch8-A.bat (standard positions, expect the triangles) and
  Test-DLSS-ch8-B.bat (xenos). They differ only in that switch.
  - B clean -> sample positions are the cause. Then FSR, then on to the launcher.
  - Both bad -> next is scope 4 (jitter the other depth targets, scaled), lead 1's other half.

## 2026-10-24 11:48 - FIXED in a test build (jobs 117-120b) [V]
- Ruled out:
  - 117: Xenos MSAA sample positions;
  - 118: character and all dynamic shadows (Shagg watched it too: cutscene only, not gameplay);
  - 119: the engine's cutscene temporal AA via ini.
- Look: whole character triangles lose their lighting.
- Cause: the renderer jitters only until the first scene colour resolve (it runs the upscaler). ch8 cutscenes then
  draw ~170 more camera draws per frame into the scene depth (later lighting passes), unjittered, so they fail
  depth-equal.
- Fix G3-0120 (patches/to-gears2/0120-upscale-late-draws.patch): count those draws and jitter them with
  --gow2_upscale_jitter_late. FSR and DLSS ch8 frames 1800 / 2400 clean, no-upscaler run unchanged.
- Sent to Gears 2 (owner of layer 0), proposing default on. Then: DLSS / FSR lose "experimental" for Gears 3, and
  XeSS gets offered (one check run then).
- 12:51 play build 121 [V]: Gears 2 0106 (0175e59) + Gears 3 game default jitter_late=true. ch8 XeSS and DLSS clean
  (frames 1800 / 2400), so all three upscalers are clean. Launcher label change asked of Gears 2 (after their Vulkan work).
