# COGBox stutter investigation and update

October 29, 2026. Changes are built into the existing game executables used by `C:\Users\ctied\Documents\COGBox\Gears2-Native\cogbox\build\CogBox.exe`. Gears 2 and Gears 3 base/TU6 configurations and Judgment Release all built successfully. Changes remain uncommitted for review.

This update targets streaming-related resource churn and cached shader-stage restoration. It reduces a substantial source of GPU memory pressure. It does **not** establish that every campaign area is stutter-free or that average FPS increased.

## Findings in the recent player logs

Sessions below are under `Gears2-Native/cogbox/build/<game>/logs/`. Frame timings come from each session's CSV; other details come from its text log.

| Game and sessions reviewed | Evidence | Interpretation |
| --- | --- | --- |
| Gears 2: `20261028-205312`, `20261028-192545` | Latest: 28,410 frames, 142 above 50 ms. Worst logged frame: 218.938 ms at frame 4,081, about 41.6 seconds into the frame log. That frame had 207.1 ms renderer CPU time, 107.4 ms measured GPU time, zero new shaders/pipelines, zero shader hold and zero skipped draws. The surrounding slow stretch includes expensive resolves and GPU waits. | That severe episode was not waiting for shader compilation. Renderer work and GPU/memory pressure are the stronger leads. CPU and GPU measurements overlap; they must not be added together. The preceding session also contained texture/resolve stalls and an initial 281 ms frame. |
| Gears 3: `20261028-213440`, `20261028-204711` | Latest: 9,180 frames, 17 above 50 ms. Initial 1,893.85 ms frame attributed to game CPU. Later frame 4,072 took 106 ms with texture work, about 69 MB uploads and 41 skipped draws; frame 4,074 took 159.6 ms with 146.7 ms renderer GPU wait. Previous session also had a roughly 2.1-second initial pause and a later 203 ms game-CPU-classified frame with 57 new shaders and 166 skipped draws. | Several causes coexist: level/asset work, texture bursts, GPU waits and first-use pipeline work. More source-shader compilation alone cannot remove all of these stalls. |
| Judgment: `20261028-204334`, `20261028-193507` | Latest gameplay CSV: 20,760 frames, 16 above 50 ms. Initial game-CPU-classified pause about 3.05 seconds. Frame 84 took 502 ms, including 424.1 ms shader hold, with 580 skipped draws and 62 new shaders/92 pipelines. Later GPU waits also occurred. The preceding session primarily records shader preparation and has no gameplay frame CSV. | The first-use pipeline hold is a demonstrated stutter cause here. Other pauses are separate. The preparation-only session cannot be used as a second gameplay benchmark. |

Around Gears 2's worst stretch, the resolve caches held **775.8 MB unscaled plus 6,921.6 MB scaled**, approximately **7.7 GB** altogether. A log interval reports two memory-pressure events and 192 resources freed by the budget, totaling 3,450 MB. This is strong evidence of resource churn around the problem; it is not proof that every slow frame was an OS paging event. The sampled usage line was 11,131 MB against a 13,740 MB budget, rather than an explicit over-budget sample.

