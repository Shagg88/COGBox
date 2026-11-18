# Experimental native HDR options

The existing Gears 2/3 native HDR paths remain available. Gears 3 compressed LUT sampling defaults to on and can be disabled independently; experimental transport features require explicit launch switches:

| Switch | Default | Purpose |
| --- | --- | --- |
| `gow3_hdr_compressed_lut` | true | Gears 3 native-HDR LUT compress/sample/restore; false restores edge-gradient sampling |
| `gowj_hdr_native` | false | Judgment native grading and post-scene HDR FXAA; exposure readback is unchanged |
| `gow1_hdr_native` | false | Gears 1 native analytic grading and HDR motion blur |
| `gow2_hdr_ui_automatic` | false | Additional bounded screen-space UI variants |
| `gow2_hdr_scene_effects` | false | Selective HDR replay of verified blur passes for Gears 2/3 |
| `gowj_hdr_single_pass` | false | Paired Judgment scene/FXAA; requires Judgment native HDR |
| `gow2_hdr_compact_targets` | false | Visible-height HDR color targets when mixed attachment dimensions are supported |

HDR scene images in all four game paths use the current render-target and visible-region dimensions, not a fixed 3840x2160 size. Internal rendering uses the existing supported scale/mode combinations; presentation may resample that output to a larger display. Hardware image-size limits and the effect allocation budget still apply.

`present_hdr` and the existing `gow2_hdr_native` remain prerequisites. Options are selected at launch. `gow3_hdr_compressed_lut` only affects Gears 3 native HDR and is read when its scene shader is created; restart the game when comparing modes. COGBox exposes this as **Improved HDR LUT sampling** in Gears 3's Display settings, enabled by default and available while HDR is on. Gears 2 uses analytic grading rather than a LUT.

For Judgment and Gears 1, COGBox's **HDR** checkbox also passes the game's native switch (`gowj_hdr_native=true` or `gow1_hdr_native=true`) and, when supported, `gow2_hdr_ui_automatic=true`. This opts into native scene grading and bounded UI together; HDR presentation alone leaves these games on legacy expansion. The launcher checks the selected executable for each switch before sending it, preserving compatibility with older ports, and logs a warning if native HDR is unavailable. With HDR unchecked neither opt-in is sent. The renderer defaults remain unchanged, and paired rendering and compact targets still require separate explicit opt-ins. Restart the game after changing HDR. Release packages need both the updated launcher and game executables built with their native-HDR switches; a launcher update cannot add that support to an older executable.

Compact targets query D3D12 OPTIONS19 support when compiled with verified header revision 616 or newer (Windows SDK 10.0.26100.0 headers were checked; the review build used 10.0.28000.0). Older headers conservatively retain matching-height targets without referencing OPTIONS19. Runtime query failures also retain that layout. The periodic transport-payload diagnostic requires `gow2_hdr_compact_targets`; Vulkan supports this layout. Every HDR draw is scissored to the common visible region. Unsupported D3D12 devices retain the matching-height attachment layout. At 3840x2160, replacing a 3840x6144 FP16 color target with a visible-height target saves 116.72 MiB of texture payload. Judgment retains two visible FP16 images for scene-to-FXAA dependencies, but no-FXAA completion swaps their handles instead of copying. An unused previous-frame encoded proxy may be evicted to preserve the effect budget and is released after successful paired FXAA, so steady paired operation does not retain that third image. These are resource payload calculations; driver VRAM usage also includes heaps, alignment and other resources.

The effect allocation cap remains 128 MiB. Refused dimensions and resident bytes are logged once per resolution rather than silently disabling the paired path. No unsupported allocation is forced through the cap.

Paired scene draws retain the original depth/stencil attachment and tests, and perform their state writes once. Unrelated-target draws do not revoke a pending Judgment scene. Scene-resolve substitution rejects texture-pack replacements and overlays and still checks frame, owner, write generation and guest-memory version.

## 4K HDR comparison

Build Judgment, then run `tools/hdr-ab-judgment.ps1` with `-Variant baseline` or `candidate` and `-Dlss off` or `quality`. Launch the game with COGBox's Play button. The script temporarily appends a comparison block to the launcher's extra arguments, fixes resolution scale to 3 (3840x2160 on these 1280x720 profiles), enables HDR and per-frame CSV logging, and saves the original extra arguments for restoration. Only launch Judgment while that temporary block is active. Verify actual output/upscaler dimensions in each run's log. Other settings, including HDR peak and paper white, remain in the launcher. After testing, run the script with `-Restore`; it checks for intervening edits before restoring the original file.

