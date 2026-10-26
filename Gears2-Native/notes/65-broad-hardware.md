# 65. Broad hardware support (user 2026-10-22 21:01-21:04)

> "this is going to eventually be distributed on GitHub ... we need to tailor it to a wide range of computer hardware.
> I know I can't use fsr4 or DLSS 5 but I know someone can out there - that's why I want this project to be able to
> support those things." - then: "Start one through six now."

Rule (D35): features are built for the range of PCs players have, not for the test PC. A feature the test PC can't run
still goes in when it switches itself on only where the hardware supports it, falls back cleanly everywhere else, and is
marked **untested on real hardware** until someone with that hardware confirms it. The test PC is an RTX 3080 Ti
(Ampere): DLSS upscaling / DLAA, Reflex, FSR 3.1, FSR frame generation and XeSS (its DP4a path) can be tested here; DLSS
frame generation (RTX 40+), multi-frame generation and DLSS 5 (RTX 50), FSR 4 (Radeon) and XeSS on Arc (XMX) cannot.

## The six items, in order of how many players each helps
1. **Upscaling modes** (Quality / Balanced / Performance / Ultra Performance): render the 3D scene below the output
   resolution, DLSS / FSR / XeSS fill in the detail - helps weaker and older GPUs most. Today we only do native-
   resolution AA (DLAA / FSR native AA). Needs the scene targets at a lower scale than the post-processing / HUD /
   front buffer: per-target scales (Gears 3's resolution_scale_threshold work, its commit cbf5af1, is the start of that
   in layer 0), then the scene resolve upscaled into the full-scale copy. Integer scales first (2x out / 1x in =
   Performance; 3x / 2x = 1.5 Quality; 3x / 1x = Ultra Performance; 4x / 2x Performance), fractional later. Plus: DLSS
   presets pinned (job 270: K for DLAA / Quality / Balanced, M Performance, L Ultra Performance; --gow2_dlss_preset).
2. **DLSS through NVIDIA Streamline**: one integration for DLSS Super Resolution, DLSS Frame Generation (RTX 40+),
   Multi Frame Generation (RTX 50), Reflex, and DLSS 5 (launched 2026-09-03 on RTX 50; integrated through Streamline;
   takes the game's colour and motion vectors - which we already make). Each feature asked of Streamline per GPU and
   offered only where it reports support. Frame generation needs the swap chain / present path (the SDK's presenter)
   and the HUD separated or flagged - the main engineering work. SDK downloaded in job 270 (third_party\streamline-sdk).
3. **FSR frame generation**: in the AMD FidelityFX SDK we already have (amd_fidelityfx_framegeneration_dx12.dll);
   works on any modern GPU, so frame generation for players without an RTX 40/50. Same present-path work as item 2.
4. **Intel XeSS**: the third upscaler, for Arc (XMX) and other GPUs (DP4a). SDK 3.0.2 downloaded in job 270
   (third_party\xess-sdk); same inputs as DLSS / FSR, so it fits behind upscale::Upscaler. Job 271:
   renderer/xess_upscaler.cpp (--gow2_upscaler=xess; launcher "XeSS"). libxess.dll is loaded when XeSS is chosen
   (LoadLibrary, no import library), so the exe starts without it; XeSS's pre-1.3 ratios are forced
   (xessForceLegacyScaleFactors) so its quality modes match ours (1.0 / 1.5 / 1.7 / 2.0 / 3.0); HDR colour uses its
   auto-exposure, Gears 3's 10:10:10:2 scene its LDR flag; the output texture has the scene colour's format (XeSS
   requires it). Also in job 271: the scene-colour check accepts Gears 3's 10:10:10:2 UNORM target (its chat's line)
   and passes hdr = (16-bit float) to every upscaler.
   Job 271 results [V] (TU6 ch02, 1x, uncapped): XeSS upscaled every frame, 0 failed, 198 fps (DLSS 192, FSR 191);
   picture checked against DLSS (frame 2700); with libxess.dll moved away the exe starts and logs "XeSS is off:
   libxess.dll is not next to the exe" - no crash. The DLL reports version 2.0.2: the package is the "XeSS 3 SDK"
   (v3.0.2: SR + frame generation + XeLL), and the super-resolution library inside has its own version number
   (xessGetVersion is the SR library's) [G: consistent with the SDK readme, not stated there in so many words].
5. **The launcher picks what the card can do**: job 270 detects the graphics card (Win32_VideoController), shows it,
   writes it to args.txt (bug reports), and DLSS on a card without RTX falls back to FSR (not SMAA). Later: XeSS / frame
   generation entries per vendor, sensible defaults. Job 271: XeSS entry (falls back to SMAA when libxess.dll is
   missing); Gears 3 gets the FPS bar and ultrawide (--gow3_ultrawide=true; its chat, 21:15). Job 272: DLSS and FSR
   for Gears 3 too (its commit 80d9b9e, job 056: every frame in ch22 / ch8, picture correct); XeSS there once its chat
   checks it. Shared renderer: its --gow2_upscale_object_constants / --gow2_upscale_camera_constant (0 = Gears 2's
   c5-c8 and no camera position; gow3.exe sets 7 and 5) and its GPU-timing fix around texture loads, merged.
6. **Release hygiene for GitHub**: no game files (the .gitignore already excludes game/, out/, build/, generated/,
   third_party/, *.iso, *.xex, *.xexp); job 270 audits the tracked files for anything game-derived. Vendor SDKs are
   never committed; their runtime DLLs ship next to the exe in release builds under the vendors' terms (DLSS: NVIDIA RTX
   SDKs License - binary redistribution allowed inside an application, the SDK must not be made subject to an open-source
   licence (clause 2(e)), so it stays a separate binary next to our MIT code (D14); FidelityFX: its API headers are MIT,
   but its signed DLLs are under AMD's binary-only licence (docs/license.md: redistribution in binary form, no reverse
   engineering, notice reproduced) - an earlier version of this note said "MIT", which was wrong; XeSS: Intel Simplified
   Software License (binary-only, unmodified, notice reproduced); Streamline: MIT for its source, but its shipped DLLs
   are NVIDIA's signed production builds from the release zip (the Git repo has none since SL 2.7.32), and its DLSS
   feature DLLs keep the RTX SDKs licence). THIRD_PARTY_NOTICES.md lists them. A hardware support table
   (below) says what's verified and what's untested.

## Hardware support table (keep current)
| Feature | NVIDIA RTX 20/30 | RTX 40 | RTX 50 | AMD RDNA2 | RDNA3 | RDNA4 | Intel Arc | Other DX12 |
|---|---|---|---|---|---|---|---|---|
| SMAA | [V] 3080 Ti | [G] | [G] | [G] | [G] | [G] | [G] | [G] |
| DLSS DLAA (preset K) | [V] 3080 Ti | [G] | [G] | - | - | - | - | - |
| FSR native AA | [V] 3.1.5 on 3080 Ti | [G] | [G] | [G] 3.1 | [G] 4.1 (SDK 2.3) | [G] 4 | [G] 3.1 | [G] 3.1 |
| Upscaling modes | planned | | | | | | | |
| DLSS FG / MFG / DLSS 5 | - / - / - | planned | planned | - | - | - | - | - |
| FSR frame generation | planned | planned | planned | planned | planned | planned | planned | planned |
| XeSS AA (XeSS-SR 2.0.2) | [V] 3080 Ti (job 271) | [G] | [G] | [G] | [G] | [G] | [G] XMX, untested | [G] SM 6.4 |
