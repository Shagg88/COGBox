<!-- Note 75: an outside Vulkan analysis (Shagg, 26 Oct 2026), saved for when Vulkan work resumes after 0.1.0.
     Its leads are hypotheses from reading the source and notes, not new test results - reproduce first.
     Already done since it was written: job 516 added the VkResult checks (bind / views / samplers / pipeline cache). -->

# COGBox Vulkan Analysis and Recommendations

**Scope:** Vulkan correctness, the Gears 3 dark/black-block discrepancy, renderer performance, diagnostics, and build readiness. This is a focused engineering analysis based on the source and notes currently present in `C:\COGBox`, plus current Vulkan primary documentation.

**Source status:** No project source files were changed. This document contains hypotheses and proposed experiments, not newly executed validation runs. Historical results below are attributed to the project notes and should be reproduced against the current source/build before treating them as current behavior.

## Executive summary

The best-supported lead for the Gears 3 Vulkan picture mismatch is the GPU texture loading path, especially the BC1–BC5 block untile path. In the recorded frame-1500 test, draw 303 produced about half the red attenuation on Vulkan. Disabling both `gow2_renderer_gpu_block_textures` and `gow2_renderer_gpu_texture_load` moved the result close to D3D12. That result is not conclusive because both switches changed together and that run's depth-copy input changed to all zero. The immediate next step is a controlled one-switch-at-a-time reproduction with identical draw inputs, followed by byte-for-byte comparison of every BC mip and layer at the loader's intermediate stages.

The black patches appear to be a second defect. The notes associate them with roughly 3.3% more unwritten pixels in later lighting draws (483–506), whereas draw 303 explains much of the scene darkening. Treat coverage and intensity as separate measurements until evidence ties them together.

There is also a separate Vulkan performance track. The current shared backend has made useful progress by avoiding some unnecessary pass-start barriers and supporting attachment-optimal layouts, but the checked-in defaults still include broad stage/access synchronization and GENERAL attachment layouts. The per-resource synchronization plan in `notes/71-vulkan-sync-design.md` is the right direction, but it is a high-risk change because the games use EDRAM aliasing, resolves, depth/stencil reinterpretation, memexport, and sampled render targets.

## Priority 0: reproduce the Gears 3 image defect with controlled inputs

### What the project has already established

The project record in [Gears 3 Vulkan black blocks](../../Gears3-Native/notes/26-vulkan-black-blocks.md) says:

- Gears 3 was darker on Vulkan and had black patches; Gears 2 and Judgment matched D3D12 in the tested cases.
- Enabling the full pass-barrier mode and changing transfer/texture-post options did not fix the discrepancy.
- Switching shader backend and changing clear-in-pass behavior also did not fix it.
- Draw 303 (pixel shader FC367470036D7CFE) wrote about 0.064 red on Vulkan versus 0.132 on D3D12 in one comparison. It samples a depth copy and BC1/BC3 textures and writes a red-only blended attenuation result.
- In a later comparison, draw 303 still differed despite a reported matching depth-copy input and target contents.
- The CPU texture path run moved the Vulkan draw result from about 0.063 to 0.121, near D3D12's 0.132. However, that run disabled both BC block loads and general GPU texture loads and reported an all-zero depth copy, so the changed result cannot yet be assigned to BC decoding or texture loading alone.
- Later lighting draws added about half as much on Vulkan, and a separate set of draws left more pixels unwritten.

The current implementation confirms these two GPU texture-path controls are distinct:

- `gow2_renderer_gpu_block_textures` gates GPU loading for BC1–BC5 textures in `Gears2-Native/src/layer0_xenos/renderer/texture_cache.cpp`.
- `gow2_renderer_gpu_texture_load` gates the GPU untile path for uncompressed 2D textures.
- The BC path dispatches `untile_blocks_cs` into scratch, then calls `CopyScratchToTexture` for each mip/layer.

### Run this minimal A/B matrix first

Use the same Gears 3 executable/build, same scene, same captured frame/draw, same GPU and driver, and compare exact draw 303 input/output readbacks. Keep the depth copy nonzero and hash it before the draw. Repeat each case at least three times to detect timing-dependent input drift.

| Case | `gpu_block_textures` | `gpu_texture_load` | Purpose |
|---|---:|---:|---|
| A | true | true | Current Vulkan baseline |
| B | false | true | Isolate BC block loader |
| C | true | false | Isolate general uncompressed GPU loader |
| D | false | false | Reproduce the prior CPU-path result |

For each case record:

1. Hash or dump the exact guest-memory byte ranges for every texture sampled by draw 303, including the GPU-current mirror pages. A CPU-side guest-memory copy may be stale when a resolve or memexport updated the GPU mirror.
2. Dump the loaded Vulkan image data for every sampled mip and layer before draw 303. For BC data, compare bytes in compressed-block order, not only decoded screenshots.
3. Record texture fetch constants, effective format, endian mode, base/mip offsets, row pitch, packed-tail layout, sampler min/mag/mip filters, LOD bias/range, view format, and selected LOD.
4. Record the draw's depth-copy texture hash and sampled values, target hash, shader/pipeline identity, blend state, and output readback.
5. Compare Vulkan against D3D12 with the same captured inputs. A run with different input hashes is not a valid A/B for assigning cause.

This matrix is more informative than repeating the prior combined toggle. If B fixes the output while C does not, focus on BC untile and mip layout. If C fixes it while B does not, focus on the uncompressed loader or shared mirror upload. If only D fixes it, examine interactions or stale guest-memory mirror behavior.

## Priority 1: inspect and validate the BC texture upload path

The relevant code is in:

- `Gears2-Native/src/layer0_xenos/renderer/texture_cache.cpp` — selects GPU block loading, computes block dimensions, builds per-level constants, and dispatches the compute untile shader.
- `Gears2-Native/src/layer0_xenos/renderer/builtin_shaders.h` — contains `kUntileBlocksCs`, the BC block untile compute shader.
- `Gears2-Native/src/layer0_xenos/renderer/vulkan_backend.cpp` — implements `PrepareScratchFor` and `CopyScratchToTexture`.

Recommended checks, in order:

1. **Read back scratch immediately after the compute dispatch.** Compare it with the CPU untile output for the same guest byte range. This separates address/tile/endian bugs from Vulkan image-copy and sampling bugs.
2. **Compare each subresource independently.** Log format, mip, face/layer, dimensions, block width/height, row pitch, storage base, packed-tail offsets, and byte hash. Do not compare only mip zero: draw 303 uses implicit texture sampling, so a wrong mip or a single bad packed tail can look like an alpha/blend problem.
3. **Use BC boundary cases.** Include 4×4, dimensions just over a block boundary, odd mip dimensions, the last mip, packed tails, and each supported BC format. The current loader rejects some base dimensions that are not 4-aligned, but smaller mips naturally reach partial-block dimensions and must still copy correctly.
4. **Validate the scratch-to-image copy.** Vulkan buffer/image copies express image extent in texels while the data addressing uses the format's texel-block geometry. Check the scratch row pitch and extent against the selected Vulkan format and validate all mip/layer offsets. Khronos documents this distinction in [compressed image copies](https://docs.vulkan.org/guide/latest/image_copies.html) and the [copy specification](https://docs.vulkan.org/spec/latest/chapters/copies.html).
5. **Check BC format compatibility and features on each GPU.** Log the selected VkFormat and queried sampled-image/transfer support; do not assume every driver exposes identical BC format capabilities.
6. **Compare the compute shader's guest address calculation** with the CPU untile implementation for the exact BC formats and tiled/linear modes encountered. Focus on packed mip-tail address selection, cube-face stride, row pitch units (bytes versus blocks), endian swap, and the mapping from output invocation IDs to block coordinates.