Both comparison variants explicitly opt into Judgment native HDR and automatic UI to keep grading and UI brightness consistent. Baseline uses the earlier replay/padded layout; candidate enables paired draws and compact targets. This isolates the transport changes within native HDR rather than comparing legacy HDR expansion against native HDR. New application features remain off by default regardless of these temporary comparison settings. Use the same save, warm the same scene, then measure the same 30-second route/camera movement in all four runs. Avoid captures and UI overlays during the timed interval. Include median, p95/p99 frame times, renderer CPU/GPU times, spikes and HDR fallback counters, and record a second pass if shader warm-up affects the first. Shader fixtures are not a whole-game performance claim.


Use `tools/hdr-frame-summary.py` with the frame CSV and explicit `--start-frame` / `--end-frame` markers for the warmed gameplay interval. It reports median/p95/p99 and maximum frame times, average frame-to-frame change, spikes, stalls, and renderer CPU/GPU averages. GPU averages use available positive asynchronous timestamp samples and are renderer timings, not whole-system GPU utilization. Keep menu/loading/warm-up intervals outside the selected window; do not discard stutters inside it.

## Validation and measured results

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

Judgment DLSS remains selectable through COGBox, or with `gow2_upscaler=dlss` / `gow2_upscaler=off`. The tester reported flickering with DLSS selected and its disappearance with DLSS off. Those reports came from the original A/B build, before the resolution-tracking correction below. DLSS remains selectable; the affected menu was subsequently confirmed fixed. Temporary A/B arguments override launcher choices until restored with the comparison script.

## Judgment DLSS resolution tracking correction

DevKit captures showed a native-HDR grading target at visible 2560x1440 while FXAA sampled a 3840x2160 scaled resolve. Another captured frame used matching 3840x2160 resources. The mismatch is sufficient to reject HDR FXAA and explains the repeated reference/native presentation with DLSS selected.

The renderer now switches to the output phase before Judgment's identified grading pass, including menu frames where DLSS cannot evaluate. This keeps grading, HDR, FXAA and subsequent UI at the output scale while leaving earlier scene rendering and DLSS evaluation in their existing phase. HDR resolve validation locates the exact producing target by its lease owner ID rather than selecting a twin from the current phase. Frame, memory-version, write-generation and dimension checks remain intact.

The correction uses the configured scales without fixed 4K dimensions. A display larger than the internal render output is handled by the existing final presentation resampling. This does not add arbitrary internal resolutions or new DLSS ratios: the existing integer-scale modes and backend texture-size limits still apply. In particular, Quality uses the existing 2x-to-3x pair; unsupported pairs retain the existing native-mode fallback and warning. Higher display resolution alone does not increase the 128 MiB scene-effect budget.

Validation: 375 assertions in 22 HDR/texture/render-target cases passed, including lease ownership through all 1x–4x target phase changes and revocation after new writes, new frames or changed memory. Judgment grading fixtures passed on D3D12 and Vulkan. The corrected executable was installed and the tester confirmed that flickering stopped in the affected scene. The follow-up log recorded 1,188 native FP16 frames with the reference count unchanged at 12 startup frames. DLSS was selected but had not evaluated in that scene; reconstruction during gameplay and a monitor-above-5K test remain unverified.

## Gears 1 highlight color correction

The Gears 1 candidate maps extended-grade luminance with PsychoV35, then applies a shared RGB scale to the unclipped game grade. It avoids reconstructing bright effect hue/chroma from a clipped SDR companion. The original shadow offsets, negative clamps and low-range grading remain intact; the earlier black-crush investigation did not add a global shadow lift, so there was no separate black-level correction to revert. The change is confined to Gears 1 and adds no full-frame copies.

D3D12 and Vulkan authored fixtures pass for colored-highlight RGB ratios, peak fitting, original shadow floor/diffuse values, fades and invalid normalization coverage. The candidate was installed and the tester reported that muzzle-flash colors looked better.

## Gears 1 aim-release transition

The post-processing output-phase boundary also covers Gears 1's identified grading pass. The aim-release log showed sporadic skipped DLSS scene resolves, an input-scale 2560x1440 motion-blur viewport, a stale/incompatible HDR resolve warning and new reference presentations, while stable frames rendered post-processing at 3840x2160. Forcing the known grading/motion-blur chain to output scale addresses this mismatch without reusing previous-frame HDR or changing the low-resolution scene/DLSS input phase. The correction retains the highlight-color change and authored shadow grading. The follow-up build was installed and the tester confirmed that the aim-release dark flicker stopped. The inspected follow-up log showed DLSS Quality evaluating successfully (2560x1440 to 3840x2160, zero failed evaluations) and a stable reference-frame count in the latest logged intervals. Other reference frames occurred earlier in the session; this is not a claim that all fallback paths are eliminated.
