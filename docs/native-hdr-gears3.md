# Gears 3 TU6 native HDR

The Gears 3 profile uses the shared renderer's FP16 scene/HUD composition and native PQ encoder. Enable COGBox's existing HDR option; its paper-white and peak controls are used directly. The legacy SDR highlight-expansion controls are unused by this encoder.

The current main native scene tonemapper is **PsychoV35**, using the supplied `customtest35.hlsli` edition. There is no tone-mapper selector. [Source, credits and MIT notice](../Gears2-Native/src/layer0_xenos/renderer/psychov35/README.md).

The shared [UE3 grading bridge](ue3-hdr-grading.md) is the current grading strategy. Earlier calibration, hashes and readbacks below describe prior investigation; use the current bridge specification and the latest build validation for this revision.

The current revision extends the filmic curve from protected mid-gray, then extrapolates the actual
3D LUT using linear-unit boundary/interior gradients. In-cube samples retain
the original encoded domain and fetch conversion. A clipped final interval
smoothly resumes a useful interior gradient; a constant region remains flat.
PsychoV35 stays the main display mapper. Curve/LUT, grading, peak and fade GPU
checks run on D3D12 and Vulkan; this is not proof of full-game visual coverage.

### RenoDX sample HDR boost

The extended linear LUT result now passes through the smoothed-power luminance
boost from RenoDX's Dishonored sample before PsychoV35. The scalar formula
matches the sample, with its power parameter tuned from the upstream default
0.20 to 0.40 after live testing; normalization stays 0.04. For luminance `Y`
and power `p`, the smoothing weight is `2*p*Y/(Y+2*p)` (the actual RenoDX
Reinhard definition), and the boosted value is the maximum of `Y` and
`lerp(Y,0.04*pow(Y/0.04,1+p),weight)`. A common RGB scale applies this value
in COGBox's linear BT.709 working space. This adapts the sample's color-space
wrapper while retaining its scalar algorithm; it is not the custom Gears 2 gain.

The filmic extension joins the original value and derivative at scene input
0.18. The preceding join was far into the original compressed shoulder, whose
flattened slope suppressed the available HDR range. The shared final blend
still keeps original source shadows/diffuse through 0.5 and transitions smoothly
to the extension by 1.0. Original SDR hue and 75% relative Oklab chroma recovery
keep brighter highlights closer to the game's grading.

PsychoV35's finite input white is `max(12,2*peak/sceneWhite)` for this profile.
This adapts its shoulder to COGBox's selected peak, including a 4,000-nit target,
without multiplying the source boost by display peak. The boost follows the
filmic/LUT range extension rather than feeding an unclipped coordinate into
a bounded sampler. The original encoded LUT
sampling and fetch conversion remain. Original SDR low-range/hue references,
grain, black grading, peak fit and post-map fades remain in place. UI brightness
and clamping stay separate. PsychoV35 remains the sole final display mapper;
there is no new mode selector, gain tied to the selected peak, or stacked boost.

