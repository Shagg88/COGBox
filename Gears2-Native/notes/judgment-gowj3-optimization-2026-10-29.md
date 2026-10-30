# Judgment: gowj3.tracy investigation and renderer changes

This pass uses `C:/Users/ctied/Documents/gowj3.tracy`, the launcher log `Judgment/logs/20261029-181244`, and a new 45-second capture recorded after the player reached the heavy area. The player described the original location as approximately the third checkpoint of the opening chapter.

## What the supplied capture establishes

The 117.61-second capture aligns with native frames **1,253–11,370**: 10,118 frame values match the CSV to within its rounding precision. The run used 3x resolution, SMAA, strict occlusion, D3D12 debug **off**, and **texture-format constants already enabled**. Enabling that flag again cannot explain an improvement relative to this capture.

The loading transition is separate from gameplay. Frame 2,462 takes 89.02 ms and uploads approximately 66 MB; frame 2,463 takes **186.46 ms**, with a very long GPU sample. Launch shader preparation does not preload all textures or remove level-streaming work.

After frame 2,464, the supplied recording averages **88.59 FPS**, with **11.29 ms/frame** and **11.08 ms of GPU work**. Drawing accounts for approximately 7.39 ms, resolves 1.74 ms, texture loading 0.86 ms and EDRAM transfers 0.72 ms. These are asynchronous completion measurements; individual GPU samples can be attributed later than their originating CPU frame.

The largest gameplay frames are about 34 ms. Several smaller spikes coincide with long `ExecuteCommandLists` API calls on the recording worker. Close, allocator reset, list reset and fence signal are much shorter. This narrows the recorder stalls to submission/driver/scheduling activity, but a CPU zone around a driver call cannot establish whether the thread was executing or descheduled. GPUView/PIX/Nsight scheduling data would be needed to distinguish those causes.

There are only **two new translated shaders and one new pipeline** in the aligned original capture, with zero shader-hold time. Shader compilation is not the main cause of these gameplay spikes.

## Changes

* **Texture-format constants default on.** The existing DXBC optimization computes format/sign/filter decoding metadata on the CPU and uploads two words per fetch slot. It replaces repeated per-pixel decoding. `--gow2_renderer_tex_post_constants=false` restores the reference path. Variant-aware shader cache identities already distinguish the two paths.
* **Unscaled resolve-through enabled.** The existing resolve dispatch also refreshes an eligible cached texture, eliminating its later untile/reload. Unsupported layouts and stale partial coverage retain the old loader.
* **New scaled resolve-through.** An existing scaled-memory write dispatch can also write the typed, cached texture that a draw samples. Eligibility requires the same base, pitch and bytes per texel; a single tiled 2D level outside the packed tail; and complete origin-aligned texture coverage. The output uses the same endian transforms and format conversion as the existing scaled loader. A page-version check rejects unaccounted overlapping writes. The normal scaled-memory copy remains available for different views, CPU coherence and later readers. This is a conservative part of proposals 3/6, **not elimination of both intermediate copies**.
* **Stronger invalidation.** Full-coverage unscaled updates now check the expected version increment as partial updates already did. A racing write leaves the cache generation behind, forcing reconstruction.
* **Selective fused resolve option.** The optional fused path now defaults to a minimum **65,536 guest pixels**; smaller resolves keep the reference path. Failed fused setup falls back to the ordinary scaled resolve. Fusion remains **off** because earlier exposure differences are unresolved; area filtering alone is not proof of correctness.
* **Query wakeup hardening.** Occlusion completion uses a manual-reset event that stays signalled until the fence target changes. The renderer rechecks pending completion after at most a requested 2 ms event-wait timeout rather than a 100 ms recovery timeout. Windows scheduling can still delay the thread beyond the requested timeout. Real sample counts and query-report reuse guards are preserved. This addresses a possible source of long query waits; it is not yet proof that all observed long waits had that cause.
* **Presentation imports retain their descriptors and resource references until the final GPU fence.** The previously staged change is retained; the new player capture reports no synchronous fallbacks and bounded pending resources.
* **Retain compiled-source inventories per shader configuration.** The launcher session `20261029-195815` explicitly switched texture-format constants to false, replacing the single legacy inventory. Returning to true subsequently spent **242.4 seconds** preparing 259,374 source shaders, despite 123,249 driver pipeline cache hits. The renderer now keeps separate validated inventories for each translator/template configuration. It validates referenced stage records individually, so appending another configuration's stages does not discard all existing readiness. Missing/truncated records are pruned and rebuilt. Driver/adapter/library identity checks remain required; the current valid legacy inventory migrates without recompiling. First use of a configuration can still need preparation, and blob checksums are checked when stages are restored.

