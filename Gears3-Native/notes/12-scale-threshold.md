# 12 - Per-target resolution scale threshold (notes/53b in layer 0 comments), 2026-10-22

## Why
- x2 (`--gow2_renderer_resolution_scale=2`) gives Gears 3 a green cast over most of the scene (job 033, 038 D1/D2).
  MSAA transfer paths off: no change (job 038 D1/D2) [V].
- The reference emulators have the same class of problem at higher scales. Two community answers [V, read]:
  - Canary game-patches "Resolution Scaling Fix / Disable All Post-Processing" (boma, 0x827B8764/827B88C0/827B8BA8):
    "Dramatically changes the overall look of the game, but fixes all edge-detection overcorrection and color
    smearing" - a game patch that switches post-processing off. Not used (Shagg: no hacky patches).
  - Edge 74db632ab (2026-07-16, goldislead) `draw_resolution_scale_threshold`: surfaces with pitch at or below the
    threshold render at 1x, "downscaled post-process chains that gain nothing from upscaling and often break from it";
    the Gears 3 tip is 364 (minimum) or 512 (safer).
- Our 1x and x2 frames of ch0 frame 1200 (job 033): the areas green at x2 are the ones too dark at 1x - the shadow
  projection bug (notes/10) may be part of it [V side by side; G cause].

## What (commit cbf5af1)
- `--gow2_renderer_resolution_scale_threshold=N` (default 0 = every target scaled, as before).
- RenderTargetCache::ScaleFor(pitch, msaa) -> 1 at or below N; targets are created at their own scale.
- Draws: viewport/scissor at the bound targets' scale; param gen's pixel scale in system flags w bits 8-11
  (ndc_scale.w stays the scaled-texture scale for texel maths).
- Resolves: rectangles in the target's scale; a 1x target writes only the 1x copy (its pages stop being
  scaled-current, so textures load them at 1x - Edge does the same for 1x1 resolves).
- EDRAM transfers carry the source scale (t3.y): 2x -> 1x takes the top-left host sample, 1x -> 2x repeats.
- Not yet: occlusion-query sample counts still divide by the global scale squared (queries are off by default).

## Test: job 042 (draft) - ch0 x2 at 0 / 364 / 512, ch17 and ch8 x2 at 512.

## Job 042 [V]: thresholds 364 and 512 both remove the x2 green tint (ch0 frame 1200 colour means equal 1x).
## Job 044 [V] (x2, memexport, uncapped): threshold 1120 keeps the shadow maps (pitch 1120) and half-size buffers
(640) at 1x too: ch8 75.7 -> 91.5 fps, ch22 104.6 -> 140.5 fps; GPU ms/frame ch22 ~9.3 -> ~6.0 (resolves 3.9 -> 1.4).
Frames look correct. Play-Best.bat uses 1120. The CPU texture path test (gpu_texture_load=false) ran at ~7 fps -
not an option. Remaining GPU time is mostly texture loads (2.4-7.6 ms) - the next target.
