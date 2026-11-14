# PsychoV35 provenance and integration

`include/xenos/renderer/psychov35_source.h` embeds the self-contained
`customtest35.hlsli` supplied for this project. Its HLSL matches the supplied
edition after whitespace normalization; the tone-mapping algorithm is unchanged.
There are no runtime shader includes or additional bindings.

- Original PsychoV35: Carlos Lopez (ShortFuse / clshortfuse), RenoDX.
- Self-contained edition modifications: Musa Haji (mqhaji).
- Upstream reference: [RenoDX test35.hlsl](https://github.com/clshortfuse/renodx/blob/main/src/shaders/tonemap/psychov/test35.hlsl).
- Retrieved upstream file blob: `94980c3b7c78d887ec255c58a6dd3b8cb1911987`.
- The supplied self-contained edition is a modification of that implementation,
  rather than a byte-identical copy of the upstream file and its includes.
- [MIT copyright, permission and disclaimer](LICENSE.txt). Both 2026 per-file
  copyright notices are retained in the embedded HLSL. The full license also
  ships as `licenses/PsychoV35-LICENSE.txt` beside both game executables and in
  COGBox's packaged notices.


COGBox's authored integration is in `psychov35_bridge.h` and
`ue3_grade_bridge.h`. PsychoV35 remains the primary HDR display mapper for
the fingerprint-checked Gears 2 and Gears 3 profiles, with no selector.

The current UE3 bridge extends the known power sections in Gears 2's analytic
grading with value/slope-matched tangents before SDR clipping. This captured
pass has no LUT. Gears 3 extends the original filmic shoulder, preserves its
grain mask and samples its actual 3D LUT with tetrahedral interpolation in the
original encoded domain, including centre scale/bias and fetch conversion.
Outside the cube, linear-unit gradients continue the boundary; a flat final
cell blends toward a useful earlier gradient. A constant region stays flat.
PsychoV35 maps the resulting extended grade. No inverse proxy scale or extra
mid-gray calibration lookup is used.

Gears 2's graded luminance above SDR white receives the same RenoDX
smoothed-power boost, with a smooth white-preserving onset and soft4x gain
bound adapted to its analytic grade. Its extra common RGB gain stops growing with
display peak above four times scene-white headroom. Moderate flashes therefore
retain headroom while much stronger emitters can reach peak. Values through
SDR white and the slope at that boundary remain unchanged by the expansion.
There are no additional texture reads or spatial/temporal masks; this is
separate from the bounded UI path. Gears 3's drive is unchanged.

Gears 2 also restores 75% toward the original grade's relative Oklab chroma
at the HDR lightness, alongside its hue correction. Linear luminance is
retained before the final peak fit. Neutral references stay neutral, and
source diffuse/shadows retain their original-grade protection. The existing
highlight expansion is retained. Gears 3 now uses the same relative-chroma
restoration alongside the original SDR hue reference.

Gears 3 applies RenoDX Dishonored's smoothed-power HDR boost to the extended
linear LUT grade before PsychoV35, tuned to power0.40/normalization0.04. The
scalar formula matches the sample (whose default power is0.20); luminance is evaluated in this shader's BT.709
working space. Copyright (c)2025 Carlos Lopez Jr. / RenoDX contributors, MIT;
the distributed RenoDX-Grading-LICENSE.txt covers this adaptation as well as
the Oklab matrices. See the Gears 3 notes for upstream file hashes and tests.

The finite input white no longer stretches a modest scene value directly to
display peak. PsychoV35's protected anchor is 0.5, with the same input/output
state. Both games use `max(12,2*peak/sceneWhite)` for their finite input white.
Gears 3 also uses its mid-gray filmic tangent; Gears 2 retains its analytic
contrast/output power tangents before clipping.
Source diffuse/shadows through 0.5 retain vanilla values; a smooth transition
ends at 1.0. Hue correction uses the original SDR grade's meaningful Oklab hue,
with chroma contraction along that hue for out-of-gamut colors. A luminance
floor at the original reference prevents a dimming dip in the transition. The explicit
fade follows the final peak fit, preserving exact black.

[UE3 reference shaders, domains, reconstruction and validation](../../../../../docs/ue3-hdr-grading.md)
document the inspected RenoDX mods: Batman Arkham Knight, Dishonored, Borderlands
2 and Mass Effect Legendary Edition. RenoDX's MIT Oklab notice is distributed
as `licenses/RenoDX-Grading-LICENSE.txt`; PsychoV35's existing notices remain.
No Luma code is included.

This supersedes the proxy reconstruction, earlier scalar highlight-gain bridge and the short
scene-white-to-peak calibration. Separate bounded UI composition, COGBox's
scene/UI white controls, the FP16 native path and direct PQ presentation remain.
GPU checks include real 3D-LUT sampling and the production transforms on
D3D12/Vulkan. A Gears 2 gameplay preview was inspected through DevKit; its live
FP16 readback retained highlights above SDR white with finite, nonnegative RGB.
Gears 3, menus and damage still need visual verification for this revision.
