# Gears 2 / Gears 3 HDR grading review (2026-11-16)

## Gears 3

The production Gears 3 scene bridge now selects max-channel compress/sample/restore instead of edge-gradient extrapolation. This follows the architecture reviewed in RenoDX UE3 mods and already applied to Judgment; see [the source review](judgment-renodx-ue3-review.md). The original Gears 3 LUT, Xbox fetch adjustment, gamma-domain coordinates, tetrahedral interpolation and gamma decode remain in use.

Positive linear input with maximum channel at or below 0.5 passes directly to the original LUT reader. Above that knee, one RGB scale brings the input into range with a shifted Reinhard shoulder, matching identity value and derivative at the join. Dividing the decoded LUT result by the same scale restores HDR range. This removes dependence on a useful gradient within the outer four LUT cells. Black LUT outputs stay black. A constant nonblack LUT can carry restored source brightness; it is no longer constrained to stay constant for all HDR inputs.

The original scene program still supplies the reference grade. Existing curve extension, grain, luminance boost, hue/diffuse protection, PsychoV35 and fade handling remain unchanged. The old gradient function is retained for offline comparison; the production wrapper calls the compressed function. There is one tetrahedral evaluation (four loads) instead of up to five (20 loads), with no extra pass or allocation. This is a structural cost reduction, not a whole-game FPS guarantee. LUT compression changes which bright-region grade is sampled and needs visual comparison in gameplay.

## Gears 2

The captured final scene profile uses analytic exposure, contrast, channel mixing, black floor and power curves, not a 3D LUT. Adding a LUT sampling method would not address this path. Its existing value-and-derivative curve extension is retained without a grading change. Both executables were rebuilt and installed; Gears 2 receives no new LUT algorithm.

## Validation

- D3D12 and Vulkan production-transform GPU fixtures pass for Gears 3 and Gears 2 at curve parameters 0.25, 1, 1.4 and 4, including diffuse/shadows, highlight separation, saturated/negative/FP16 inputs, peak bounds, fades, black grades and hue protection.
- Gears 3 LUT fixtures check tinted identity in linear units, a continuous compression knee, flat-edge range restoration and black-chart stability. On an authored broad-plateau LUT, input 64 yielded 0.0289612 with the old gradient and 1.86133 with range restoration before display mapping. This fixture is not a measurement of the game's actual LUT.
- Actual captured Gears 3 scene and matching vertex shader create the FP16 graphics pipeline on both backends.
- Both rebuilt executables pass AMD64 and runtime dependency checks. Rollback executables and hashes are recorded in `work/gears23-lut-20261116/installation.json`. Runtime DLLs were not replaced.
- Installed Gears 3 SHA256: `72AD26BBD265E30B3CE9CBA3834B382BA20E15D9903A26575873944BA3227736`.
- Installed Gears 2 SHA256: `0151E7629ED9399D401AC3CC3D4D0B8AF3E9954463198C480B1288317D91B3DE`.

Work remains on the existing `hdr-gears2-gears3` branch. No commit or push was made. Live appearance and frame pacing confirmation remain pending. These grading changes do not broaden render-chain fallback coverage.
