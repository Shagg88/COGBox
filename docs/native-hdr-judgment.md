# Judgment native HDR

Initial native HDR candidate using the shared Gears 2/3 FP16 companion and UE3/PsychoV35 display mapper. `present_hdr` and `gow2_hdr_native` still control activation. GPU checks pass; live colors, scene transitions and performance still require validation.

## Captured scene and grading

Scene pixel program `FF451AA8CB30AC21`: 168 words, FNV64 `F61C1939FAF8F0BE`; matching vertex `BE5D572A02B2CFFD`. Capture exposed `r[1].xzy * r[1].w` immediately before its first RGB saturation, after original packed-light decoding, DOF and bloom. Keep the original program for bounded grading and alpha-luma reference.

The second captured scene permutation is `887C175ADE20613D`: 177 words, FNV64 `670D2E213F1E1DD7`, same scene vertex. It folds exposure into `c250.z` before its first ceiling. Its power scale, exponential coefficient/multiplier, LUT scale/bias and grain offset live in different components; the bridge selects those bindings explicitly rather than reusing the first permutation's constants.

The actual 3D LUT is texture slot 4 (`t68` in generated HLSL). Sampling uses its real dimensions, `c250.z` scale, `c252.z` bias and original Xbox fetch adjustment. Neutral grading retains `c250.x/y/w` and `c251.y`; grain retains `c3.w` and `c251.x`. Exposure is already included in the captured scene: do not apply it twice.

Like Gears 3, continue the neutral curve above 0.18 with its tangent, use tetrahedral LUT sampling plus shared boundary-gradient continuation, then the RenoDX-informed luminance boost (power 0.40, normalization 0.04) and shared UE3/PsychoV35 mapper. Preserve the original diffuse grade, hue reference and output fade. Word count, fingerprint and unique source markers guard the transformation; mismatches fall back safely.

## FXAA, UI and performance boundaries

Post-scene FXAA pixel program `8309620DF190ABDF`: 135 words, FNV64 `BD5BB78CFE396879`; vertex `DDE732FE7045AADD`. The higher-quality FXAA permutation is `16CBC2E3D5BEC0A5`: 345 words, FNV64 `707DDFD1C3744044`, captured with vertex `4B3A7954F2660D9C`. Both captured FXAA permutations use the same validated transport. The Judgment-only `native_hdr_fxaa` hint enables existing selective scene-effect transport. A same-frame, version-checked full RGBA8 resolve permits a lazy FP16 encoded proxy. The original FXAA edge/coordinate algorithm reads this proxy and writes the owned linear FP16 scene.

Quantize original alpha luma to its 8-bit precision before scene FP16 storage; the FXAA encoder restores that lattice without deriving luma from expanded RGB. This avoids a second full-resolution texture read. Filtered alpha is not promised bit-identical because the proxy uses FP16 storage.

Reuse existing compatible Canvas/HUD handling. Unknown later writers, stale resolves, unsupported fetches and failed pipelines keep conservative reference fallback. This does not automatically convert every Judgment effect. Preserve CPU exposure readback for eye adaptation.

Prepare the captured scene and matching vertex shader during disk-cache loading; prepare FXAA variants asynchronously. Reuse the existing 128 MiB selective-proxy budget and process visible rows only. Gears 2/3 game hints and experimental scene-effect defaults remain unchanged. No gameplay performance equivalence is claimed before measurement.

## Validation

`gn_hdrtest judgment` passes for both scene permutations on D3D12 and Vulkan (including 4000-nit peak calibration at 200-nit scene white): source guards, RGB order, diffuse color, neutral/LUT continuation, finite bright inputs through 65000, peak limits, fades and alpha precision. Actual captured scene and FXAA shader pairs also create FP16 graphics pipelines on both backends. Existing Gears 2/3 grading and selective scene-effect GPU checks pass on both backends. All 239 CPU tests pass (1,105,449 assertions), including the separate local Windows UTF-8 fixture correction.

Logs and transient generated shader programs are under ignored `work/hdr-judgment-20261116/`; game shader data is not added to source. The first live native scene logs original grading constants and the raw slot-4 LUT descriptor without GPU readback. Live acceptance remains pending: capture the real LUT and FP16 target with DevKit, inspect native/reference counters, compare daylight, interiors, damage/reload, menus, transitions and DLSS Quality frame pacing against the saved original build.

## First live run and correction

The first candidate compiled correctly but the gameplay scene used the second grading/FXAA permutations. The log therefore showed zero native FP16 frames and reference-only presentation. The follow-up adds those exact captured profiles, preserving strict fingerprint/state checks. It is installed, but native-frame presentation and live image/performance acceptance still need confirmation from the next gameplay run. The launcher settings in that first run requested 400 nits, so 4000-nit GPU capability does not mean the live peak was set to 4000.

## Corrected build live results

The next live run (`logs/20261116-113237/gowj.log`) compiled both scene profiles successfully. Native presentation reached 3,048 FP16 frames / 252 reference frames; after the observed transition, the reference counter stayed at 252 while native frames continued increasing. Both FXAA variants replay successfully after their first pipeline-preparation misses. One lazy 4K proxy is reused at 63 MiB.

The run also recorded a conservative stencil-Canvas fallback for `19055C4D86C6C08C` with vertex `F9EBE3D4293EC775`; this does not establish complete coverage of all menus. Do not report zero fallbacks. User assessment of colors and performance, and comparison of equivalent gameplay routes, are still needed.

DevKit verified the native scene draw writes `R16G16B16A16_FLOAT` and reads the game's actual 16x16x16 3D LUT at `t68`. The final SMAA image passed to the HDR encoder is linear FP16 at 3840x2160. A live EXR readback contained 25,319 pixels above reference white, RGB maximum 1.6494, no negative RGB and no NaN/Inf pixels across 8,294,400 pixels (current 400-nit peak setting). Saved capture and analysis: `live-native-scene.exr` and `live-native-scene-analysis.json` in the task folder.

Native presentation subsequently reached 6,348 FP16 frames while reference frames remained at 252. This demonstrates stable native presentation in the sampled gameplay interval, not complete coverage of all scenes or a controlled FPS comparison.

A subsequent Judgment-only performance candidate is now installed: paired SDR/HDR scene and FXAA outputs, direct linear-source encoded-domain filtering without a per-frame encoded proxy dispatch, and diffuse/black/fade shortcuts. See [performance review](judgment-hdr-performance-review.md) for validation, measured fixture timings, live counters and remaining limits.

A Judgment-specific blend-calibration candidate is documented in [grading calibration](judgment-hdr-grading-calibration.md). It uses continued neutral-grade brightness for the SDR/HDR blend while retaining the paired-output performance path, original SDR output and protected mid-gray. See that document for current validation and installation status.

The installed follow-up uses a [RenoDX-inspired LUT bridge and analytic curve slope](judgment-renodx-ue3-review.md), based on a pinned review of Dishonored, Batman AK and Borderlands 2. That document records the source links, validation and rollback location.
