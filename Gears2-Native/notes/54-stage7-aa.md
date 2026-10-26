# 54. Stage 7.2 - anti-aliasing

Gears 2 draws without MSAA in most passes (the console's EDRAM 2x/4x is not used for the main scene), so edges stair-step
at any resolution. Two ways, both switches:
- **Supersampling**: `--gow2_renderer_resolution_scale=N` (Stage 7.1) with a window smaller than N x 720 - the presenter
  scales the image down (bilinear; exact 2x2 box when the window is exactly half). No new code.
- **SMAA 1x** (`--gow2_present_aa=smaa`, default off): SMAA (Jimenez et al., MIT, vendored unmodified in
  src/layer0_xenos/renderer/smaa/ with its notice) on the image the swap's gamma pass made, before it goes to the
  window: luma edges -> blend weights (SMAA's area / search tables) -> neighbourhood blending written back in place as
  packed RGB10A2 words, so presenting and frame shots are unchanged. The HUD is included (it is already in the swap
  image). Preset HIGH, no predication, no temporal reprojection.

## Pass criteria (job 195)
- aa off: unchanged (ch10 capped shots match job 194's 1x sweep run where the timing variant matches, else by eye).
- smaa: runs with no error lines; diff against aa off only along edges (most pixels identical); edges visibly smoother
  on crops; GPU cost logged (frame category).
