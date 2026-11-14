# Gears 2 native HDR investigation

The current native scene mapper is **PsychoV35**, using the supplied `customtest35.hlsli` edition. It replaces the previous scene extension as the primary mapper; there is no tone-mapper selector. [Source, credits and MIT notice](../Gears2-Native/src/layer0_xenos/renderer/psychov35/README.md).

The shared [UE3 grading bridge](ue3-hdr-grading.md) is the current grading strategy for both games. The earlier calibration and live readbacks below are investigation history, not the current finite-white specification.

The current Gears 2 grade extends its two analytic power sections from their
value and slope before SDR clipping. PsychoV35 maps that extended grade, with
the original hue, diffuse/shadows and explicit fade retained. Gears 2's
captured pass has no LUT. The invalid motion/blur edge fallback remains active.

### Display-peak range calibration

The boost now uses the same RenoDX Dishonored smoothed-power kernel as Gears 3,
at power0.40/normalization0.04, applied to linear BT.709 luminance `Y`. Gears 2
adapts that drive with a smooth onset between luminance1 and4 and a soft4x
gain bound. If `d = max(RenoDXBoost(Y)/Y-1,0)` and
`h = min(peak/sceneWhite,4)-1`, the common RGB gain is
`1 + smoothstep(1,4,Y)*h*d/(h+d)`. The boost is identity when luminance or
display headroom is at/below SDR white, preserving value and slope there.
Above four times scene-white headroom, the gain no longer grows with display
peak. This retains flash protection while opening the stronger highlights.

PsychoV35 now uses the same peak-scaled finite white as Gears 3,
`max(12,2*peak/sceneWhite)`, instead of the previous100/4*peak white. It fits the
extended lighting to COGBox's selected peak without a peak-proportional source
gain. The analytic game's existing power pivots remain: copying Gears 3's
filmic mid-gray pivot to a convex contrast power would suppress its slope.

This replaces the preceding brightest-channel drive whose gain could approach
15x at a 4,000-nit peak and 200-nit scene white. That calibration pushed medium
flashes/surface lighting too close to peak. Luminance gating also avoids
classifying a lone hot RGB channel as an intensely bright surface. This is a
pointwise boost policy, not a spatial detector of large flashes or light types.

At 203-nit scene white and a 4,000-nit peak, the four-white neutral flash test
maps to 2,039.5 nits; the preceding conservative boost mapped it to1,359.9
nits, and the earlier aggressive calibration to3,482.7. A modest1.5-white light
maps to328.9 nits, while a strong64-white emitter reaches3,999.7 nits.
An eight-white bright highlight maps to3,527.1 nits, retaining separation from
that peak-limited emitter.
GPU fixtures cover1,000/4,000/10,000-nit peaks,
SDR stability, value/slope continuity, finite and bounded common gain, hot
channels below the luminance threshold, peak-independent drive, color and fades.
The source must actually be strong enough to approach the selected peak.

The normal Gears 2 EXE was rebuilt and installed with a previous-file backup.
All eight scene/profile and sixteen UI/AA/upscaler checks passed, followed by
installed AMD64/import/runtime verification. Whole-game performance and full
menu/damage coverage have not been established for this calibration.

The earlier3,615-nit gameplay readback described the superseded aggressive
boost. The earlier bounded-boost preview was inspected in a paused scene. The
RenoDX transfer/peak-scaled revision was GPU-tested with the captured shaders;
the available DevKit belonged to Gears 3, so its Gears 2 gameplay appearance
and specific ground-flash behavior still need a live check.

### Stronger SDR highlight color

The Gears 2 highlight correction now restores 75% of the difference between
PsychoV35's relative Oklab chroma and the original SDR grade's relative chroma
(`C/L`), at the mapped HDR lightness. Previously it followed the SDR hue while
keeping the mapper's chroma, which could make boosted highlights too white.
Achromatic references do not acquire a tint. Source diffuse/shadows retain the
existing original-grade protection.

