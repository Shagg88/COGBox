# COGBox frame-pacing regression investigation

October 29, 2026. The earlier real-occlusion implementation did introduce avoidable overhead, most clearly in Judgment. Its original Gridlock check was too narrow to establish performance neutrality across both games. The revised implementation is built into the normal COGBox game executables, with real occlusion still enabled for Gears 3 and Judgment.

## What the latest player logs show

These are the latest player sessions, before this revision. Statistics exclude the first 600 frames. Routes and durations differ, so these are diagnostic observations rather than matched performance comparisons.

| Game / log session | Average frame time | 99th percentile | Largest frame | Main finding |
| --- | ---: | ---: | ---: | --- |
| Gears 2 / `20261029-114138` | 9.23 ms | 33.79 ms | 119.91 ms | Texture preparation bursts followed by GPU stalls; real occlusion is off |
| Gears 3 / `20261029-113834` | 11.59 ms | 33.73 ms | 94.64 ms | 8.58 ms/frame average guest query waiting; the largest spike coincides with a long GPU frame |
| Judgment / `20261029-113302` | 12.40 ms | 25.45 ms | 187.63 ms | 9.50 ms/frame average guest query waiting, plus a large texture/load transition |

The Gears 2 spike is particularly informative. Frame 8,321 uploads **98.55 MB**, spends **50.49 ms in texture work**, and takes 98.01 ms overall. Frame 8,323 then takes **119.91 ms**, including **110.63 ms waiting for the GPU**; the completed GPU work is timed at 126.34 ms. More GPU waits follow for several frames. A similar upload-then-GPU-stall pattern exists in the preceding player log, so this was not introduced by enabling real occlusion in Gears 3/Judgment. Gears 2's query mode remains off. Nearby memory statistics show about 3.2 GB used against a 15.3 GB local-memory budget, with no reported memory-pressure eviction. These logs do not establish a driver/residency cause for the long GPU work.

Gears 3 frame 2,224 takes 94.64 ms, with 94.04 ms of guest query waiting and a 97.84 ms GPU sample. Judgment frame 1,188 uploads 60.88 MB and spends 26.80 ms in texture preparation; the following 187.63 ms frame coincides with 191.78 ms of GPU work. Query waiting exposes that GPU delay to the guest. Removing polling overhead cannot remove the underlying long GPU frame.

The three largest frames above create **no new shaders or pipelines** and have **zero shader-hold time**. Mean pipeline waits in these sessions are approximately 0.04–0.10 ms/frame. These freezes are not evidence that launch shader preparation missed another large shader batch. Texture loading, guest-memory uploads, rendering and synchronization remain runtime work even with a warm shader cache.

Wait measurements overlap work on other threads and the GPU. They must not be added together as separate frame costs.

## Changes made

1. **Batch GPU result copies.** End each host query where the game ends it, but defer `ResolveQueryData` until command-list submission. Consecutive ended slots are resolved together. Gaps and heap wrap are kept separate; unfinished slots are excluded. This avoids interleaving a separate result-copy command between every pair of proxy draws.
2. **Submit small completed query batches early too.** The previous threshold required at least 16 completed queries before giving them an earlier fence. Some scenes had fewer queries and still waited for subsequent frame work. Completed batches can now submit before the next draw outside a query, retaining the existing limit of four such submissions per frame.
3. **Wake on GPU completion.** On Windows, the idle renderer waits for either queued rendering commands or the oldest pending query's actual fence event. Normal completion no longer depends on the 1 ms idle polling interval. Event-unavailable and query/device-failure recovery paths remain bounded. This does not add a separate CPU/GPU wait for every query.
4. **Measure CPU work and waits separately.** New periodic log lines report individual frame steps, time waiting for the command-recording worker, and OS CPU time for the render thread. The older “own CPU”/CPU-split fields measure elapsed time after selected waits are subtracted; they can still include recorder/presentation waits.

Actual full sample counts, resolution normalization, report-reuse protection, incomplete-draw visibility fallback and shader preparation are retained. No shader translation or cache identity changed, so this revision does not require a fresh full shader compilation.

## Matched uncapped comparisons

Tests used the player's 3x DirectX 12 settings on the RTX 4080 SUPER, warm shared caches, a fixed seed, fixed 30 Hz simulation and matching scripted camera turns. **Rendering was uncapped.** No image capture or build ran during the performance samples. Each row below samples frames 601–3,000: 2,400 frames, excluding initial loading.

### Judgment: Baird's Testimony / Museum opening

