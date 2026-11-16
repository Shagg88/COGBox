# Judgment: RenoDX UE3 grading and highlight review

Reviewed RenoDX at commit `bfdb52d3c75a943690ab8eb9e6716bc2ed509492` (2026-11-12), including the actual Dishonored, Batman Arkham Knight and Borderlands 2 shader implementations. Reference copies and the MIT license are under ignored `work/renodx-ue3-review-20261116/references/`. Existing distribution attribution is `Gears2-Native/cogbox/licenses/RenoDX-Grading-LICENSE.txt`.

## Source findings

| Reference | Relevant method | Judgment application |
| --- | --- | --- |
| [Dishonored grading](https://github.com/clshortfuse/renodx/blob/bfdb52d3c75a943690ab8eb9e6716bc2ed509492/src/games/dishonored/grading_0x52282493.ps_3_0.hlsl) and [common](https://github.com/clshortfuse/renodx/blob/bfdb52d3c75a943690ab8eb9e6716bc2ed509492/src/games/dishonored/common.hlsl) | Builds a bounded neutral proxy using a max-channel compression scale, applies its original LUT in the game's encoded domain, decodes and divides by the scale before custom tone mapping. Its optional HDR boost maps luminance and rescales RGB together. | Adopt the compress/sample/restore architecture. Keep Judgment's proven gamma, coordinates and fetch conversion; the Dishonored texture layout and encoding are different. Our existing highlight boost already follows the luminance-scaling principle, so adding another boost would duplicate it. |
| [Batman AK tone mapper](https://github.com/clshortfuse/renodx/blob/bfdb52d3c75a943690ab8eb9e6716bc2ed509492/src/games/batmanak/tonemap/tonemapper.hlsl) and [extended Uncharted 2 curve](https://github.com/clshortfuse/renodx/blob/bfdb52d3c75a943690ab8eb9e6716bc2ed509492/src/games/batmanak/tonemap/uncharted2extended.hlsli) | Extends the original curve with matching value and derivative at a selected pivot, and supports compression around LUT sampling followed by range restoration. Another path explicitly matches the game's mid-gray before the display mapper. | Keep Judgment's protected 0.18 anchor and derive its exponential/power slope analytically. Do not transplant Batman's Uncharted 2 coefficients or inflection-root solver: Judgment has a different curve. |
| [Borderlands 2 tone map](https://github.com/clshortfuse/renodx/blob/bfdb52d3c75a943690ab8eb9e6716bc2ed509492/src/games/borderlands2/tonemap_0x54ED86A0.ps_3_0.hlsl) and [common grading](https://github.com/clshortfuse/renodx/blob/bfdb52d3c75a943690ab8eb9e6716bc2ed509492/src/games/borderlands2/common.hlsli) | Retains upstream HDR through bloom/DOF, constructs a neutral bounded grading path, then restores range from the graded SDR result before display mapping. Its luminance grading is distinct from color grading. | Preserve Judgment's upstream capture and original effect masks. Extend luminance/range around the original grade rather than simply raising the configured peak or amplifying the final SDR frame. |

## Candidate implementation

Judgment's original grading reference and encoded SDR output remain intact. The extended path now scales all RGB channels together into the original 3D LUT's valid linear-input range, uses its existing gamma-domain addressing and tetrahedral interpolation, then divides the decoded grade by that scale. A shifted Reinhard shoulder starts at linear neutral brightness 0.5, leaves lower inputs untouched, matches the value and first derivative at the join and approaches LUT white smoothly. This is authored glue inspired by the RenoDX architecture, not a verbatim replacement with another game's shader or curve.

This avoids relying on a useful slope in the LUT's last few cells. The old edge-gradient continuation remains available to offline source comparisons, but the new candidate selects the compressed bridge. Pure-black grades stay black. The existing final reference/hue handling and PsychoV35 mapper remain the display stage; the previous Judgment-specific blend calibration and shared scene/FXAA outputs remain in place.

For Judgment's unclipped neutral curve, `f(x) = [m * exp2(a*x) * abs(s*x)^p]^2.2`, so `f'(x) = f(x) * 2.2 * (p/x + a*ln(2))`. The candidate uses that slope at the existing pivot, clamps a saturated/negative derivative appropriately and retains the original bounded curve below the pivot. The protected mid-gray value does not change. This removes finite-difference slope estimation and avoids importing a heavy inflection solver for the wrong curve family.

## Validation and limits

D3D12 and Vulkan GPU checks pass for both Judgment scene permutations, identity/tinted LUT range through 65000, black grades, original SDR output, diffuse/gray, fades, alpha and configured peak fitting at 4000 nits. A deliberately broad flat-edge LUT produced a constant 0.02899 green value with the old local-gradient path at input 64; the new range-restoring path produced 1.86271 before display mapping, retaining upstream highlight separation. This is an authored property fixture, not a claim that Judgment's real LUT has that exact plateau. The analytic slope also agrees with the numerical reference within the tested tolerance.

The actual captured scene/vertex pairs create their FP16 + SDR MRT pipelines on both backends. Extended LUT evaluation falls from up to five tetrahedral evaluations (20 texel loads) to one (4 loads), with no additional render pass or allocation. This is a structural cost reduction, not a new measured whole-game FPS result. Gears 2/3 paths and the existing PsychoV35 algorithm are unchanged.

The different LUT sampling locations can change bright-area color grading. Live appearance and frame pacing need a same-scene comparison. Earlier native HDR captures exceeding 1000 nits ruled out a global 203-nit ceiling in those sampled scenes; they did not establish that every effect is fully extended. Unknown/stencil/unsupported render-chain fallbacks remain a separate issue. Build logs, candidate shaders and validation results are under `work/renodx-ue3-review-20261116/`.

## Installation

The verified candidate is installed for Judgment, SHA256 `70E3EDE7B796C3328C16ADD83DFC8E509585B8A8E3699B5560F15C2247086F8B`. Previous executable: `G:\COGBox\work\renodx-ue3-review-20261116\before-install\20261116-125005`. Runtime DLLs, Gears 2/3 installations, and PsychoV35 itself were not changed. Live appearance and frame pacing still require the same-scene comparison.

The next live run, `20261116-125048`, confirmed activation: at the sampled point, 1,488 native FP16 frames / 12 reference frames, 1,492 paired scene draws and 1,491 paired FXAA draws. No source-compile failure was seen in that check. This confirms that the candidate executes; it does not establish a same-frame visual improvement or a whole-game FPS gain.


## No-FXAA staging completion (2026-11-16)

The follow-up live DevKit snapshot captured a paired scene draw into visible FP16 staging and original SDR, followed directly by UI draws. There was no FXAA draw. Native frame counters stopped while paired scene draws continued: staging was waiting for a pass the game skipped, and the first later draw invalidated it.

The renderer now publishes the pending visible FP16 rectangle to the existing padded HDR attachment once before a later draw needs it, after giving paired FXAA and selective effects their normal opportunity. Presentation also completes a pending scene if no later draw occurs. Completion requires the original target key and write generation to match; it cannot repair unknown writes or stale lineage. The fast paired FXAA route remains copy-free. No extra texture is allocated and there is no preservation copy per UI draw. Other unsupported materials still use the existing fallback policy.

Validation: Judgment renderer builds; D3D12 and Vulkan direct-UI GPU checks pass with the production copy shader publishing visible FP16 staging, checking that padded rows are untouched and stencil/scissor coverage and read-only depth/stencil survive. Executable and runtime dependency checks pass. Installed executable SHA256: `98B3C32416ABFB121996BC947B64905E5E1A844D43A08166DCDC240308415C8F`. Installation and rollback details: `work/renodx-ue3-review-20261116/installation-no-fxaa.json`.

The periodic Judgment paired-draw log now includes `no-FXAA completions`. Live scene confirmation and whole-game performance comparison remain pending; GPU fixture checks do not establish whole-game FPS or universal fallback coverage.


### Live verification after restart

Session `20261116-130355`, PID 9872: the no-FXAA completion counter reached 3,277 while native presentation increased to 5,787 frames. Across successive no-FXAA intervals the reference count stayed at 511, confirming the continuous staging fallback is fixed. DevKit snapshot (737 records) contains native encoder `0x6F2AD75A` at draw 732 reading 3840x2160 FP16 after native SMAA; reference encoder `0xA0A1A9E6` is absent.

This is not universal fallback elimination. The same run logged earlier stencil Canvas rejections and two later additive writers (`34632F4F93A1C06F`, `8C3D375A058D6CAD`, VS `2E88B37100D9D0CE`, blend one/one). The reference count increased from 511 to 513 during a later transition. These remain separate coverage/preparation issues; this capture alone does not establish their duration or a whole-game performance result.
