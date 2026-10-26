# 40 - Lessons from gears-judgement-recomp (OverkillLabs3)

Repo: https://github.com/OverkillLabs3/gears-judgement-recomp (v0.1.0 5 Oct 2026, v0.2.1 by 11 Oct; read 2026-10-21
from a shallow clone of main). BSD-3-Clause: code may be borrowed with its notice kept (THIRD_PARTY_NOTICES). Same SDK
(rexglue 0.10.0) and same starting point (a retail default.xex) as ours; runs on the SDK's emulated renderer with an SDK
patch for FSR 1 / SMAA / DLSS hooks.

| # | Lesson | Their file | For us | When |
|---|---|---|---|---|
| 1 | Independent confirmation of our SDK patch 1 (InputSystem race: concurrent XamInputGetState -> heap corruption in the device list; their "crashes at title / mission start") | src/gowj_xam_input_guard.cpp | strengthens the upstream report (still waiting on GitHub + "post it") | upstream list |
| 2 | Xenia Canary game patches turned into post-codegen edits behind cvars (for Gears 2 only the few patches in the table at the end are ported) (each patched word verified against the original instruction first), so every patch is switchable without a rebuild | tools/patch_game_patches.py, src/gowj_game_patches.cpp | the D23 reference patch set (we did the black-shading fix by hand); ours would use manifest mid-asm hooks, same idea | soon |
| 3 | Ninja can link a stale object after a post-codegen rewrite (measured: a 9-hour-old object shipped without the patch); they build twice and fail loudly if a generated source is newer than its object | tools/check_objects_fresh.py, scripts/build.ps1 | we inject at codegen time (manifest), so less exposed; add the freshness check to tools/build.ps1 anyway | soon |
| 4 | ~500 lines of manifest function-entry hints for a UE3 Gears game, incl. Bink decoder jump tables (auto-detection found 4 of 11 entries) | gowj_manifest.toml | if Gears 2 videos (Bink) misbehave, check the same pattern; head start for Gears 3 | when hit / Gears 3 |
| 5 | Performance: PGO builds of the recompiled code (with a periodic profile flush because the title hard-exits), a persistent D3D12 pipeline library, opting out of Windows power throttling (EcoQoS) | src/gowj_pgo.cpp, gowj_pso_library.cpp, gowj_system_tuning.cpp | cheap wins | performance tuning |
| 6 | Keyboard and mouse: raw input mapped to the gamepad, frame-rate-independent mouse look, engine mouse smoothing and pitch re-centring patched out, FOV option; known gap: no menu cursor | src/gowj_kbm_input.cpp, gowj_coalesced.cpp, tools/patch_raw_look.py, patch_mouse_smoothing.py | same work needed for Gears 2 (UE3, close engine version) | soon (layer 2/3) |
| 7 | Their limits are the emulated renderer's: internal resolution above 2x breaks, CPU-limited at 90-110 FPS, first-visit pipeline stutter | README | supports the native renderer (Stages 3-6) as the way past those walls | - |
| 8 | Upscaling on the emulated renderer as one SDK patch (FSR 1, SMAA) | gpu/patches/0001-upscaler-and-overrides.patch | not planned: the goal is a native renderer with no GPU emulation in the shipped build (user, 03:37); the emulated renderer stays only as the developer-build reference | not planned |

Borrowing rule (unchanged): BSD code keeps its notice; our code stays MIT; credit in THIRD_PARTY_NOTICES.md.

Goal restated (user, 2026-10-21 03:37): as native as possible, not relying on the emulator in the end. Lessons 2-6 are
all non-emulator work (patches, build, input, performance) and carry over to the native renderer unchanged.

## Gears 2 game patches: which to port (user, 2026-10-21 03:58)

Each patch site in xenia-canary/game-patches "4D53082D - Gears of War 2" was disassembled from our image dump
(out\image\default-image.bin) to see what it really changes. Result: most patches are emulator workarounds or cheap
hacks, so only a few are ported; the switch mechanism stays, for those only.

| Group | Patches | What they really do | Decision |
|---|---|---|---|
| Emulator workarounds | AO Upscaling Fix, AO Disable, DoF A/B, Disable Motion Blur, Texture Streaming Fix, Smart Shadow Cuts, Disable Bloom, Disable Lens Flares | force a setting load to 0 (effect off), or work around upscaling artefacts / emulator speed; Smart Shadow Cuts lowers shadow resolution and drops shadows | not ported: our renderer must draw these effects correctly |
| Cheap hacks for real features | Unlock FPS (nops the frame-time clamp load at 824A2E94 plus an immediate 2->1 at 8298CFB3), 21:9 and 16:10 (code caves; 21:9 renders 1280x550) | known timing risks; lower resolution | not ported; a proper frame-rate unlock (engine timing tested) and true widescreen go on the Stage 7 list |
| Keep | Black Shading Fix (already in: hook at 82806A44, the `stw r11,0x2ea4(r6)` that sets GUseTilingMode), 16x AF (one immediate), Show FPS (data), Coalesced hash check (modding) | needed for P3 / harmless dev aids | Black Shading on; the rest as switches, off by default |

Notes: several "be8/be16" patches are immediates inside instructions (e.g. 8298CFB3 is the low byte of `li r11,2`), not
data, so they also need codegen hooks, not memory writes. The reference comparisons keep only the black-shading fix (D23).