| Version | Average FPS | 99th-percentile frame time | Mean frame-to-frame time change |
| --- | ---: | ---: | ---: |
| Previous real occlusion | 77.95 | 18.267 ms | 2.662 ms |
| Occlusion disabled, comparison only | 94.29 | 14.581 ms | 0.194 ms |
| Batched result copies alone | 82.31 | 16.915 ms | 2.737 ms |
| Revised real occlusion, staged build | 92.91 | 15.504 ms | 0.710 ms |
| Revised real occlusion, installed normal build | **92.63** | **15.577 ms** | **0.698 ms** |

The installed repeat improves average FPS by about **19%** over the previous real-query build and substantially reduces frame-time variation. It retains some overhead relative to disabling occlusion. Disabling occlusion changes visibility and approximately doubles the logged draw count in this scene; it is a diagnostic comparison, not the shipped default. Query-on draw counts across the previous and final tests differ by about 1%, so these are matching scripted scenes rather than identical GPU command streams.

The long opening movie was skipped through `bForceNoMovies` only in isolated test folders. Normal movie settings and player saves were not changed.

### Gears 3: Gridlock Horde with camera turns

| Version | Average FPS | 99th-percentile frame time | Mean frame-to-frame time change |
| --- | ---: | ---: | ---: |
| Previous real occlusion | 115.92 | 10.879 ms | 0.703 ms |
| Occlusion disabled, comparison only | 117.79 | 8.855 ms | 0.121 ms |
| Revised real occlusion | **117.00** | **8.700 ms** | **0.061 ms** |

Average FPS changes little here; the meaningful improvement is more consistent frame times. Previous and revised query-on runs average 671.32 and 670.77 logged draws per frame. Both revised scene checks retain real zero/visible results and show no incomplete/refused queries, stale completions or dropped results in their reported intervals. These counters are not independent proof of perfect occlusion in every camera position.

### Gears 2 CPU and compatibility check

The revised TU6 build completed the scripted Hospital check at 116.58 FPS with an 11.733 ms 99th-percentile frame time and no sampled frames above 33.34 ms. This is a compatibility/diagnostic check, not a before/after optimization claim. It did not reproduce the player's large transition freeze.

In the final measured intervals the render thread used approximately **4.0 ms of OS CPU time per frame**, while waiting approximately **3.7–3.8 ms for the command recorder** and **1.0 ms at frame throttling**. Gridlock examples used approximately 1–2 ms of render-thread CPU time plus 2–4 ms of recorder waiting. Different scenes have different CPU costs. Worker CPU time is not included in the render-thread CPU figure.

The developer's point about renderer CPU work and threading remains relevant. PM4 parsing and command recording already have separate threads; substantial preparation still runs serially. The new figures distinguish that work from waits, rather than treating every elapsed millisecond as CPU execution. This pass does not implement a broad renderer threading rewrite.

## Validation and remaining work

- Normal executables rebuilt successfully: Gears 2 base/TU6, Gears 3 base/TU6 and Judgment Release.
- Shared suite: **139 cases, 818,002 assertions**, all passing, including contiguous query ranges, gaps/wrap and the existing report-reuse/zero-result checks.
- Live checks used Gears 2 TU6, Gears 3 TU6 and Judgment Release with the existing installed content. No checked run reported a guest exception, device loss or error-level log line.
- Executable backups: `work/framepacing-regression/backups/`. Tests use isolated user folders. Test processes are closed.
- Per-frame CSVs, arguments and summary JSON remain in `work/framepacing-regression/`, `work/optimization-round2/pacing-gridlock-*` and `work/judgment-occlusion/pacing-museum-*`.

**This resolves measured avoidable occlusion overhead, not all stuttering.** Gears 2's texture/GPU transition stall and the long GPU frames seen in Gears 3/Judgment remain separate optimization targets. Short scripted comparisons cannot establish performance across every campaign encounter. Real occlusion remains enabled automatically in Gears 3/Judgment; Gears 2's automatic query setting remains off.

## Reference implementation and API checks

[Flippage's actual occlusion patch](https://github.com/Flippage/gears-of-war-3-recomp/blob/master/sdk/patches/0062-occlusion-queries.patch) also defers guest answers until host GPU results complete and flushes outstanding work before waiting. COGBox's custom renderer needs its own submission and wake handling; copying that patch alone would not address these integration costs. [Microsoft's DirectX 12 query documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d12/queries) describes explicit result resolution and query boundaries. Full counts are retained here because a binary visible/hidden replacement would change sample-count semantics.