Upstream reference: [Dishonored common.hlsl](https://github.com/clshortfuse/renodx/blob/main/src/games/dishonored/common.hlsl),
blob `48fbec53cca389fe618779c3bf90385e6daf7fe3`, and
[Reinhard helper](https://github.com/clshortfuse/renodx/blob/main/src/shaders/tonemap/reinhard.hlsl),
blob `14c99a953c0eeaa8e651e7d0df38c1c8066cb962`. Copyright (c) 2025 Carlos
Lopez Jr. / RenoDX contributors. The MIT permission/disclaimer already packaged
as `licenses/RenoDX-Grading-LICENSE.txt` covers the adapted boost too.

GPU tests verify golden gain vectors at the upstream 0.20 setting,
negative/black/FP16-max input, a common RGB gain, low-range stability, finite
LUT highlight improvement, peak limits and fades on D3D12 and Vulkan. A live
paired scene readback of the initial, mild revision measured maximum luminance
1.8555 versus 2.5614 scene-white units. Moving the filmic join to mid-gray then
measured 2.1641 versus 6.3425 in another paired scene readback. These describe
intermediate tuning, not the final 0.40/peak-calibrated revision. Later UI draws
may alter RGB, so paired alpha/RGB is scene evidence rather than a frozen
full-presentation comparison or full-game test. Gears 2's EXE is unchanged.

The final 4,000-nit calibration fixture maps a strong neutral highlight to
3,999.7 nits and a modest 1.5-white light to 821.5 nits, with the grey anchor
stable. The source and SDR hue/chroma/range/fade regressions also pass on both
APIs. A later live readback measured maximum luminance 14.4603 scene-white
units, about 2,892 nits at the running 200-nit scene white, with finite and
nonnegative RGB. That session's selected peak was 3,800 nits; the shader uses
the existing COGBox peak control, so 4,000-nit output requires selecting4,000.
The normal EXE is rebuilt separately from temporary live diagnostics; no
alternate game EXE or tone-mapper option is added.

## Captured path

### Menu artifacts and SDR color preservation

The initial grading bridge could darken parts of the main-menu background and
remove its warm grading. A live menu capture reproduced sharp dark patches;
temporarily displaying the original HLSL material's bounded SDR result removed
them, isolating the scene bridge from the post-scene menu overlays.

The shared bridge now uses the actual graded SDR RGB direction, rather than
subtracting neutral chromaticity from PsychoV35's different cone response. It
retains shadows through the neutral mid-gray anchor exactly, then smoothly
introduces only positive PsychoV35 luminance headroom above that anchor. A
locally lower mapped response cannot cut dark holes into the reference image.
Peak limiting scales RGB uniformly and explicit fades remain after that bound.
The corrected live Gears 3 menu preview removed the patches and retained its
original warm color balance. The same code is used by Gears 2's analytic grade.

GPU regressions now verify SDR chromaticity for saturated and asymmetric scene
colors, exact shadow retention, and an intentionally dark/differently colored
mapped response over a valid SDR grade. Both games pass on D3D12 and Vulkan,
including finite scene-white headroom, peak/fade/black/grain checks and actual
captured scene pipeline creation. All 16 UI/AA/upscaler checks pass. Full gameplay
color/motion and whole-frame performance validation remain separate. Both corrected EXEs are installed in their existing launcher locations; runtime/import/hash verification passed, and the temporary menu preview was moved out of the live directory.

### PsychoV35 input-white calibration

The first PsychoV35 integration retained its default finite input white of 100.
A live Gears 3 diagnostic confirmed a selected `peak / sceneWhite` of 20, but
the scene input only reached about 4 units. The default response therefore
remained near 1,000 nits rather than using much of the selected display range.
The finite white now follows the game's rational shoulder: `19 * abs(c4.x)`
reaches 95% of its asymptotic encoded white, with a minimum of 0.72 scene units.
This input point is converted through PsychoV35's anchor gain for `clip_point`.
There is no second output shoulder, histogram normalization or paper-white gain.

A single live readback stored the corrected RGB and the original-default-clip
counterfactual luminance in alpha, evaluated from the same scene inputs. Maximum
luminance changed from 4.25781 to 17.01344 scene-white units, or approximately
852 to 3,403 nits at 200-nit paper white. Mean luminance changed by 1.98%.
For 6,614,614 pixels below 0.18 original luminance, mean absolute change was
0.00001382 scene-white units. All 8,294,400 pixels were finite and nonnegative.
These are scene-specific measurements; other scenes need not reach display peak.
The alpha diagnostic was removed before installation.

Both games' GPU regressions now check finite scene-white input as well as extreme
FP16 input, preventing a test at 65,000 alone from hiding this calibration error.
All scene/fade/grading checks, captured scene pipeline checks on D3D12/Vulkan,
and all 16 AA/UI/upscaler checks passed. Both usual EXEs were rebuilt and installed;
the temporary live replacement was moved out of the DevKit live directory.

Captured through RenoDX DevKit on 2026-11-12, D3D12, 3840x2160 output, DLSS Quality. These are original shader IDs, before native replay:

| Role | Host shader | Guest shader | Validation |
|---|---|---|---|
| Final scene curve and LUT | `287C461B` | `2F284B68A112ED3E` | 150 dwords; fingerprint `8BA713A2E446A745` |
| Scene vertex | `BB6B7D00` | `A9C7FE3382D41904` | Matching HLSL vertex twin for the DXIL scene replay |
| Basic HUD | `9C1EE0CA` | `56B153DABC9D68B3` | 57 dwords; fingerprint `D923D828C2CD577A` |
| Animated reload | `CFB9E8BA` | `3A795B52038F5F99` | 36 dwords; fingerprint `27ECDC4E8FE1A683` |
| Damage mask | `AA3E3635` | `7FA8DF4F5A89A67E` | 195 dwords; fingerprint `5B6ECACDD433C080` |

The scene contains real HDR lighting in the `k_2_10_10_10_FLOAT` EDRAM target, hosted as FP16. A live readback of its allocated 2560x4096 image measured RGB maxima 85.3125, 76.75, 135.75; all 10,485,760 pixels were finite, with 1,690,764 pixels containing a component above 1. These figures include unused allocated rows and describe the live resource, not a frozen snapshot or the visible image's average brightness.

The final scene material reads a scaled `R10G10B10A2_UNORM` resolve containing **packed floating lighting**. Its microcode decodes the mantissa/exponent before filmic processing. The UNORM input descriptor therefore does not imply that its lighting is already SDR. Host shader `35AFF560` is a built-in EDRAM transfer, not the final tonemapper.

## Scene mapping

`Gears3SceneSource` retains the entire original material, including packed-light decoding, DOF, bloom, grain and 3D LUT grading. It captures nonnegative scene color immediately before the filmic curve at instruction 28.

The current bridge compresses this linear lighting into a safe SDR proxy, evaluates the original filmic curve and grain, and samples the real 3D LUT with tetrahedral interpolation in its original encoded domain. It reconstructs the full graded RGB result with the saved scale before PsychoV35 display mapping. The unchanged vanilla result supplies hue and diffuse references; explicit fades follow the final peak fit. No separate mid-gray LUT samples are required. See the shared UE3 notes for exact domains and validation.

This replaces the earlier tangent/highlight-headroom extension and exponential shoulder; neither is stacked with PsychoV35. The CPU tangent solve/cache has been removed. The 3D LUT texture remains unchanged and supplies grading rather than limiting the available HDR lighting.

The final scene draw is replayed into linear `R16G16B16A16_FLOAT`. Native HDR presentation reads this image directly, bypassing the integer EDRAM resolves and guest-memory presentation copies. It converts scene-white-relative color to nits and BT.2020/PQ without inverse tone mapping. The original guest target layouts remain intact for aliasing and later guest reads.

## HUD, reload and damage

HUD output RGB and alpha are bounded only during the identified post-tonemap float replay. The same shader's ordinary scene uses retain their original output. UI white is independent of scene white, with a default of 203 nits and a display-peak bound.

The captured reload material samples the BC3 HUD atlas and an 8x128 mask. The captured damage material uses a 512x512 R8 mask, straight-alpha blending and a recognized Canvas vertex shader. It does not sample the resolved world image. Its bounded source is composited over the retained HDR scene; no whole-screen UI cap is applied. Vanilla damage tint and opacity remain part of that blend.

The pause/menu capture also identified independent color, atlas, image and mask shaders (`A7AB2D75C169DFDE`, `19055C4D86C6C08C`, `B8A5DDABA29740E5`, `1DE36FC34E4B7E48`). The multiplicative darkening layer `A8EEB25041FFBA45` is decoded into a bounded linear modulation factor with no UI-white gain. It darkens the retained scene according to the original mask instead of imposing an SDR ceiling on the world.

A later damage capture found another blood-overlay material, `9FCCAF2C4923CA93` (host `D825C47C`, 129 dwords, fingerprint `5CF7251E4670B002`). It samples three BC3 UI textures and uses straight-alpha blending, with no world-scene sample. Its missing profile invalidated the native frame and caused the entire presentation to fall back to SDR. It now has its own bounded float replay. The same capture also covers overlay `63C7939AE43787AB` (54 dwords, fingerprint `59ABADA2DC6A676A`); solid-color UI `D2B5DF9B2B04E715` (42 dwords, fingerprint `FF8DB83E451A0BF5`) has an explicit profile too.

The shared renderer also recognizes the two Gears 3 Canvas vertex shaders `6F83A894AB66D831` and `2E88B37100D9D0CE`, restricted to the owned final target, straight-alpha blending, post-scene order and disabled depth/stencil. Unsupported later writers still produce a logged reference-white fallback. Additional scenes or full-screen scene-sampling materials need capture and validation before claiming complete game coverage.

COGBox FPS/menu/achievement overlays use the existing SDK HDR overlay patches at 203-nit white.

A large-dump audit and live capture also identified UI shader
`2C1E4E099E31E037` (host `D782C90B`, 51 dwords, fingerprint
`B241E3475A37FF9C`). It blends the same 160x80 BC3 UI image from two texture
slots; no world image is sampled in the captured draw. Its missing profile
invalidated the whole native frame. Its bounded float replay requires the
captured vertex program `5BF4E342D00F1593` and straight-alpha blending.
Actual shader-pair FP16 pipelines and UI-isolation readbacks pass on D3D12
and Vulkan; the installed result still needs a live visual check. The other
logged writers and menu state restrictions are detailed in
`docs/gears3-hdr-shader-audit.md`.

The later modulation material `1C0932D0C22ECC2A` (host `AFF5BC66`, 21 dwords,
fingerprint `88FB1A623EDF043F`) has a separate profile. A live capture found
four draws sampling the same 68x76 BC1 image with destination-color/zero
blending. Its replay requires vertex program `2E88B37100D9D0CE` and that
blend mode. The source is a dimensionless bounded linear factor, without
UI-white gain or a scene highlight shoulder; it multiplies the existing
HDR scene rather than replacing it with the SDR reference. The same
explicit modulation output mode also applies to the already recognized
menu darkening material. Both API GPU readbacks execute the actual new
pixel shader over HDR backgrounds for neutral/dark/negative/overrange
factors, alpha retention and UI-white/peak independence.

## Controls and backends

| Existing option | Native path |
|---|---|
| `present_hdr` | Select HDR presentation; restart required |
| `gow2_hdr_paper_white` | Scene reference white in nits |
| `gow2_hdr_peak` | PsychoV35 display peak and final peak bound |
| `gow2_hdr_ui_white` | Independent game HUD white in nits |
| `gow2_hdr_native` | Enable captured native profiles, default true |

The shared `gow2_` option names are intentional: COGBox already passes them for Gears 3. D3D12 uses a matching DXIL vertex/pixel pair for the scene; Vulkan uses SPIR-V. The captured scene pipeline compiles on both APIs.

`gn_hdrtest d3d12 gears3` and `gn_hdrtest vulkan gears3` exercise the production PsychoV35 source transformation with authored inputs, three curve exposures, four display ranges and four fade values. They check the neutral mid-gray anchor, highlight headroom, grading, saturated colors, negative and maximum FP16 input, finite nonnegative output, peak bounds, a black LUT over bright lighting, bounded grain and exact black. The corresponding `gears2` cases check its analytic-contrast anchor. Captured scene shaders also pass matching-vertex FP16 pipeline creation on both APIs. The UI-isolation checks verify world-HDR retention, bounded UI emission, partial coverage, tint and negative-safe gamma. AA/upscaler range checks cover D3D12 off/SMAA/SMAA T2x/DLSS/FSR/XeSS and Vulkan off/SMAA/SMAA T2x/FSR, including vendor quality-size cases.

Vulkan FSR requires SDK 2.4 headers and signed Vulkan DLLs (`GN_FFX_DIR=Gears2-Native/third_party/fidelityfx-sdk-v2.4.0/Kits/FidelityFX`). DLSS and XeSS are D3D12-only in the existing wrappers. GPU range tests do not establish gameplay motion quality, frame-generation behavior or full-game shader coverage.

The rebuilt AMD64 executable is installed at the launcher's existing `Gears3-Native/build/Release-tu6/gow3.exe` path, with matching FSR 2.4 D3D12/Vulkan runtime DLLs. Executable hashes, runtime hashes and imported dependencies passed verification. No launcher executable-path change is needed.

Before the PsychoV35 replacement, live D3D12 validation after restart confirmed the native scene and matching vertex shader draw into the dedicated 3840x2160 FP16 image, which is sampled directly by native presentation. With DLSS Quality, scene white 200 nits and peak 3800 nits, the live image contained 1,865 pixels above SDR white; RGB maxima were 2.0410, 2.6953, 2.4941 in scene-white-relative units. All 8,294,400 pixels were finite and nonnegative. The reference-frame count stayed at 1,074 while native frames increased to 18,126 during this observation; this demonstrates stable native presentation over that interval, not complete game coverage. Logs also confirmed that the bounded HUD, reload, damage-mask and menu shader variants loaded successfully. A complete live firing/reload/damage sequence still needs verification.

Extracted microcode/HLSL, readbacks and transient shader binaries remain under ignored `work/native-hdr-gears3`; no game shader data is included in the source contribution. Live readback results describe the current resource, not a frozen historical snapshot.

The earlier highlight source was previewed through DevKit in the running first build. A live FP16 readback measured maximum luminance 16.36085 scene-white units (about 3,272 nits at 200-nit scene white), with every pixel finite and nonnegative. This was a different live image from the earlier readback, not a controlled same-frame comparison. The highlight and blood-overlay revision was installed in the existing executable (SHA256 `D5EAECE0362C8AD52CFA83A2540E205BE5BE2F9224C4FD0DDBE0E57F74995C1A`) and is superseded by the fade-preserving revision below. Its damage/reload materials passed real FP16 alpha-blend pipeline creation on D3D12 and Vulkan.

## Previous revision: fade preservation and shared optimizations

The first highlight-headroom revision normalized the graded pixel before adding HDR light. This could cancel a game's fade toward black. The regression test reproduced a half-faded highlight retaining red 0.85791 instead of the expected 0.429932. The corrected shader retains the complete LUT/output scaling and reapplies it after the HDR shoulder. That revision passed tests for 100%, 50%, 0.1% and exactly black output at three curve exposures and two display ranges on D3D12 and Vulkan. The original below-pivot result, graded hue and display-peak bound are checked too. PsychoV35 now uses the explicit post-peak output-fade handling described above.

Both games now skip the SDR presentation-gamma dispatch when a validated native frame will overwrite its entire output. SMAA retains this dispatch because its edge calculation reads the SDR image; the decision follows any pending live AA change. Reference/menu/video frames and the separate HUD-less frame-generation chain retain their original gamma processing. This change does not skip guest rendering or the scene/GUI composition.

The 4K benchmark runs the real gamma and native-encoder kernels in six alternating batches of 80 iterations. Every final pixel is identical before and after removing the redundant dispatch:

| API | Sharpening | Before GPU ms | After GPU ms | Saved GPU ms |
|---|---:|---:|---:|---:|
| D3D12 | 0 | 0.6308 | 0.3763 | 0.2545 |
| D3D12 | 0.35 | 0.6257 | 0.3761 | 0.2496 |
| Vulkan | 0 | 0.6629 | 0.3821 | 0.2808 |
| Vulkan | 0.35 | 0.6615 | 0.3795 | 0.2820 |

These are native presentation-chain measurements, not whole-game FPS gains. Before PsychoV35, Gears 3 cached its CPU tangent solve until any of its seven curve parameters changed. A repeated-curve benchmark measured 1.2566 to 0.0024 microseconds per call with an identical checksum; every input parameter is individually tested for cache invalidation. The whole-frame impact was small; PsychoV35 removes this solve and cache.

The optimized Gears 2 and fade-preserving Gears 3 builds are installed in their existing EXE locations. Executable/runtime dependency checks passed, along with all 16 AA/upscaler GPU range checks. Temporary live shader previews were moved out of the DevKit live directory; the executable owns the current mapping.

## Comparison with Gears 2 grading

The captured Gears 2 final material uses analytic color grading, while Gears 3
uses a finite 3D LUT after its filmic curve. Both profiles capture real lighting
before their SDR ceilings: Gears 2 after its black offset and before exposure,
and Gears 3 before the filmic curve. The current bridge extends the known Gears
2 grade and extrapolates Gears 3's filmic/LUT response before PsychoV35. The
unchanged vanilla programs supply graded hue, low-range and fade references.

## Chapter access for HDR coverage

The existing Gears 3 engine-console bridge can dispatch `UnlockAllChapters`.
To trigger it after the campaign player exists, add this line to
`extra-args.txt` beside the COGBox launcher (this checkout uses
`Gears2-Native/cogbox/build/extra-args.txt`):

```text
--ue3_hotkeys=F7=UnlockAllChapters
```

Launch Gears 3, load a campaign, press F7, then return to the game's Chapter
Select. If `ue3_hotkeys` is already configured, append
`;F7=UnlockAllChapters` to that existing value instead of adding a second
switch. An explicit hotkey list replaces the default cheat hotkeys.
The engine log reports `ue3_console: ... 'UnlockAllChapters' done` or
`not recognised by the engine`; dispatch and actual menu unlocks are separate
checks. This command has not been exercised in the current HDR session, so
its saved-progress effect and chapter coverage remain unverified. It does
not require the `gow3_cheats` package patch. This concerns campaign chapters,
not missing multiplayer or DLC map content.

Prefer the game's Chapter Select for captures: the launcher's old direct
chapter URL shortcut was removed because it could bypass checkpoint and
chapter scripting. The separate `gow3_unlock_all_chapters` test flag is not
a complete standalone unlock: without known `gow3_unlock_chapter_settings`
IDs/masks it only enables Insane.

The HDR replacement reads the active 3D LUT bound at texture slot 3 for
each recognized scene draw; it does not embed one level's LUT. Changing LUT
contents in that same shader path therefore uses the current game grade.
Different final-post-process shader permutations still need captures and
profiles. Chapter access helps collect that evidence; it does not establish
whole-game or multiplayer-map HDR coverage by itself.
