# Gears 2 native HDR investigation

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

The scene extension removes the upper SDR limits at instructions 70 and 83. Instruction 72's absolute operand becomes a nonnegative floor, preventing newly exposed negative shadow values from turning into positive light. The output is decoded to linear light. A smooth hue-preserving shoulder above scene reference white approaches the configured peak before bounded HUD color is composited.

SMAA uses its existing edge/weight calculation but blends the HDR color into another FP16 target. Resizing uses normalized positive weights; sharpening is bounded by its neighborhood. The authored shader in `native_hdr_shaders.h` reads the retained float color and converts nits directly to BT.2020 / ST.2084. It applies no highlight expansion to finished SDR pixels. Menus, videos and unsupported passes use a reference-white fallback without ITM.

| Role | Original host | Guest hash | HDR host |
|---|---|---|---|
| Scene | `9A12A665` | `B6B6DD4E7D91F3D9` | `6C0C5E5B` |
| Canvas | `8833D4DF` | `A9AC71DCCCEAEBE4` | `5C48BA01` |
| Additional Canvas material | `4AF4124E` | `3F3410143BA49FE9` | `6E7816BE` |
| Animated reload HUD | `CA04DB7A` | `CDA0455F4F4EAFB4` | `FA4CF4DE` |
| Animated Canvas material | `41D2BE35` | `790DEB5680F51A2D` | `ED5A8350` |
| Additional Canvas material | `AD0932AA` | `CF8D65F2EAD24695` | `5897CB2C` |

The active-reload firing capture identified `CA04DB7A` after the main HUD. It samples the exact same 512x512 BC3 HUD atlas as the basic Canvas shader, plus an 8x128 mask ramp. Omitting this draw from float composition caused reference fallback for the entire frame, clamping the otherwise HDR muzzle flash. The additional Canvas materials now remain in the bounded float composition.

## Damage / scene-textured UI

The full-screen material `ECE0CF7069CD4A91` (original host `3A29E910`) samples the resolved SDR scene at t0 and four UI/noise textures. Bounding this material's entire output would also bound the world behind the damage effect. It therefore uses a separate, fingerprint-checked path:

1. Build a floating gamma-encoded proxy from the real native scene; retain the original SDR resolve.
2. Evaluate the same original material twice into FP16 MRTs, once with each scene input. Keep the game's material math, tint, alpha test, sampler and blending. Extend texture gamma only above the original UNORM range.
3. Restore their difference onto the game's bounded result: `native = bounded_game_result + material(HDR_scene) - material(SDR_scene)` in linear light. Equal UI-only emission cancels; actual scene HDR energy remains. Bound invalid/negative values and the configured display peak.

There is no whole-screen UI-white cap or temporal transition to SDR. The damage material's vanilla colour differences remain. Ordinary HUD/reload elements use their independent UI white; the scene-sampling material retains the game's bounded reference-white base. D3D12 pairs a matching HLSL vertex twin with the DXIL pixel shader; Vulkan uses SPIR-V.

Additional Canvas materials use the two captured Canvas vertex shaders (`868DFF11A6BDEBAA`, `E775468CA654846D`), final-target ownership, post-scene order, straight-alpha blending and disabled depth/stencil. This keeps new popup/reload materials in float composition without globally replacing shared pixel shaders.

## COGBox overlays

SDK patch `Gears2-Native/patches/sdk-patch80-hdr-overlays.diff` adds a separate HDR10 pixel shader to the D3D12 immediate drawer. It bounds SDR input, decodes sRGB and converts to PQ/BT.2020 at a fixed 203-nit white. This covers FPS text, shader-preparation notes, menus, achievements and other host overlays. SDR retains its original shader. The patch includes readable HLSL and its FXC SM5.1 header; rebuilding/installing the SDK and replacing `rexruntime.dll` applies it.

SDK patch `sdk-patch81-vulkan-hdr.diff`, applied after 80, requests `VK_EXT_swapchain_colorspace`, selects an advertised 10-bit ST.2084 surface format and uses a matching SPIR-V host-overlay shader. A missing HDR10 surface is reported instead of presenting PQ in an SDR colour space.

## AA and Vulkan

Native scene/Canvas DXBC variants also translate to SPIR-V. D3D12 GPU range checks cover AA off, SMAA, SMAA T2x, DLSS/DLAA, FSR and XeSS, including native-size and quality-size vendor upscaling. Vulkan range checks cover AA off, SMAA, SMAA T2x and FSR native/quality. DLSS and XeSS remain D3D12-only in the existing project.

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
