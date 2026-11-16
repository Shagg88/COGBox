# Judgment HDR blend calibration

The captured Judgment scene is exposed before the bridge captures it. Gears 3 captures a different lighting representation before its own grading response. Applying the same raw scene threshold to both is not an exposure/grade-invariant choice.

The previous shared mapper blended between the original grade and the extended result using `smoothstep(0.5, 1.0, max(scene.rgb))`. This kept Judgment's original SDR response at exposed values up to 0.5, even when its continued neutral curve already exceeded reference white. With the captured alternate-profile coefficients, exposed 0.5 gives original neutral linear 0.8694 and continued neutral 1.0773, but the old HDR mix weight is zero. This is a calibration concern, not evidence that every effect is clipped at that point.

## Candidate change

Judgment alone now supplies its continued neutral-grade brightness as the shared mapper's blend signal. The neutral result is reused from the existing grading calculation; it is not an additional texture pass. The original scene remains the input to the actual game grading reconstruction. The mapper's diffuse blend, hue handling and PsychoV35 output remain unchanged; only the value used to choose Judgment's SDR/HDR mix changes.

The optimized branch still preserves the original grade for black, zero fade and captured scene values at or below the protected 0.18 mid-gray anchor. More moderately bright pixels now enter the extended grading path. This has a potential shader-work cost compared with the old 0.5 shortcut; the shared scene/FXAA draws and removal of the recurring conversion pass remain in place. Whole-game frame pacing must still be compared in the same area.

Gears 2/3 grading source, peak setting, UI white and bounded SDR output are unchanged. This does not automatically upgrade other effect passes or remove their valid saturation operations.

## Validation

D3D12 and Vulkan checks pass for both captured Judgment permutations, ordinary authored curves and captured-coefficient fixtures. Coverage includes protected diffuse/gray, exact original encoded SDR output, black, alpha luma, fades, continued neutral/LUT range and 4000-nit peak fitting. Moderately bright fixture inputs (0.35 and 0.40 exposed) produce more extended output than the previous blend. Both actual captured scene/vertex pairs create their FP16 + SDR MRT pipelines on both backends. The staged executable and DLL checks pass.

Artifacts: `work/hdr-judgment-grade-20261116/`. The prior production bridge is saved there as `before-bridge.h`; generated game shaders remain under ignored work folders.

## Live diagnostic context

DevKit was restored from the existing local add-on, then reconnected to Judgment PID 4056. Run `20261116-122715` held reference presentation at 251 while native FP16 frames increased. The paired scene wrote visible FP16 and the original RGBA8 output; paired FXAA wrote the original RGBA8 and padded FP16 output. The final linear SMAA image read by the HDR encoder reached luminance 6.5983 at 200-nit paper white, approximately 1320 nits, with no nonfinite pixels. This reads live resource contents, not a frozen same-frame series. It rules out a universal 203-nit ceiling in that captured scene, not per-material or per-effect limitations. The DevKit resource analyzer could not read the 3D LUT (it currently accepts 2D views only).

## Installation

The verified candidate is installed at `GearsJ-Native/build/Release/gowj.exe`, SHA256 `AB4D2D00FF28DDAE7AD117AC6F2AE3D00952F314A1A880323418DB64CF16EC6B`. The previous executable is saved at `G:\COGBox\work\hdr-judgment-grade-20261116\before-install\20261116-123820`. Runtime DLLs and Gears 2/3 installations were not changed. Live appearance and frame-pacing validation are pending the next same-scene run.

The installed candidate's next run (`20261116-123829`) compiled and presented native HDR successfully. At the sampled gameplay interval, native frames reached 2,149 while reference frames stayed at 251; 2,156 paired scenes and 2,154 paired FXAA draws were recorded. This confirms activation and preserved transport, not subjective lighting coverage or a controlled performance comparison.

A DevKit readback of the installed candidate's final FP16 encoder input contained finite values in all 8,294,400 pixels, no negative RGB and maximum luminance 6.6524 reference whites (about 1330 nits at 200-nit scene white). This is a different live scene/frame from the earlier capture and must not be used as an image-matched before/after brightness measurement. Saved analysis: `work/hdr-judgment-grade-20261116/calibrated-final-analysis.json`. User assessment of the affected lights and frame pacing remains pending.