The Vulkan sampling specification says implicit LOD depends on neighboring fragment coordinates, and implicit LOD can be undefined if the image or sampler varies within a quad without the relevant feature. This is a secondary hypothesis, not the leading one: verify whether the translated shader uses implicit sampling and whether its descriptor selection is uniform within each quad before changing shader translation. See [Vulkan sampling and derivative LOD rules](https://docs.vulkan.org/spec/latest/chapters/textures.html).

A useful diagnostic, if draw 303's shader path allows it without affecting unrelated draws, is to force a known explicit LOD or sample each mip explicitly into a small readback. If explicit LOD makes Vulkan agree with D3D12, inspect gradients, descriptor uniformity, sampler state, and view mip ranges. If it remains wrong, the loader/image contents or blend/format path remains more likely.

## Priority 1: investigate black-patch coverage separately

The project notes report that draws around 483–506 leave about 3.3% more pixels unwritten on Vulkan. Track this as a coverage defect independent of draw 303's attenuation mismatch.

For the exact affected draws, capture and compare:

- per-draw color write masks and a binary written-pixel mask;
- depth and stencil contents before/after the draw, including sample count and sample locations;
- viewport, scissor, render area, clipping, culling, front-face, depth compare/write, stencil compare/write, and blend state;
- primitive/vertex counts and rasterized coverage;
- the destination target's initialization/clear value and all alias/transfer events before the draw;
- draw ordering and target contents immediately before/after each suspect draw.

The notes mention an infinity in a large Vulkan target as another possible source of black blocks. Add a finite-value scan after the relevant draws and after resolve/copy; record the first writer and texel coordinates for NaN/Inf. This distinguishes missing coverage from invalid source values that later become black.

Full pass barriers did not change the earlier black-block result, which weakens a simple “missing pass barrier” explanation. It does not prove all synchronization correct: the project has EDRAM image aliasing, depth/stencil conversions, MSAA transfers in Gears 3, and resolve dependencies. Check synchronization validation on a deterministic capture and inspect actual source/destination resource barriers around the first bad draw. Vulkan makes the application responsible for resource synchronization; image layout transitions themselves have ordering and read/write requirements. See [Khronos synchronization guidance](https://docs.vulkan.org/guide/latest/synchronization.html) and the [Vulkan synchronization specification](https://docs.vulkan.org/spec/latest/chapters/synchronization.html).

## Priority 2: complete a measured resource-scoped synchronization path

The current implementation in `vulkan_backend.cpp` batches pending barriers, but its fallback path still uses broad memory access masks and ALL_COMMANDS destinations; image transitions span remaining mips/layers. The options currently default to `gow2_renderer_vulkan_barrier_stages=all` and `gow2_renderer_vulkan_attachment_layouts=general`. The checked-in `gow2_renderer_vulkan_pass_barriers=when_needed` option is already a useful improvement and should remain covered by the picture gate.

The project's `notes/71-vulkan-sync-design.md` describes a better target: per-image and per-buffer stage/access/layout state, range tracking for guest-memory writes, no read-after-read barriers, attachment-optimal layouts while rendering, and GENERAL only for storage use. Proceed behind a diagnostic switch and validate incrementally:

1. Build state tracking for one resource class at a time: sampled images, render targets, transfer/resolve images, then guest buffers and memexport ranges.
2. Track exact subresource ranges, not just whole image mip/layer spans, where operations touch only a mip or face.
3. For every operation, declare the intended usage (attachment read/write, shader sample, compute read/write, transfer source/destination, vertex/index input, present) and derive barriers from recorded last usage.
4. Keep the old path as a comparison fallback until all game captures pass.
5. Enable Khronos synchronization validation, capture all validation errors, and run focused Gears 3 ch1 black-block and Gears 2/Judgment regression routes.
6. Require both image equality gates and repeatable GPU timings before changing defaults. Avoid inferring a performance win from fewer barriers alone; barriers can alter overlap and synchronization correctness.
7. Keep a targeted test for EDRAM aliasing and depth/stencil aspects. The synchronization design notes call out D24S8/D24FS8 transfers, aliased 7e3/depth targets, resolves read later in the frame, and memexport-to-vertex/index/texture use. These are essential cases for the tracker.

Khronos explains that broad synchronization can cause unnecessary GPU idle time and provides synchronization validation; see [the synchronization guide](https://docs.vulkan.org/guide/latest/synchronization.html). The project notes themselves record that the full-barrier path and narrow-path comparisons require both picture gates and timing repeated across runs.

## Priority 2: expose backend/build capability clearly

Vulkan enablement spans both the local renderer and the rexglue SDK. The current CMake setup can compile a stub when Vulkan headers are unavailable, and the SDK provider requires a Vulkan-enabled SDK build. The runtime has logs for provider failure/fallback; release and development builds should make the active backend unambiguous.

Suggestions:

- Add a startup diagnostic that prints requested API, actual active API, device name/vendor/device ID, driver version, Vulkan API version, enabled extensions/features, validation-layer status, and whether the renderer is the full Vulkan backend or a stub.
- Treat a requested Vulkan backend that silently falls back to D3D12 as a failed Vulkan test. Make harness scripts assert the actual active API before accepting screenshots or timing results.
- Keep a CI/build matrix for all three games covering Vulkan headers present/absent and SDK Vulkan on/off, and ensure the release binaries intended to support Vulkan use the correct SDK snapshot.
- At startup log missing Vulkan loader/provider/extension reasons in a machine-readable form. This helps distinguish project bugs from test runs that never activated Vulkan.

Relevant build sources include the CMake files and `Gears2-Native/src/layer0_xenos/gpu_system.cpp`. The notes document the `REXGLUE_USE_VULKAN=ON` SDK requirement. Historical Gears 3 testing used a Vulkan-enabled SDK snapshot, but check the present build rather than assuming that old configuration remains active.

## Priority 3: make Vulkan diagnostics useful for real failures

- Add Vulkan debug-utils labels for render passes, game draw IDs, resolves, texture untile dispatches, and EDRAM transfers. The tooling notes say the backend does not yet add Vulkan labels. Labels let RenderDoc/Nsight captures identify draw 303 or the first bad coverage draw directly.
- Add a compact per-draw diagnostic record for the first divergence: shader hash, pipeline key, texture hashes/LOD, render-target hash, blend/depth/stencil state, and barrier/resource transitions.
- On `VK_ERROR_DEVICE_LOST`, query `VK_EXT_device_fault` when available and save the fault data/binary alongside the run metadata. It is optional and vendor support varies; it is a diagnostic aid, not a correctness fix. See [VK_EXT_device_fault](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_device_fault.html).
- Include GPU/driver/build identity and a random run ID in diagnostic reports. Avoid including user names or absolute user paths in shareable logs.

## Performance recommendations (separate from correctness)

The project notes recorded Vulkan pipeline startup improvements, but also first-visit pipeline creation hitches and a remaining GPU-time gap in some scenes. Keep performance investigation separate from the Gears 3 image fix.

1. Use timestamp queries around GPU work and pass/dispatch regions, not only CPU frame time. The project already has per-target GPU timing and should extend it to the suspicious BC loader, draw 303, EDRAM transfers, and barriers.
2. Benchmark warm and cold pipeline-cache runs separately. Record cache hit/miss counts, pipeline creation duration, driver version, and whether the capture is from first visit or steady state. Khronos describes pipeline cache use in the [Vulkan sample guide](https://docs.vulkan.org/samples/latest/samples/performance/pipeline_cache/README.html).
3. Use a GPU trace to determine whether frame gaps align with barriers, pipeline creation, memory traffic, or shader execution. Do not assume the remaining cost is synchronization; the project notes list shader memory access as another candidate.
4. Compare the same scene and target across at least three runs and report median plus range. Existing notes describe scene-target timings varying by around ten percent between nominally identical runs, so small changes need more samples.
5. Investigate device-local scratch and guest-mirror bandwidth. Keep counters for bytes copied, BC blocks processed, dispatch count, and per-stage time. A CPU texture fallback is useful diagnostically but is documented as extremely slow and should not be a proposed end-user workaround.
6. Confirm supported features/extensions before experimenting with vendor-specific shader paths; retain a portable baseline.

## Suggested implementation sequence

1. Reproduce the frame-1500 Gears 3 discrepancy with input hashes and confirm Vulkan is truly active.
2. Run the four-case texture-loader matrix.
3. Diff GPU scratch output against CPU untile output for draw 303's BC textures.
4. If texture bytes match, force explicit mip selection and compare decoded samples; then inspect sampler state, SPIR-V sampling instructions, and blending/gamma-format behavior.
5. Independently compare coverage masks and first non-finite values for draws 483–506.
6. Run synchronization validation around these captures; investigate only the first resource hazard or first divergence.
7. Implement resource-scoped synchronization under a switch and use targeted image gates before broad playthroughs.
8. After correctness is stable, profile first-visit stutter and steady-state GPU time separately, then adjust defaults only when measurements support the change.

## Project files reviewed

- `Gears2-Native/notes/68-vulkan-plan.md`
- `Gears2-Native/notes/70-renderer-overview.md`
- `Gears2-Native/notes/71-vulkan-sync-design.md`
- `Gears2-Native/notes/74-ps-refactoring.md`
- `Gears3-Native/notes/26-vulkan-black-blocks.md`
- `Gears2-Native/src/layer0_xenos/renderer/texture_cache.cpp`
- `Gears2-Native/src/layer0_xenos/renderer/builtin_shaders.h`
- `Gears2-Native/src/layer0_xenos/renderer/vulkan_backend.cpp`
- `Gears2-Native/src/layer0_xenos/renderer/shadow_renderer.cpp`
- `Gears2-Native/src/layer0_xenos/gpu_system.cpp`
- Vulkan-related sections of `Gears2-Native/CMakeLists.txt` and `Gears3-Native/CMakeLists.txt`

## References

- [Vulkan sampling specification: implicit and explicit LOD](https://docs.vulkan.org/spec/latest/chapters/textures.html)
- [Vulkan synchronization guide](https://docs.vulkan.org/guide/latest/synchronization.html)
- [Vulkan synchronization specification: image layouts and dependencies](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)
- [Vulkan image-copy guide: compressed texel blocks](https://docs.vulkan.org/guide/latest/image_copies.html)
- [Vulkan copy specification](https://docs.vulkan.org/spec/latest/chapters/copies.html)
- [VK_EXT_device_fault reference](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_device_fault.html)
- [Khronos pipeline-cache sample](https://docs.vulkan.org/samples/latest/samples/performance/pipeline_cache/README.html)


