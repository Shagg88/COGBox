# Experimental native HDR options

The existing Gears 2/3 HDR behavior remains the default. New renderer features require explicit launch switches:

| Switch | Default | Purpose |
| --- | --- | --- |
| `gowj_hdr_native` | false | Judgment native grading and post-scene HDR FXAA; exposure readback is unchanged |
| `gow1_hdr_native` | false | Gears 1 native analytic grading and HDR motion blur |
| `gow2_hdr_ui_automatic` | false | Additional bounded screen-space UI variants |
| `gow2_hdr_scene_effects` | false | Selective HDR replay of verified blur passes for Gears 2/3 |
| `gowj_hdr_single_pass` | false | Paired Judgment scene/FXAA; requires Judgment native HDR |
| `gow2_hdr_compact_targets` | false | Visible-height HDR color targets when mixed attachment dimensions are supported |

`present_hdr` and the existing `gow2_hdr_native` remain prerequisites. Options are selected at launch. The improved Gears 3 LUT sampling remains enabled without a separate switch, as requested after review; Gears 2 uses analytic grading rather than a LUT.

Compact targets query D3D12 OPTIONS19 support; Vulkan supports this layout. Every HDR draw is scissored to the common visible region. Unsupported D3D12 devices retain the matching-height attachment layout. At 3840x2160, replacing a 3840x6144 FP16 color target with a visible-height target saves 116.72 MiB of texture payload. Judgment retains two visible FP16 images for scene-to-FXAA dependencies, but no-FXAA completion swaps their handles instead of copying. An unused previous-frame encoded proxy may be evicted to preserve the effect budget and is released after successful paired FXAA, so steady paired operation does not retain that third image. These are resource payload calculations; driver VRAM usage also includes heaps, alignment and other resources.

The effect allocation cap remains 128 MiB. Refused dimensions and resident bytes are logged once per resolution rather than silently disabling the paired path. No unsupported allocation is forced through the cap.

Paired scene draws retain the original depth/stencil attachment and tests, and perform their state writes once. Unrelated-target draws do not revoke a pending Judgment scene. Scene-resolve substitution rejects texture-pack replacements and overlays and still checks frame, owner, write generation and guest-memory version.

## 4K HDR comparison

Build Judgment, then run `tools/hdr-ab-judgment.ps1` with `-Variant baseline` or `candidate` and `-Dlss off` or `quality`. Launch the game with COGBox's Play button. The script temporarily appends a comparison block to the launcher's extra arguments, fixes resolution scale to 3 (3840x2160 on these 1280x720 profiles), enables HDR and per-frame CSV logging, and saves the original extra arguments for restoration. Only launch Judgment while that temporary block is active. Verify actual output/upscaler dimensions in each run's log. Other settings, including HDR peak and paper white, remain in the launcher. After testing, run the script with `-Restore`; it checks for intervening edits before restoring the original file.

Both comparison variants explicitly opt into Judgment native HDR and automatic UI to keep grading and UI brightness consistent. Baseline uses the earlier replay/padded layout; candidate enables paired draws and compact targets. This isolates the transport changes within native HDR rather than comparing legacy HDR expansion against native HDR. New application features remain off by default regardless of these temporary comparison settings. Use the same save, warm the same scene, then measure the same 30-second route/camera movement in all four runs. Avoid captures and UI overlays during the timed interval. Include median, p95/p99 frame times, renderer CPU/GPU times, spikes and HDR fallback counters, and record a second pass if shader warm-up affects the first. Shader fixtures are not a whole-game performance claim.


Use `tools/hdr-frame-summary.py` with the frame CSV and explicit `--start-frame` / `--end-frame` markers for the warmed gameplay interval. It reports median/p95/p99 and maximum frame times, average frame-to-frame change, spikes, stalls, and renderer CPU/GPU averages. GPU averages use available positive asynchronous timestamp samples and are renderer timings, not whole-system GPU utilization. Keep menu/loading/warm-up intervals outside the selected window; do not discard stutters inside it.

## Review status and measured results

The review diff is assessed against upstream `dev`. Retargeting PR #326 from `main` to `dev` remains outstanding: the GitHub integration rejected the attempted update with HTTP 403. Local changes remain on the existing HDR branch and have not been committed or pushed by this review pass.

The pending-scene ownership, paired depth/stencil, texture-pack rejection and allocation-warning changes are implemented. Depth/stencil and real texture-pack fixtures passed on D3D12 and Vulkan; the HDR/texture unit selection passed 247 assertions in 19 cases. Changed C++ ranges were formatted with clang-format while retaining comment text. Machine-specific session notes were removed from `docs/`; this document describes portable behavior.

Four manual 30-second Judgment samples were recorded at 3840x2160 HDR. DLSS Quality used 2560x1440 input. Baseline and candidate used the same executable, native HDR and UI settings; the comparison toggled paired rendering and compact targets together.

| Layout | DLSS | Average FPS | Median frame ms | p95 ms | p99 ms | Maximum ms |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Baseline replay/padded | Off | 89.0 | 10.93 | 14.84 | 22.12 | 27.04 |
| Candidate paired/compact | Off | 92.9 | 10.21 | 14.52 | 15.52 | 136.94 |
| Baseline replay/padded | Quality | 99.0 | 9.47 | 13.59 | 20.55 | 34.90 |
| Candidate paired/compact | Quality | 88.1 | 10.70 | 15.66 | 21.46 | 30.64 |

These are single manual samples, not deterministic benchmark results. The candidate without DLSS includes one 137 ms stutter; it was retained in the statistics. The candidate Quality run was slower in this sample and the tester reported both whole-scene brightness/color flicker and object/shadow flicker. Flicker persisted after switching paired rendering and compact targets off, so those switches do not explain the entire issue. The candidate is not validated for release or for a no-regression claim.

The follow-up baseline log confirms both switches were disabled. FXAA shader `16CBC2E3D5BEC0A5` reported an initial pending/failed pipeline and later stale/incompatible HDR resolve input. Presentation repeatedly used the reference path. The scene allocation also changed between output-size and input-size dimensions. These observations identify HDR input tracking and resolution transitions for investigation; the log's combined rejection reason does not establish which lease or compatibility condition failed, nor does it establish the cause of object/shadow flicker. DLSS reported zero upscaled frames in that follow-up scene, so selection of DLSS alone is not evidence that its reconstruction caused the flicker.

Transport texture payload decreased from approximately 243 MiB (180 MiB padded scene plus 63 MiB proxy) to 126 MiB (two visible-height images) in the paired/compact run. This confirms the allocation saving, not visual correctness or a speedup. The baseline comparison is the current review build with experimental switches disabled, not an older executable from before the review. Reusing the game's existing images to remove dedicated HDR allocations altogether has not been implemented or verified.
