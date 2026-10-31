# 03 - Gears 1 on the shared renderer (2026-10-30, jobs 677-688)

Marks: [V] verified here, [S] source, [G] guess.

Gears 1 boots to the title screen (job 677), the menus and chapter 1 play (678), but the first scenes were nearly black.
Gears 1 is the only one of the four games that draws its scene with 2x MSAA in two predicated tiles (1280x512 + 208
rows), a 7e3 (2_10_10_10_FLOAT) scene target at EDRAM base 1024 and a 2x D24FS8 depth at base 0 [V, resolve log].
Four renderer gaps, all in layer 0 (Gears2-Native/src/layer0_xenos/renderer/shadow_renderer.cpp), none hit by Gears 2/3/J:

1. MSAA depth resolves were skipped ("MSAA depth resolves are not in M1"): Gears 1 resolves its 2x scene depth per tile
   and draws it back before lighting - the lights read stale depth. Fix: depth_resolve_ms_cs (one sample of
   Texture2DMS), --gow2_renderer_msaa_depth_resolves (on). [V job 682: 2 per frame; layout matches the reference]
2. Fast clears never recognised: Gears 1's older D3D clears with shader pair 0A6D1DD7767FDF27 / 2E372EA28CC404B7
   (the other games 1E6883FCCDE1F688 / A4A965C189287B99). Found with --gow2_renderer_learn_clear_pair (developer switch,
   off by default), now passed as Gears 1's renderer hints in gow1_app.h. [V jobs 684/685: 4,500 clears / 300 frames;
   the yellow block gone]
3. Colour was only drawn into non-MSAA targets: the whole 2x lit scene was thrown away (its resolve made an empty
   "resolve-only view" target). Fix: --gow2_renderer_msaa2x_colour_draws (on; 4x unchanged), and a colour-only draw
   takes its sample count from the colour target. [V job 686: the scene is lit]
4. 2x colour targets weren't cleared by a resolve's clear or by the frame-start fast clear, so tile 2 started with
   tile 1's picture (the bottom 208 rows repeated the top). Fix: ColourResolveClear and the fast-clear twin handle 2x.
   [V job 687]

Regression: Gears 3 TU6 chapter 2 with fixes 1-3: unchanged, none of the new paths used [V jobs 682, 686]. Judgment /
Gears 2: job 688.

Research [S]: Canary's "Black Shading Fix" (boma) turns predicated tiling off (GUseTilingCode=0, GTilingMode=4; TU5
addresses 0x82F9779B / 0x82F97733, --gow1_tiling_off test switch) - not needed with fixes 1-4. Canary issue #44:
character "black shading" (light environments), blood decal flicker (depth bias), lens flares through walls
(occlusion), white checkerboard in the tutorial: watch for these next.
