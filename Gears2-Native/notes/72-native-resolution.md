# 72 - "Match my monitor" (native resolution), Shagg 23 Oct 23:37-23:44; design agreed 24 Oct 03:07

Shagg: the game should come out at the screen's exact size (1080p, 1440p, 1600p, 4K...), no shrinking, no black bars,
"done right, not a hack" (03:04). Agreed design (03:07, "go with your recommendation"):

## Why whole-number render scales [V, design]
Every render target, resolve and EDRAM transfer works on EDRAM tiles; scaling them is exact only at whole numbers.
Canary and Edge offer only whole-number draw resolution scales for the same reason. A fractional scale (1.5x for
1080p) puts tile edges on half pixels: seams, shimmer and wrong copies. So "native" is done in two steps:

1. **Render scale chosen from the monitor** (`--gow2_renderer_resolution_scale=0`): the smallest whole number N whose
   image covers the monitor the game is on - fitted by the 16:9 shape (N = ceil(min(W / 1280, H / 720))), or by width
   when the game image is wider than 16:9 (`gow2_present_frame_aspect`, ultrawide). 1080p -> 2, 1440p -> 2 (exact),
   1600p 16:10 -> 2, 4K -> 3, 5K -> 4; capped 1-4.
2. **Exact-size present** (`--gow2_present_native_size=true`): after the swap's gamma, SMAA and sharpening (before the
   HDR encode), the image is resampled to the largest game-shaped rectangle inside the window, so the presenter shows
   it 1:1. Shrinking: an exact box filter (each output pixel averages the source pixels it covers, by overlap) - no
   aliasing, no extra blur; enlarging (rare): bilinear. Averaging in **linear light** (sRGB curve), so fine detail keeps
   its brightness. At an exact multiple (1440p at 2x) the pass is skipped. Shader: builtin_shaders.h
   kPresentResampleCs; gn_vktest checks its numbers against a double-precision reference.

Launcher: "Render resolution: Match my monitor (native)" sets both switches.

## Window and monitor sizes (layer 0, any game)
`xenos/display_size.h`: the app calls `xenos::display::SetWindow(native handle)` and `SetSurfaceSize(w, h)` when its
window exists and on every pixel-size change; the renderer reads `SurfaceSize` (present size) and `MonitorSize`
(Win32 MonitorFromWindow; the primary monitor before the window exists). Gears 2: gow2_app.h OnCreateDialogs /
OnWindowPixelSizeChanged. Gears 3 makes the same two calls in its app and can use `MonitorSize` for its own aspect
hooks (`--gow3_aspect=auto`).

## Next
- The game's shape on a monitor that isn't 16:9 (no bars): Gears 2's aspect hooks generalised from the 21:9 patch to
  any wider aspect (scene height = 1280 / aspect, on the EDRAM tile step), as the Gears 3 chat did for Gears 3.
- Option 2 for weaker GPUs: DLSS / FSR / XeSS outputting the exact monitor size from a lower render scale.