Rollback options are `--gow2_renderer_resolve_through=false`, `--gow2_renderer_scaled_resolve_through=false`, `--gow2_renderer_tex_post_constants=false`, and `--gow2_renderer_defer_import_release=false`. The optional fused path is `--gow2_renderer_fused_scaled_resolve=true`; `--gow2_renderer_fused_scaled_min_pixels=0` restores the original unrestricted experiment.

## Evidence collected during this pass

The existing unscaled checker reported **12 comparisons with zero differing texels** in Judgment's opening map. The new scaled-only checker reported **11 comparisons with zero differing texels**, each comparing a 3,840 × 2,160 texture, including colour and depth conversion paths. These checks cover the exercised textures and formats, not every scene in all games.

An initial matched opening-map comparison, before the query-event change, used frames 601–3,000, 3x/SMAA, fixed simulation and camera inputs, uncapped rendering, warm caches, no screenshots and no concurrent builds:

| Measurement | Texture reuse off | Both reuse paths on |
| --- | ---: | ---: |
| Average FPS | 97.23 | 98.95 |
| 99th-percentile frame time | 14.47 ms | 12.81 ms |
| GPU work | 10.05 ms | 9.74 ms |
| GPU texture loads | 1.06 ms | 0.31 ms |
| GPU resolves, including direct refresh | 2.33 ms | 2.85 ms |
| Mean guest draws | 974.7 | 978.5 |

The net GPU saving is approximately **0.31 ms**, much smaller than adding the proposed savings independently. Part of the former texture-load work moves into the resolve category. This opening-map check is **not** the player's heavy checkpoint. A single pair, with slightly different draw counts, is limited evidence of a modest performance gain.

The longer candidate run also contains separate 77/88 ms query-wait spikes after frame 3,000, despite short GPU samples. They are explicitly not hidden by the fixed comparison window and motivated the query-wakeup investigation.

The player then navigated to the heavy area with both texture reuse paths enabled. The 45.10-second trace aligns with frames **15,581–19,636**, 4,056 frames. It averages **89.90 FPS**, p99 **16.80 ms**, maximum **34.75 ms**; one frame exceeds 33.334 ms and none exceeds 50 ms. GPU texture loads average **0.15 ms**, resolves **2.03 ms**, drawing **7.72 ms**. Only one new shader and zero pipelines appear. Submission-related stalls remain. Because the player route differs from the supplied trace, comparing their FPS directly would not establish a causal improvement.

The generic `GearGame_P` checkpoint attempt is excluded: it did not restore Judgment correctly. Screenshot-heavy checks are rendering checks only, excluded from performance comparisons. Original saves/settings were preserved; manual navigation used a separate copied profile.

Final validation:

