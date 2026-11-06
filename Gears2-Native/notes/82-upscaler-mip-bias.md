# 82: Upscaler mip bias

An upscaling mode draws the scene below the output, but textures pick their mips from the scene's own pixel size, so they
come out as blurry as the lower resolution even though the upscaler rebuilds detail. The usual answer is a negative mip
bias of log2(scene width / output width).

`--gow2_upscale_mip_bias` (default `auto`; `off`, or a number):

| Mode at a 3x output | Scene | Bias |
|---|---|---|
| DLSS Ultra Performance | 1x | -1.58 |
| DLSS Quality | 2x | -0.58 |
| Native (DLAA), SMAA T2x, no upscaler | output | none |

- `TextureCache::SetMipBias` adds it to textures with mips only; rendered targets have none and are left alone.
- It covers the scene part of the frame only (`UpdateMipBias`, at each `SetPhaseScale`): post-processing and the HUD are
  drawn at the output scale. Changing it invalidates the texture cache's slot memo and the renderer's texture memo.
- F2 toggles it live with `--gow2_ao_debug_keys`.

## Results

Gears 2 launch chapter 1, 4K, CACAO low, one high-end GPU:

| Mode | GPU time, off -> auto | fps, off -> auto |
|---|---|---|
| DLSS Ultra Performance | 2.62 -> 2.64 ms | 263 -> 266 |
| DLSS Quality | 3.44 -> 3.45 ms | 268 -> 265 |

Within the variation between runs. By eye, toggled live: clearly better at Ultra Performance, imperceptible at Quality.

Not looked at: other levels, Gears 3, Judgment, Gears 1, FSR, XeSS, Vulkan. The risk to watch for is shimmer on fine
patterns (grates, fences) in motion; `off` or a smaller number is the way out.
