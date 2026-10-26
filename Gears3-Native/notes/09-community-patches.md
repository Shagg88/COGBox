# 09 - Community patches for our Gears 3 version (catalogue only - nothing applied)

Source: xenia-canary/game-patches, patches/"4D5308AB - Gears of War 3.patch.toml" (hash EF4AD5E9DD1982ED = our base
disc default.xex; D7). Fetched 2026-10-22 08:55. Rule (Shagg / roadmap): faithful by default; enhancements only as
opt-in options, and patches only when needed. Already in layer 3 (off by default): 16x aniso (0x82969614),
show FPS (0x834290D8/0x834290EC), query-poll yield (0x82961994, ours).

| Patch (author) | Writes | First addresses |
|---|---|---|
| Unlock FPS (illusion, boma) | 2 | 8253D9B0, 82AF66BF |
| Occlusion Query Fix (boma) | 2 | 827B2BD0, 827B6C58 |
| Disable Lens Flares (illusion) | 1 | 82861FD0 |
| Disable Ambient Occlusion (illusion, boma) | 2 | 82762C54, 827C9628 |
| Disable Motion Blur (boma) | 2 | 82430640, 827A2BCC |
| 16x Anisotropic Filtering (boma) | 1 | 82969614 (in layer 3) |
| Resolution Scaling Fix / Disable All Post-Processing (boma) | 3 | 827B8764, 827B88C0, 827B8BA8 |
| Disable Depth of Field (boma) | 2 | 824BAA50, 82593DC4 |
| Disable Bloom (boma) | 1 | 82593DC0 |
| Performance A: Low Settings, no dynamic shadows (Sowa_95) | 2 | 827CE810, 8236C98C |
| Performance B: Smart Shadow Cuts (boma) | 12 | 827D0878, 82301218, ... |
| Show FPS (Sowa_95) | 2 | 834290D8, 834290EC (in layer 3) |
| 21:9 Ultrawide (boma) | 11 | 825F8650, 820CDE2C, 82BC9D84, ... (needs "Disable Coalesced Hash Check" + a fixes zip in Canary) |

Notes: Gears 2 learned that emulator-era patches can break a native final pass (motion-blur patch -> fog only) -
implement such options inside our renderer where possible. Gears 2's frame-rate and ultrawide options (their notes
56, 57) are the model for ours. Credit the authors in any option description.