Microsoft documents that GPU memory budgets fluctuate and that exceeding them can cause paging and execution stalls. That makes reducing unnecessary resident resources a relevant fix for this trace. [Direct3D residency](https://learn.microsoft.com/en-us/windows/win32/direct3d12/residency), [DXGI memory budget information](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/ns-dxgi1_4-dxgi_query_video_memory_info).

## Changes made

**Reuse temporary resolve textures.** When UE3 streams assets and changes guest destination addresses, the renderer can reuse an older temporary GPU texture of exactly the same physical dimensions. It removes the old cache key before installing the new one. Current/recent resources are protected for four frames, and known presentation buffers are excluded. Written rectangles still copy into guest memory and the persistent scaled-memory windows in command order. This avoids repeated GPU allocations, destruction and large batches of budget eviction. Both scaled and unscaled resolve paths use it; rendering resolution is unchanged.

An initial 30 FPS scripted Gears 2 route check logged the following at frames 1,201–1,500:

| | Reuse disabled | Reuse enabled |
| --- | ---: | ---: |
| Resolve cache, scaled plus unscaled | 4,104.5 MB | 654.5 MB |
| Total reported video memory | 6,830 MB | 3,202 MB |

That is about **84% less resolve-cache memory** in those checks. They reached different views, so this is evidence of the resource reduction, not a controlled FPS comparison. This initial comparison preceded the additional presentation-buffer exclusion. A later run of the final version kept the resolve cache at **690.4 MB**, recorded 88 unscaled and 88 scaled reuses in one interval, and reported zero pressure events during that run. The exact worst area from the player's session has not been replayed.

**Restore cached stages on background workers sooner.** Gears 2's existing shader-construction notifications now request known cached stages without rebuilding guessed pipelines. The shared first-bind path also requests these stages before vertex/texture preparation, covering Gears 3 and Judgment. Requests are deduplicated and bounded to 256 queued speculative jobs; actual draw pipelines retain priority. Cache misses retain the normal fallback. Per-shader state avoids repeating the cache lookup on every draw.

Final checks recorded 392 stage prefetches/restores in Gears 2, 378 in Judgment's University check, and 367 in Gears 3's chapter-22 check. Their logged restoration times are accumulated worker time, **not measured wall-clock time saved**. The complete source inventory and existing caches remain valid; this change does not require another whole-game cold compilation.

This approach was informed by UnleashedRecomp's asset/mesh pipeline preparation and Microsoft's asynchronous pipeline-cache sample. COGBox's change is stage prefetching plus its existing live/recorded pipeline system; it does not reproduce UnleashedRecomp's full asset-driven pipeline enumeration. [UnleashedRecomp source](https://github.com/hedge-dev/UnleashedRecomp/blob/main/UnleashedRecomp/gpu/video.cpp), [Microsoft pipeline-cache sample](https://learn.microsoft.com/en-us/samples/microsoft/directx-graphics-samples/d3d12-pipeline-state-cache-sample-win32/).

**Improve stall attribution.** Export-readback polling, resolve-readback retirement and vertex rechecks now fall inside the renderer timer. Previously that work could appear as unexplained game CPU time. Logs also identify export-copy work and frame throttling. Capture frame labels now follow the game's swap counter, including skipped presentations, to help align input and images.

## Validation and FPS interpretation

- All five game configurations built. The test suite passed **136 cases / 817,987 assertions**, including new resource-reuse coverage for recent-frame protection, exact shape matching, presentation exclusions and avoiding two live cache keys pointing to one recycled texture. `git diff --check` passed.
- Short runs used isolated test user folders and the player's rendering settings, including 3x resolution. Normal user saves/settings were preserved. Only test-owned processes were closed.
- Visual captures verified a playable Gears 2 Rescue scene, Judgment University gameplay and Gears 3's chapter-22 opening scene. These are spot checks, not an exhaustive image comparison. The final stationary-camera attempt stayed at the pause menu and is excluded from gameplay validation.
- One Gears 2 validation attempt failed after its default audio device could not open, followed by a guest access violation. It is excluded. Later Gears 2 runs completed without that error. This failure was absent from the recent player logs used to diagnose their stutter.
- Capture tests intentionally used a **30 FPS cap** and GPU image readbacks. Those settings explain lower FPS in those tests and are unsuitable for comparison with normal uncapped play. The earlier Gears 2 report also mistakenly treated a title-screen test as a hospital-map test because it used the Gears 3 launch flag. That report has been corrected.

Separate uncapped checks disabled image capture. After the first 600 frames, the final Gears 2 Rescue run averaged 69.7 FPS with a 24.2 ms 99th-percentile frame time and no frames over 50 ms or skipped draws. Judgment University averaged 115.6 FPS, with one 66.6 ms game-CPU-classified frame and no skipped draws. Gears 3 Char averaged 109.2 FPS, but still had two pipeline-hold frames above 50 ms, a 198.7 ms maximum and 18 skipped draws. These are short, different scenes; **they do not establish a before/after FPS gain**. Even scripted movement did not reliably produce identical camera views across runs.

Longer 30 FPS visual checks also retained first-use skipped draws: Judgment University had 1,935 and Gears 3 chapter 22 had 1,194 after frame 600. The background stage change is working, but those counts prevent claiming that geometry pop-in has been eliminated.

## Remaining limits

Whole-game preparation covers the enumerated source shaders and stored variants. A graphics pipeline also depends on shader pairing, render-target formats and other render state. UE3 can introduce combinations during play that were not recorded beforehand; those still require live pipeline work. [Microsoft pipeline-state documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d12/managing-graphics-pipeline-state-in-direct3d-12).

The latest Gears 3 check demonstrates that the existing load-hold setting can still freeze a frame while its real draw pipelines finish. Removing that wait outright would exchange some freezes for missing draws. It was retained while the new prefetch reduces restoration work; it is not described as fixed.

UE3 streams levels as the player moves through streaming volumes. Epic notes that loading can take seconds and needs testing on the actual target. That is consistent with movement-triggered asset bursts, but does not identify a specific guest function in these logs. There is no evidence here sufficient to blame garbage collection or safely disable it. Texture streaming, uploads and GPU resolve/transfer costs remain possible contributors to pop-in and stalls. The guest texture pool and shader math were not changed speculatively. [Epic UE3 streaming volumes](https://docs.unrealengine.com/udk/Three/LevelStreamingVolumes.html).

Cached source preparation in the final uncapped checks took 0.9 seconds for Gears 2, 4.1 seconds for Gears 3 and 6.6 seconds for Judgment, with zero package failures. These measure the shader-preparation stage, not total launch time. A fresh complete driver-cache build remains expensive; the earlier Judgment completion took 12 minutes 45 seconds. This update improves cached restoration and streaming resource behavior, and does not claim a new reduction in that cold-build time.

Detailed recent-log summaries, per-run CSV analysis, build logs and test captures remain under the project's ignored `work/` directory for follow-up. The most useful remaining validation is a longer replay through the same Gears 2 area with the new resource-reuse counters and improved stall timings enabled.
