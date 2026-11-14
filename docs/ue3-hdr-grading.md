# UE3 HDR grading references and implementation

The native scene profiles extend Gears 2's analytic grading and Gears 3's filmic
curve and 3D LUT from their original floating lighting. The supplied PsychoV35
remains the primary HDR display mapper. The completed SDR backbuffer is not the
source of highlight detail.

## RenoDX references for UE3 games

These were read from the actual upstream shaders, rather than treating a mod's
existence as proof of a particular technique. File hashes pin the versions read.

| UE3 RenoDX mod | Relevant behavior | Upstream file / blob |
|---|---|---|
| Batman: Arkham Knight | Gamma 2.2 LUT input/output, size 16, optional tetrahedral sampling, smooth common RGB clamp scale and inverse reconstruction; per-channel/luminance paths with hue/chrominance correction | [tonemapper.hlsl](https://github.com/clshortfuse/renodx/blob/main/src/games/batmanak/tonemap/tonemapper.hlsl), `9f76d9ff269e07b8f7ef3ad788f8cd85373edffd` |
| Dishonored | Compress before the vanilla grading/LUT, preserve the actual encoded lookup, decode and divide by the saved scale before HDR display mapping | [grading shader](https://github.com/clshortfuse/renodx/blob/main/src/games/dishonored/grading_0x52282493.ps_3_0.hlsl), `67049e2d5b622f45a24975b5b6dec57ebd70c780` |
| Borderlands 2 | Max-channel scene compression before the original curve/packed LUT, decode the graded SDR color, reconstruct with the saved scale, then display map | [tonemap shader](https://github.com/clshortfuse/renodx/blob/main/src/games/borderlands2/tonemap_0x54ED86A0.ps_3_0.hlsl), `09dde43f4244d372164ddcdaf72758683f615c1b`; [common](https://github.com/clshortfuse/renodx/blob/main/src/games/borderlands2/common.hlsli), `30888626efd24cf6af35d9c55668f946acad6f46` |
| Mass Effect Legendary Edition | Preserve the original filmic/grade/encoded lookup context and pass a decoded graded reference to ToneMapPass | [ME2 filmic LUT shader](https://github.com/clshortfuse/renodx/blob/main/src/games/masseffectle/tonemap_me2_filmic_lut_0x222186F8.ps_5_0.hlsl), `b5b5ea6e586eedf379dcd998353c0b1237ae01ce` |

These examples are alternatives, not one identical shared UE3 implementation.
The preceding integration followed the compress/sample/reconstruct ordering
used by Borderlands 2 and Dishonored. The current revision replaces that proxy
reconstruction with explicit curve/LUT continuation, described below. Each game
keeps its own grading and transfer function. No Luma implementation is included.

## Proven Gears inputs and grading

| Profile | Scene capture | Original grading context |
|---|---|---|
| Gears 2 `B6B6DD4E7D91F3D9` | Normalized floating scene after its black offset, before instruction 70's first ceiling and the exposure multiplier | The original analytic exposure, contrast, channel mixing, second SDR ceiling, black floor and output exponent. There is no LUT in this captured pass. |
| Gears 3 `2F284B68A112ED3E` | Floating linear scene before the filmic curve | The original neutral curve and grain response, then the real 3D LUT at t67 / sampler 3. LUT coordinates retain `c252.x` / `c253.w` centre scale/bias and the translator's Xbox fetch adjustment. Interpolation occurs in the original encoded domain, followed by the original texture-fetch conversion and gamma 2.2 decode. |

The unchanged vanilla program still supplies a hue/low-range/fade reference.
No black, mid-gray or white LUT calibration samples are added. Gears 3 uses four
explicit corner loads per tetrahedral evaluation; outside-domain pixels also
sample interior gradients. The existing vanilla sample supplies the reference.

## Curve and LUT extension

1. Gears 2 continues both known power sections with their value and analytic
   derivative before the two SDR ceilings. The first pivot is scene input
   0.75 after the original exposure; the second is graded input 0.9. Below
   each pivot the original power remains intact. The channel mix and black
   floor remain part of the extended grade. Bounded power evaluation avoids
   exponentiating an FP16-max input before applying the tangent.
2. Gears 3 continues the original neutral curve using its value and numerical
   derivative at scene input 0.18. The previous join deep in the original
   shoulder retained an excessively flattened slope. Grain keeps the original bounded
   mask while the extended coordinate itself remains unclipped.
3. Gears 3 samples in-cube values with tetrahedral interpolation in the
   original gamma domain, including the game's centre scale/bias, Xbox fetch
   adjustment and fetch conversion. Outside the cube, it samples the boundary
   and up to four interior intervals along the excess direction. Derivatives
   use linear input/output units, so a gamma-encoded identity LUT remains
   approximately linear when extended. A flat final interval resumes the
   nearest useful interior derivative with an integrated smoothstep; a wholly
   flat region stays flat. This is an extrapolation policy, not recovery of
   unknown authored colors beyond the finite LUT. No inverse proxy scale is
   used in either game.
4. PsychoV35 maps the extended, linear graded scene to COGBox's selected
   peak. Both games use `max(12,2*peak/sceneWhite)` to adapt their shoulder to the selected
   peak, including 4,000 nits. The
   previous calibration could stretch an input near 3 toward display range 19,
   making ordinary highlights look excessively white and bright.
   Gears 2 additionally applies the bounded above-SDR-white luminance drive
   described in [its range calibration](native-hdr-gears2.md#display-peak-range-calibration),
   so strong gameplay highlights can use a 4,000-nit range without forcing
   moderate flashes toward peak. Gears 3 applies the RenoDX Dishonored sample's
   smoothed-power luminance boost after its LUT extension, at power0.40 and
   normalization0.04, with its own peak-calibrated display shoulder.
5. Oklab hue correction follows the vanilla reference where it carries a
   meaningful hue. An achromatic reference does not define a stable hue. The
   corrected color is fitted to nonnegative RGB and peak without independent
   channel saturation. Linear luminance is floored at the vanilla reference
   before blending to avoid a dip when the display shoulder is darker than a
   concave SDR grade. Source shadows/diffuse through 0.5 keep vanilla grading,
   with a smooth transition ending at 1.0. The explicit game fade is applied
   after the final peak fit, preserving fades to exact black.

The supplied PsychoV35 algorithm is unchanged. Original SDR rendering, the FP16
native scene ownership and the separate bounded UI paths remain in place.

## HDR boost references and flash control

The current upstream [Dishonored common shader](https://github.com/clshortfuse/renodx/blob/main/src/games/dishonored/common.hlsl)
(blob `48fbec53cca389fe618779c3bf90385e6daf7fe3`) exposes a luminance-driven
`ApplyHDRBoost` path with a smoothed power response before display mapping.
The [Batman AK tonemapper](https://github.com/clshortfuse/renodx/blob/main/src/games/batmanak/tonemap/tonemapper.hlsl)
extends the game curve and preserves the grade through a bounded LUT proxy;
its display mapping is separate from that grading reconstruction. Neither
example implements a detector for the screen area occupied by a ground flash.

The Gears 2 refinement now adapts the same Dishonored scalar power-boost kernel
used by Gears 3, adding a white-preserving onset and soft bounded gain for its
analytic grade. It does not make unrelated emission shaders follow that game's
exposure. A4x gain bound and gradual knee replace the preceding display-peak-proportional gain, whose
asymptote approached 15x at a 4,000-nit target and 200-nit scene white. Actual
pixel brightness drives the result; no ground/material classification,
neighborhood samples, frame averaging or global UI/world clamp is added.
Existing SDR hue/chroma restoration and PsychoV35 remain in place.

The Grim Dawn helper was also inspected (blob
`0c97249f3256ecd5e379581be9f066e6b5a6570b`): it defines similar boost/rolloff
helpers, but its default per-channel boost branch has the boosting statements
commented out. Helper names alone therefore do not establish active behavior.

## Attribution and validation scope

Oklab conversion matrices are adapted from [RenoDX's Oklab include](https://github.com/clshortfuse/renodx/blob/main/src/shaders/color/oklab.hlsl),
blob `35c1b65defe9b656b3559ddf8507b6e1823f49b5`. Credit Carlos Lopez Jr. /
ShortFuse and RenoDX contributors. The upstream MIT permission/disclaimer is in
`Gears2-Native/cogbox/licenses/RenoDX-Grading-LICENSE.txt` and is packaged with
both games. PsychoV35's separate Carlos Lopez / Musa Haji notices remain intact.

GPU checks exercise all six tetrahedral orders, identity/affine LUT values,
centres/corners/edge clamps, a nonlinear chart distinguishing tetrahedral from
trilinear, tangent identity and value/slope continuity. Gears 3 fixtures also
exercise linear-unit extrapolation, the analytic filmic derivative, boundary
continuity, flat-final-cell recovery and constant-LUT stability. Scene fixtures exercise
the production transforms, low-range preservation, highlight transitions,
finite/negative/FP16 input, peak limits, fades and black grading. Captured game
profiles also create actual FP16 pipelines on both APIs. These checks do not
replace in-game visual comparison of menus, damage and gameplay lighting.
Outside-domain LUT gradients add texture reads; whole-game performance of this
revision has not been benchmarked.

The extension revision passed eight scene/profile GPU checks and sixteen
UI/supported-AA/upscaler range checks. Both normal game executables were rebuilt
and installed with previous-file backups; AMD64/import/runtime checks passed.
A Gears 2 DevKit gameplay preview retained HDR values: the current FP16 scene
readback contained 3,563 pixels above SDR white, maximum RGB 3.3301 and maximum
luminance 2.7264 scene-white units, with no negative, NaN or infinity pixels.
Its last-row maximum luminance was 0.10014. These are live contents from one
scene, not a synchronized SDR comparison or proof of motion-edge coverage.
Gears 3 and menu/damage visual coverage remain unverified for this revision.

The Vulkan compute readback tests exposed an existing hardcoded 2D SRV view for
3D textures. Compute dispatch now requests a 3D view for an actual 3D texture;
the game's pixel-shader texture-binding path is unchanged.

## Invalid motion/blur coverage at Gears 2's screen edge

A live capture reproduced a white segment on the last image row while moving
backwards. All 84 peak-bright pixels in the normalization diagnostic had zero
combined blur coverage. The final reciprocal becomes infinity, and treating
that result as a valid HDR scene lets the grading reconstruction amplify the
invalid input to display peak.

The Gears 2 profile now rejects nonpositive/nonfinite normalization reciprocals
and nonfinite normalized colors. Those pixels use the central unblurred scene
sample already fetched by the original shader, with the same black offset and
analytic grading. The reference hue is also taken from this valid sample, so
the bad original blur result cannot leave an SDR-white line. No added texture
sample, cropped row, global reciprocal change or full-screen clamp is required.

In a paired live diagnostic, 216 affected last-row pixels that formerly reached
19 scene-white luminance now have a maximum of 0.1614 and mean 0.03176; real
highlights elsewhere still reach 2.4901. Old integration luminance is computed
from the same inputs into alpha beside corrected RGB. The normalization and
paired readbacks are current live contents, not synchronized frozen screenshots.
GPU checks cover infinite/NaN/negative/zero reciprocals, overflow, valid very
small coverage, central-sample fallback and controls through the production
source transformation. That edge-only fix did not change the Gears 3 profile;
the subsequent extension revision changes both profiles.

## What highlight reconstruction can preserve

The original floating lighting is the source of real highlight range. Gears 2's
known analytic grade supplies a derivative for deliberate curve continuation.
Gears 3's finite LUT supplies measured interior gradients for extrapolation,
but cannot reveal color differences that it maps to the same output. The
nearest useful gradient is a controlled estimate of the missing continuation,
not an exact inverse of clipping or a recovered HDR master.
The display peak is a ceiling. Only sufficiently strong valid lighting should
approach it; pushing invalid border samples or every SDR-white pixel to peak
does not recover genuine highlights.
