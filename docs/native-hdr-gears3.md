# Gears 3 TU6 native HDR

The Gears 3 profile uses the shared renderer's FP16 scene/HUD composition and native PQ encoder. Enable COGBox's existing HDR option; its paper-white and peak controls are used directly. The legacy SDR highlight-expansion controls are unused by this encoder.

## Captured path

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

## Scene extension

`Gears3SceneSource` retains the entire original material, including packed-light decoding, DOF, bloom, grain and 3D LUT grading. It captures nonnegative scene color immediately before the filmic curve at instruction 28.

The CPU reconstructs the draw's actual filmic parameters from pixel constants c4, c5, c252 and c253. It finds the point at 75% of the curve's encoded white and passes its linear-light tangent to the shader. Above this pivot, the shader adds only the energy lost relative to that tangent, using the real pre-curve scene input. It maps that reference energy through the display shoulder, then reapplies the game's LUT grading and output/fade scaling as an RGB throughput factor. Below the pivot the original material result is unchanged. A hue-preserving bound prevents a graded output exceeding the configured display peak.

The highlight revision maps that added native energy into the display headroom, `peak / sceneWhite - 1`, before the shoulder. The first build scaled the added energy with scene white alone, leaving ordinary highlights far below a high configured peak; increasing paper white consequently brightened both the scene and its highlights. This revision uses the selected peak to extend the graded highlight output while retaining the original pivot, LUT hues, low values and independent UI white. The 3D LUT texture itself remains unchanged.

The final scene draw is replayed into linear `R16G16B16A16_FLOAT`. Native HDR presentation reads this image directly, bypassing the integer EDRAM resolves and guest-memory presentation copies. It converts scene-white-relative color to nits and BT.2020/PQ without inverse tone mapping. The original guest target layouts remain intact for aliasing and later guest reads.

## HUD, reload and damage

HUD output RGB and alpha are bounded only during the identified post-tonemap float replay. The same shader's ordinary scene uses retain their original output. UI white is independent of scene white, with a default of 203 nits and a display-peak bound.

The captured reload material samples the BC3 HUD atlas and an 8x128 mask. The captured damage material uses a 512x512 R8 mask, straight-alpha blending and a recognized Canvas vertex shader. It does not sample the resolved world image. Its bounded source is composited over the retained HDR scene; no whole-screen UI cap is applied. Vanilla damage tint and opacity remain part of that blend.

The pause/menu capture also identified independent color, atlas, image and mask shaders (`A7AB2D75C169DFDE`, `19055C4D86C6C08C`, `B8A5DDABA29740E5`, `1DE36FC34E4B7E48`). The multiplicative darkening layer `A8EEB25041FFBA45` is decoded into a bounded linear modulation factor with no UI-white gain. It darkens the retained scene according to the original mask instead of imposing an SDR ceiling on the world.

A later damage capture found another blood-overlay material, `9FCCAF2C4923CA93` (host `D825C47C`, 129 dwords, fingerprint `5CF7251E4670B002`). It samples three BC3 UI textures and uses straight-alpha blending, with no world-scene sample. Its missing profile invalidated the native frame and caused the entire presentation to fall back to SDR. It now has its own bounded float replay. The same capture also covers overlay `63C7939AE43787AB` (54 dwords, fingerprint `59ABADA2DC6A676A`); solid-color UI `D2B5DF9B2B04E715` (42 dwords, fingerprint `FF8DB83E451A0BF5`) has an explicit profile too.

The shared renderer also recognizes the two Gears 3 Canvas vertex shaders `6F83A894AB66D831` and `2E88B37100D9D0CE`, restricted to the owned final target, straight-alpha blending, post-scene order and disabled depth/stencil. Unsupported later writers still produce a logged reference-white fallback. Additional scenes or full-screen scene-sampling materials need capture and validation before claiming complete game coverage.

COGBox FPS/menu/achievement overlays use the existing SDK HDR overlay patches at 203-nit white.

## Controls and backends

| Existing option | Native path |
|---|---|
| `present_hdr` | Select HDR presentation; restart required |
| `gow2_hdr_paper_white` | Scene reference white in nits |
| `gow2_hdr_peak` | Display peak and highlight shoulder |
| `gow2_hdr_ui_white` | Independent game HUD white in nits |
| `gow2_hdr_native` | Enable captured native profiles, default true |

The shared `gow2_` option names are intentional: COGBox already passes them for Gears 3. D3D12 uses a matching DXIL vertex/pixel pair for the scene; Vulkan uses SPIR-V. The captured scene pipeline compiles on both APIs.

`gn_hdrtest d3d12 gears3` and `gn_hdrtest vulkan gears3` exercise the actual source transformation with authored inputs, three curve exposures and two peak values. They check exact SDR lows, highlights above SDR white, retained graded hue, finite nonnegative output and peak bounds on both APIs. The existing UI-isolation GPU checks verify world-HDR retention, bounded UI emission, partial coverage, tint and negative-safe gamma. All 16 existing AA/upscaler range checks pass: D3D12 off/SMAA/SMAA T2x/DLSS/FSR/XeSS and Vulkan off/SMAA/SMAA T2x/FSR, including vendor quality-size cases.