* **149 tests / 818,076 assertions passed**, including independent configuration inventories, stage-store growth, truncated records, resolve coverage and overlapping-write invalidation.
* **All five normal executables rebuilt:** Gears 2 base/TU6, Gears 3 base/TU6 and Judgment. Both resolve-through paths and CPU texture-format constants default on. Fusion, GPU_UPLOAD and deferred guest-UAV experiments keep their existing opt-in status. Explicit launcher arguments still override defaults: the user's latest `tex_post_constants=false` was preserved.
* The 75-second Judgment query-wakeup check contains **6,960 frames after frame 600**, with maximum **27.77 ms**, no frames over 33.334 ms, and p99 **12.65 ms**. The trace records the new native event waits (p99 2.72 ms, maximum 3.16 ms, including scheduler/lock delay). The former 77/88 ms query spikes did not recur in this run. It is a scripted opening-map check, not a repeat of the heavy checkpoint, and cannot prove all query stalls are fixed.
* Judgment ran with the D3D12 debug layer and passed the exercised resolve comparisons. Gears 3 Gridlock and Gears 2 Hospital also ran with the debug layer: **12/12 GPU comparisons matched in each game**, including full-size scaled depth/colour and smaller unscaled formats. Pipeline-library cache-miss warning 971 occurred and fell back to normal pipeline creation; no D3D12 errors or other debug warning IDs were found in those checks. These cross-game checks disabled full launch preparation to exercise live shader/pipeline fallback; their FPS is not a performance comparison.
* Gridlock's strict-query log reports actual sample counts with no dropped or wrongly hidden queries in checked intervals. The shared query-wakeup change is therefore included in Gears 3. Presentation imports remained bounded with zero synchronous fallbacks in checked runs.
* The new inventory migrated the valid 259,374-source Judgment inventory; its cached package/preparation phase completed in **4.4 seconds**. This excludes earlier process initialization and stage-store indexing.
* A subsequent normal-executable launch loaded the **configuration cache** directly and finished that preparation phase in **4.3 seconds**. Its matching frames 601–3,000 averaged **100.17 FPS**, p99 **12.69 ms**, maximum **19.04 ms**, with 971.7 mean guest draws. The initial control averaged 97.23 FPS with 974.7 draws. This is a modest local improvement of about 3%, not a prediction for the heavy checkpoint.
* The optional area-filtered fused path was exercised with the debug layer. Screenshot comparisons were **inconclusive because the camera views differed**, so pixel differences are not attributed to fusion. The earlier exposure issue remains unresolved and fusion stays off.

Executable backups are under `work/gowj3-optimization/backups`. Source changes remain uncommitted for review. This is a measured, limited optimization pass, not a full renderer rewrite or a claim to eliminate all freezes.

## Other proposals

GPU_UPLOAD remains opt-in: earlier measurements were within run variation, and support depends on the driver/adapter. Fine timestamp splitting remains on for attribution; the previous coarse-timestamp comparison was neutral. Strict query early submissions are preserved because their dependency on visibility results makes blind merging risky. The supplied trace contains approximately three backend Submit calls per frame, not twenty actual executions.

Lazy 1x materialization and a second compute queue are not implemented in this pass. The first needs reliable interception of every CPU and non-scaled reader; write tracking alone is insufficient. The second needs explicit resource ownership and cross-queue ordering plus evidence of independent GPU work. EDRAM transfer elimination also needs proof that a later clear/draw fully overwrites data before any reader. These are separate correctness-sensitive projects, not safe default toggles.

References consulted: [Microsoft GPU_UPLOAD specification](https://microsoft.github.io/DirectX-Specs/d3d/D3D12GPUUploadHeaps.html), [NVIDIA synchronization guidance](https://developer.nvidia.com/blog/advanced-api-performance-synchronization/), and [Xenia's texture-cache design](https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/texture_cache.h).

## HLSL and the pasted flag explanation

COGBox already supports `--gow2_renderer_shader_backend=hlsl`, which translates guest microcode to HLSL and compiles it with DXC. The local CoD3Recomp also translates microcode to HLSL, then compiles it with `D3DCompile` to shader-model-5 bytecode. HLSL is source code, not the representation the GPU executes. CoD3's alpha-test-free opaque variants are a distinct optimization that could be investigated separately; they cannot be transplanted without respecting Gears' kill, coverage, depth and export behavior.

The current direct-DXBC default avoids the slower HLSL compiler stage. Switching all shaders to HLSL would need output and GPU-performance comparisons, and could substantially increase startup compilation. Neither trace identifies shader compilation as the dominant gameplay problem.

In this project `tex_post_constants=false` does **not** disable post-processing or texture constants. It restores per-pixel format decoding. The pasted UWP/Dev Mode explanation does not describe this implementation; its generic [Xbox Emulation Hub link](https://github.com/XboxEmulationHub) contains no documentation supporting those flag-specific claims.