The correction keeps the mapped linear luminance before the final peak fit.
The shared RGB peak fit can still lower the luminance of a strongly colored
highlight to keep every channel within the selected peak; it retains that
color instead of forcing it toward white. The above-white highlight drive,
neutral peak response, independent UI clamp and exact output fades are kept.
Gears 3 uses zero chroma restoration and receives no executable update.

GPU tests compare six warm/cool/flame/purple/neutral/black cards at six light
levels against the preceding hue-only mapping. They check closer SDR `C/L`,
hue, diffuse/neutral/black stability, retained HDR boost and luminance/peak
bounds at 1,000 and 4,000 nits on both APIs. A live scene diagnostic computes
the old and new color errors from the same inputs: among 11,224 marked HDR
pixels, the median remaining error was 25% of the preceding error and the
maximum was 34%. The readback had finite, nonnegative RGB. Separate menu
captures show the preview but are not synchronized before/after frames.

## Earlier calibrations and captures

The finite input white was previously calibrated to Gears 2's lighting range: one scene
stop above the first SDR ceiling, including its exposure multiplier, with a
0.72-unit minimum. It is converted through PsychoV35's anchor gain before use.
This replaces its default 100-unit input-white assumption without raising paper
white. The shared finite-input regression passes on D3D12/Vulkan; the live
first input-white comparison was performed in Gears 3; the newer Gears 2 lighting comparison follows.

Gears 2 now has a separate highlight profile. Its brightest linear scene channel
drives the PsychoV35 intensity response, while the original analytic grade
retains color. The highlight mask/ratio use RGB peak brightness instead of
classifying saturated lights by their lower luminance. Diffuse neutral values
through at least 0.5 stay protected, and output fades/black/peak bounds remain.

Live tracing confirmed FP16 lighting/filter inputs already preserve native
range; no global format upgrade or blur-weight change was needed. In the final
sample, the former mapping's maximum was 0.99854 scene-white units while the
revised output reached 9.40274 (approximately 200 versus 1,881 nits at 200-nit
paper white). Pixels above 1.05 increased from 0 to 3,483, excluding bounded UI
white. All 8,294,400 RGB pixels were finite and nonnegative. Source-shader
reference luminance was stored in alpha beside revised RGB; later UI can affect
RGB, so this is an instrumented scene comparison rather than a frozen complete
presentation benchmark. A prior view also showed substantially more headroom.

The Gears 3 menu path keeps its existing math: the generated scene shader
compiles to byte-identical DXIL using `ps_6_6 -HV 2021 -Gis -denorm ftz` before
and after this Gears 2-specific change. Both games retain their color/fade/range
regressions, with moderate colored emitters added at contrast 1.4 (the captured
value) and the other test contrasts. UI isolation remains separate.

The shared SDR-color correction also applies to Gears 2's analytic grading. It
preserves the original graded RGB direction and shadows through the neutral
mid-gray anchor, then introduces only positive PsychoV35 highlight headroom.
The mapped response can no longer darken a valid SDR reference pixel. Gears 2's
GPU checks cover exact shadows, chromaticity for saturated/asymmetric inputs,
an under-responding mapped sample, peak bounds and fades on D3D12/Vulkan. Live
menu inspection for this correction was performed in Gears 3.