Vulkan FSR requires SDK 2.4 headers and signed Vulkan DLLs (`GN_FFX_DIR=Gears2-Native/third_party/fidelityfx-sdk-v2.4.0/Kits/FidelityFX`). DLSS and XeSS are D3D12-only in the existing wrappers. GPU range tests do not establish gameplay motion quality, frame-generation behavior or full-game shader coverage.

The rebuilt AMD64 executable is installed at the launcher's existing `Gears3-Native/build/Release-tu6/gow3.exe` path, with matching FSR 2.4 D3D12/Vulkan runtime DLLs. Executable hashes, runtime hashes and imported dependencies passed verification. No launcher executable-path change is needed.

Live D3D12 validation after restart confirmed the native scene and matching vertex shader draw into the dedicated 3840x2160 FP16 image, which is sampled directly by native presentation. With DLSS Quality, scene white 200 nits and peak 3800 nits, the live image contained 1,865 pixels above SDR white; RGB maxima were 2.0410, 2.6953, 2.4941 in scene-white-relative units. All 8,294,400 pixels were finite and nonnegative. The reference-frame count stayed at 1,074 while native frames increased to 18,126 during this observation; this demonstrates stable native presentation over that interval, not complete game coverage. Logs also confirmed that the bounded HUD, reload, damage-mask and menu shader variants loaded successfully. A complete live firing/reload/damage sequence still needs verification.

Extracted microcode/HLSL, readbacks and transient shader binaries remain under ignored `work/native-hdr-gears3`; no game shader data is included in the source contribution. Live readback results describe the current resource, not a frozen historical snapshot.

The revised highlight source was previewed through DevKit in the running first build. A live FP16 readback measured maximum luminance 16.36085 scene-white units (about 3,272 nits at 200-nit scene white), with every pixel finite and nonnegative. This was a different live image from the earlier readback, not a controlled same-frame comparison. The highlight and blood-overlay revision was installed in the existing executable (SHA256 `D5EAECE0362C8AD52CFA83A2540E205BE5BE2F9224C4FD0DDBE0E57F74995C1A`) and is superseded by the fade-preserving revision below. Its damage/reload materials passed real FP16 alpha-blend pipeline creation on D3D12 and Vulkan.

## Fade preservation and shared optimizations

The first highlight-headroom revision normalized the graded pixel before adding HDR light. This could cancel a game's fade toward black. The regression test reproduced a half-faded highlight retaining red 0.85791 instead of the expected 0.429932. The corrected shader retains the complete LUT/output scaling and reapplies it after the HDR shoulder. Tests now pass for 100%, 50%, 0.1% and exactly black output at three curve exposures and two display ranges on D3D12 and Vulkan. The original below-pivot result, graded hue and display-peak bound are checked too. Full live gameplay transition verification remains pending a restart of the installed revision.

Both games now skip the SDR presentation-gamma dispatch when a validated native frame will overwrite its entire output. SMAA retains this dispatch because its edge calculation reads the SDR image; the decision follows any pending live AA change. Reference/menu/video frames and the separate HUD-less frame-generation chain retain their original gamma processing. This change does not skip guest rendering or the scene/GUI composition.

The 4K benchmark runs the real gamma and native-encoder kernels in six alternating batches of 80 iterations. Every final pixel is identical before and after removing the redundant dispatch:

| API | Sharpening | Before GPU ms | After GPU ms | Saved GPU ms |
|---|---:|---:|---:|---:|
| D3D12 | 0 | 0.6308 | 0.3763 | 0.2545 |
| D3D12 | 0.35 | 0.6257 | 0.3761 | 0.2496 |
| Vulkan | 0 | 0.6629 | 0.3821 | 0.2808 |
| Vulkan | 0.35 | 0.6615 | 0.3795 | 0.2820 |

These are native presentation-chain measurements, not whole-game FPS gains. Gears 3 additionally caches its CPU tangent solve until any of its seven curve parameters changes. A repeated-curve benchmark measured 1.2566 to 0.0024 microseconds per call with an identical checksum; every input parameter is individually tested for cache invalidation. Its whole-frame impact is small.

The optimized Gears 2 and fade-preserving Gears 3 builds are installed in their existing EXE locations. Executable/runtime dependency checks passed, along with all 16 AA/upscaler GPU range checks. Temporary live shader previews were moved out of the DevKit live directory; the executable owns the current mapping.

## Comparison with Gears 2 grading

The captured Gears 2 final material uses analytic color grading, not Gears 3's final 3D LUT. Its native profile removes the SDR ceilings at instructions 70 and 83 and corrects the negative-value handling at 72. This directly exposes values above 1 through the remaining analytic math.

Gears 3 instead applies a saturating filmic curve at 41, bounded grain at 45, coordinates at 46 and a finite 3D LUT lookup at 47. Removing the saturations alone still leaves the lookup at the edge of its cube. Equivalent HDR grading therefore needs an explicit extension of the LUT response, or retention of its bounded grading while restoring native highlight energy. This build implements the latter, using the real pre-curve scene input and retaining the LUT's original hue. It does not yet extrapolate texture samples outside the LUT cube. A direct LUT extrapolation would need its own boundary-continuity, color and flat-endpoint tests before replacing the current installed mapping.