The 2026-11-12 optimization update is installed in the existing `gow2.exe`. Valid native HDR frames now skip the SDR presentation-gamma dispatch when presentation SMAA does not require it for edge detection. Reference frames, live AA changes, HUD-less frame generation and guest rendering retain their original processing. The shared 4K D3D12/Vulkan GPU benchmark saved approximately 0.25-0.28 ms in the native presentation chain with all 8,294,400 final pixels identical; this is not a whole-game FPS measurement. Details and benchmark results are recorded in [native-hdr-gears3.md](native-hdr-gears3.md#fade-preservation-and-shared-optimizations).

Measured on Gears 2 TU6 on 2026-11-11/12. Native HDR is implemented in the existing `Gears2-Native/build/Release-tu6/gow2.exe`, using a dedicated FP16 scene/HUD composition. The original capture findings below explain the previous SDR bottlenecks; implementation and validation follow them.

| Role | Live host shader | Guest microcode shader | Evidence |
|---|---|---|---|
| Final scene grading, tone mapping and gamma | `0x9A12A665` | `B6B6DD4E7D91F3D9` | Reads full-resolution FP16 scene color at t1, depth at t0, and two auxiliary post-processing textures at t2/t3. Writes the final 8-bit scene target before HUD draws. |
| Earlier scene/filter pass | `0x51667FA6` | `0354AF5BA0F333D1` | Reads depth and FP16 scene color, writes an FP16 intermediate. Preserve its scene range. |
| Repeated FP16 filter pass | `0x1C51CF83` | `CF4B08DF90C0001F` | Reads an FP16 auxiliary image and runs repeatedly before the final scene pass. Preserve its scene range. |
| Canvas HUD and active-reload UI | `0x8833D4DF` | `A9AC71DCCCEAEBE4` | Post-tonemap alpha-blended draws. The reload capture uses the same shader with several HUD atlases and indicator textures. |
| Color resolve | `0xCE5F31AA` | Built-in compute shader | Packs render-target values into integer destination words. |
| Guest-memory transfer | `0xFA593C60` | Built-in compute shader | Copies resolved words through guest memory. |
| Presentation gamma | `0xDD497B9E` | Built-in compute shader | Processes the packed presentation image. |
| Presentation unpack | `0xB7FA57B2` | Built-in compute shader | Unpacks the RGB10A2 presentation words. |

The capture's scene input was `R16G16B16A16_FLOAT`, 3840x2160. A live readback measured RGB maxima of approximately 3.016, 2.188 and 1.805, with no NaN or infinity pixels. This is current-resource evidence, not a frozen historical frame. Actual scene intensity above SDR white is available before final processing.

The final scene target was `k_8_8_8_8`, EDRAM base 720, guest pitch 1280, hosted as `R8G8B8A8_UNORM`. Its larger allocated height includes unused rows; the visible region is 3840x2160. Both the final shader and the destination format impose SDR limits.

## Original SDR bottlenecks

1. Extend the scene path inside guest shader `B6B6DD4E7D91F3D9`, using its original FP16 lighting and grading. Its CF13 explicitly saturates the graded color immediately before the CF14 gamma operation. Removing that clamp alone is insufficient while later resources still pack and quantize to SDR.
2. Carry the final scene and HUD composition in FP16 through the affected resolve, guest-memory, presentation and anti-aliasing operations, or use a dedicated HDR sidecar that bypasses those integer copies. Do not globally widen unrelated targets or infer ownership from dimensions alone: Xbox render targets alias EDRAM.
3. Feed that preserved image into a native HDR encoder. The current `kHdrEncodeCs` in `renderer/builtin_shaders.h` reads finished SDR RGB10A2, applies a luminance-dependent highlight boost and emits PQ/BT.2020. That is inverse tone mapping. A native encoder must read the retained float image and apply display mapping/PQ once, without that SDR expansion.
4. Composite bounded HUD color at its reference white after scene tone mapping. A white UI pixel must map to UI white, not display peak. The old final-image expansion would still brighten white UI even if its shader output were saturated, so the final encoder must change too.

## HUD safety implemented

`HlslOptions::clamp_ui_output` adds explicit RGBA saturation after the render-target exponent in both shader translators. It is now selected only for the float replay of identified post-tonemap UI draws on the final UI target. Other uses of the same shader retain their original output, including scene effects. UI profiles are matched by guest hash, dword count and full microcode fingerprint.

The current renderer's straight-alpha HUD blend has source alpha and one-minus-source-alpha color factors. Bounding RGB and alpha prevents an upgraded float target from exposing oversized UI intensity or alpha values. This guard does not introduce native HDR output by itself.

The earlier live SM5.1 replacement has been moved out of the `renodx-dev/live` directory: a global shader replacement cannot distinguish HUD draws from other uses of a shared textured shader. Extracted shader contents and capture data remain in ignored local work folders, not this contribution.

The launcher selects the existing `Release-tu6/gow2.exe`. The DevKit installation remains beside it. The earlier alternate UI-safe executable is not used.

## Implemented float path

The guest's normal target/resolve layouts remain intact because Xbox EDRAM aliases different formats. The final scene draw is replayed into a dedicated `R16G16B16A16_FLOAT` image using its original floating-point lighting textures, exposure, DOF, bloom and grading. This image bypasses the packed resolves and guest-memory copies for HDR presentation.

The scene profile captures normalized floating lighting after the original black offset and before instruction 70's SDR ceiling and exposure multiplier. It evaluates the original analytic grading on a safe SDR proxy, reconstructs the complete RGB result with the saved scale, and display-maps that linear graded scene with PsychoV35. The original bounded program remains the hue/diffuse/fade reference. See the shared UE3 grading notes for the current sampling, reconstruction, protected range and peak calibration.

The current grading and peak calibration are documented in [the UE3 notes](ue3-hdr-grading.md). Earlier short finite-white calibration and unclipped analytic color recovery are superseded.

The Gears 2 main menu uses this native scene path too. A live capture identifies
the floating-point lighting input, the FP16 scene replay and the native encoder
reading the same FP16 target after bounded menu Canvas composition. With SDR
color ratios restored, a current-resource readback contains 10,821 pixels above
1.05 scene-white luminance and a maximum luminance of 1.9414 scene-white units.
The scene shader's chromaticity diagnostic covers all 8,294,400 pixels and
matches its own SDR grading reference within measurement precision. The alpha
diagnostic measures the scene shader before later UI; it is not a comparison
between synchronized SDR/HDR screenshots of the animated menu. Menu text and
buttons keep their independent UI-white limit.

SMAA uses its existing edge/weight calculation but blends the HDR color into another FP16 target. Resizing uses normalized positive weights; sharpening is bounded by its neighborhood. The authored shader in `native_hdr_shaders.h` reads the retained float color and converts nits directly to BT.2020 / ST.2084. It applies no highlight expansion to finished SDR pixels. Menus, videos and unsupported passes use a reference-white fallback without ITM.

| Role | Original host | Guest hash | HDR host |
|---|---|---|---|
| Scene | `9A12A665` | `B6B6DD4E7D91F3D9` | `6C0C5E5B` |
| Canvas | `8833D4DF` | `A9AC71DCCCEAEBE4` | `5C48BA01` |
| Additional Canvas material | `4AF4124E` | `3F3410143BA49FE9` | `6E7816BE` |
| Animated reload HUD | `CA04DB7A` | `CDA0455F4F4EAFB4` | `FA4CF4DE` |
| Animated Canvas material | `41D2BE35` | `790DEB5680F51A2D` | `ED5A8350` |
| Additional Canvas material | `AD0932AA` | `CF8D65F2EAD24695` | `5897CB2C` |

This table records the earlier native build's host hashes; the PsychoV35 scene shader has a new host hash. Guest hashes and fingerprints identify profiles across rebuilds.

The active-reload firing capture identified `CA04DB7A` after the main HUD. It samples the exact same 512x512 BC3 HUD atlas as the basic Canvas shader, plus an 8x128 mask ramp. Omitting this draw from float composition caused reference fallback for the entire frame, clamping the otherwise HDR muzzle flash. The additional Canvas materials now remain in the bounded float composition.

## Damage / scene-textured UI

The full-screen material `ECE0CF7069CD4A91` (original host `3A29E910`) samples the resolved SDR scene at t0 and four UI/noise textures. Bounding this material's entire output would also bound the world behind the damage effect. It therefore uses a separate, fingerprint-checked path:

1. Build a floating gamma-encoded proxy from the real native scene; retain the original SDR resolve.
2. Evaluate the same original material twice into FP16 MRTs, once with each scene input. Keep the game's material math, tint, alpha test, sampler and blending. Extend texture gamma only above the original UNORM range.
3. Extract the positive luminance difference between the two material evaluations and carry that scene energy in the original bounded result's RGB direction. Equal UI-only emission cancels, while native world brightness survives. Fit the configured peak uniformly across RGB, preserving the effect's vanilla color balance. Keep exactly black references black.

There is no whole-screen UI-white cap or temporal transition to SDR. The damage material's vanilla colour differences remain. Ordinary HUD/reload elements use their independent UI white; the scene-sampling material retains the game's bounded reference-white base. D3D12 pairs a matching HLSL vertex twin with the DXIL pixel shader; Vulkan uses SPIR-V.

The initial per-channel `bounded + HDR_material - SDR_material` composition
could briefly reverse colors as the effect parameters changed. A Gears 2 live
capture traced the scene-sampling material `3A29E910` / `ECE0CF7069CD4A91`, its
FP16 pair and its composite. The corrected live preview was confirmed fixed by
the user while repeating the UI transition. The pair now also shares bounded
SDR-reference coverage for both evaluations; HDR world RGB is not saturated.

GPU regressions include an opposing-channel case where the old subtraction
turns a red reference cyan, then check preserved SDR chromaticity and scene
luminance, no/opaque/partial UI, decreasing material energy, black transitions,
uniform peak fitting and negative-safe gamma. They pass on D3D12/Vulkan, along
with the captured material's actual FP16 MRT pipeline and all 16 UI/AA checks.

Additional Canvas materials use the two captured Canvas vertex shaders (`868DFF11A6BDEBAA`, `E775468CA654846D`), final-target ownership, post-scene order, straight-alpha blending and disabled depth/stencil. This keeps new popup/reload materials in float composition without globally replacing shared pixel shaders.

## COGBox overlays

SDK patch `Gears2-Native/patches/sdk-patch85-hdr-overlays.diff` adds a separate HDR10 pixel shader to the D3D12 immediate drawer. It bounds SDR input, decodes sRGB and converts to PQ/BT.2020 at a fixed 203-nit white. This covers FPS text, shader-preparation notes, menus, achievements and other host overlays. SDR retains its original shader. The patch includes readable HLSL and its FXC SM5.1 header; rebuilding/installing the SDK and replacing `rexruntime.dll` applies it.

SDK patch `sdk-patch86-vulkan-hdr.diff`, applied after 85, requests `VK_EXT_swapchain_colorspace`, selects an advertised 10-bit ST.2084 surface format and uses a matching SPIR-V host-overlay shader. A missing HDR10 surface is reported instead of presenting PQ in an SDR colour space.

## AA and Vulkan

The PsychoV35 scene uses HLSL-generated DXIL with its matching vertex twin on D3D12, and SPIR-V on Vulkan. Existing bounded Canvas variants retain their DXBC/SPIR-V path. D3D12 GPU range checks cover AA off, SMAA, SMAA T2x, DLSS/DLAA, FSR and XeSS, including native-size and quality-size vendor upscaling. Vulkan range checks cover AA off, SMAA, SMAA T2x and FSR native/quality. DLSS and XeSS remain D3D12-only in the existing project.

`gn_hdrtest d3d12 gears2` and its Vulkan counterpart exercise 36 authored scene inputs, four curve parameters, four peak ranges and four fades. They test original diffuse/shadows, Oklab hue, continuous highlight transitions, HDR headroom without forced early peak, negative/FP16 inputs, black grading and exact fades. Empty-blur cases require the valid central scene to match its control pixel, including HDR colors. Real 3D LUT fixtures cover all six tetrahedra, centres/corners/edge clamps, nonlinear interpolation and analytic tangent identity/continuity. The captured scene profiles also create FP16 pipelines on both APIs. These checks do not establish full-game coverage or motion quality.

Vulkan FSR requires the Vulkan headers and matching signed DLLs from FSR SDK 2.4. Configure `GN_FFX_DIR` to `third_party/fidelityfx-sdk-v2.4.0/Kits/FidelityFX`. The previous SDK 2.3 tree lacks those files. The checks verify float range through the actual GPU wrappers, not gameplay motion quality or frame generation.

## Controls

- `--present_hdr=true`: existing COGBox HDR option; requires Windows HDR and a restart.
- `--gow2_hdr_native=true`: float scene profile, enabled by default when HDR output is selected.
- `--gow2_hdr_paper_white=203`: scene reference white, exposed by COGBox's existing paper-white control.
- `--gow2_hdr_peak=1000`: display peak; the user's selected COGBox value is preserved.
- `--gow2_hdr_ui_white=203`: independent game HUD/reload white, bounded by the display peak.

The legacy knee/expansion flags are accepted for compatibility but unused by the native encoder.

## Validation and remaining coverage

The Vulkan first-present crash was reproduced independently with the installed ReShade 6.8 DXGI proxy and RTSS hooks. The startup hardware-description query called that app-local proxy even for the Vulkan renderer. The same query through the full Windows System32 DXGI path avoids activating the proxy's DirectX rendering hooks during a Vulkan launch. `system_report.cpp` now uses that path; the installed overlay files and their global settings are not changed.

| Window presentation regression check | Result |
|---|---|
| Vulkan HDR10, local DXGI proxy used for the adapter query | Access violation |
| Vulkan HDR10, local proxy already loaded but adapter query uses full system DXGI path | 120 presents pass, RTSS hooks and Vulkan layer enabled |
| Vulkan SDR, local DXGI proxy used for adapter query | 120 presents pass |

The HDR encoder uses a separately compiled matching-resolution variant with direct pixel loads. Both variants are prepared at startup. The renderer also skips the unused SDR SMAA neighbourhood blend and SDR resize/sharpen dispatches when the float HDR result replaces them. SMAA's SDR edge/weight inputs remain unchanged.

A 3840x2160 RTX 4080 SUPER GPU test compares all 8,294,400 output pixels against the preceding encoder, with identical results on both APIs without sharpening and at strength 0.35. Timings use tagged GPU timestamp batches after warm-up, accounting for Vulkan's delayed query retirement. Without sharpening the difference is within 2%; at 0.35 D3D12 measured 0.2948 to 0.1618 ms and Vulkan 0.3576 to 0.1659 ms. These are encoder-only measurements; the extra native scene/UI replay still has a cost, and a whole-game HDR-versus-SDR comparison remains necessary.

The renderer library, SDK runtime and existing executable build successfully. D3D12 accepts the actual scene and Canvas bytecode into FP16 targets and the host-overlay shader into an HDR10 target. An off-screen GPU dispatch/readback verifies black, midgray, 203-nit white, real highlights above reference white, peak limiting, NaN and negative input handling. The original scene still regenerates to CRC `9A12A665`.

Live DevKit evidence confirms the dedicated 3840x2160 FP16 image and HDR10 swapchain. A current-resource readback measured RGB maxima of approximately 5.254 / 4.621 / 3.885 reference-white units, with 9,197 pixels above 1.0 and no NaN or infinity pixels. This is live-resource evidence, not a frozen historical copy. Runtime logs distinguish actual native FP16 presentation from reference fallback. The added reload-material composition still requires its final in-game firing check.

The `gn_hdrtest` utility (an excluded test target, not an alternate game executable) checks the real AA wrappers, native encoder and damage/UI composite. Readbacks verify retained HDR scene energy, cancellation of overbright UI-only emission, original tint, partial coverage and negative-safe gamma on both APIs. The captured damage material and matching vertex shader also compile into FP16 MRT pipelines on both APIs. The AA fixture's real RGB `(4,2,1)` survives above reference white.

The latest damage/popup and reload-firing changes still require an in-game check after restarting the rebuilt executable. Vulkan window presentation passes the isolated reproduction above; the complete rebuilt Vulkan game still needs its next launch check. Other tone-map permutations and HDR frame generation remain unvalidated. This profile does not claim complete scene coverage or measured exact SDR grading parity.
