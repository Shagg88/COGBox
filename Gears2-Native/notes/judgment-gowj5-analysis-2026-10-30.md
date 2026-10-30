# Judgment: gowj5.tracy performance analysis

The clearest recurring hitch is a submission stall that propagates through the recording worker to the renderer. Most ordinary frames also have a substantial GPU cost. Shader compilation is not the main cause of either pattern in this capture.

## Capture and matching run

- Capture: `C:/Users/ctied/Documents/gowj5.tracy`, 8,193,383 bytes, saved October 30, 2026.
- Matching launcher session: `Gears2-Native/cogbox/build/Judgment/logs/20261030-000122`.
- Tracy 0.14.1 exports align with **7,838 native frames, 6,563–14,400**, covering approximately **88.3 seconds**. Every aligned renderer-time value matches the log within its rounding precision.
- Settings: **3x resolution, SMAA, strict occlusion, two renderer frames in flight, no frame cap, D3D12 debug off**. CPU texture-format constants are already enabled.
- The installed Judgment executable and runtime have the same SHA-256 hashes as the backups taken before the presentation experiment. **This run does not contain the proposed presentation change or the new native Present telemetry.**

The previous shared-runtime build has now completed successfully, but its output remains staged in the SDK build tree. No game was launched or installed during this analysis.

## Measurements

| Measurement | Result |
| --- | ---: |
| Average frame interval / FPS | 11.261 ms / 88.80 FPS |
| Median / 99th-percentile interval | 11.181 / 15.776 ms |
| Longest captured frame | 29.951 ms |
| Frames over 20 / 25 / 33.334 ms | 26 / 24 / 0 |
| Renderer elapsed time | 2.825 ms/frame |
| Renderer waiting for its recording worker | 0.404 ms/frame; maximum 15.666 ms |
| Measured GPU time | 11.096 ms/frame |
| GPU drawing / resolves | 7.773 / 2.142 ms/frame |
| GPU EDRAM transfers / texture loading | 0.632 / 0.175 ms/frame |
| Game waiting for query results | 7.960 ms/frame |
| New translated shaders / new pipelines | 1 / 0 |
| Shader-hold time | 0 throughout |

CPU and GPU work overlap; these columns must not be added together. GPU samples are collected asynchronously, so individual samples can arrive later than the CPU work that generated them. GPU timestamp intervals also include scheduling effects; they are not a direct measurement of shader-unit utilization.

## Where the repeated hitches happen

**25 of the 26 captured frames over 20 ms coincide with an individual `ExecuteCommandLists` call lasting more than 4 ms.** Across the capture there are 104 such calls, affecting 81 aligned frames.

The recurring sequence is:

1. The command-recording worker enters `ExecuteCommandLists`.
2. The renderer reaches the final submission inside `PresentOurs` and waits for that worker to finish.
3. The next frame arrives late.

For example, frame **12,326** takes **26.054 ms**. Its submission API calls total **15.543 ms**, its renderer waits **15.666 ms** for the worker, and the `present image` step takes **15.714 ms**. Draw counts are **1,392 → 1,384 → 1,392**, and uploads are **5.31 → 5.33 → 5.36 MB** across the surrounding three frames. This is a clear hitch without a corresponding burst of geometry or uploads. No new shader or pipeline is reported on that frame.

Frame **13,295**, the longest captured frame, takes **29.951 ms**, with **13.992 ms** waiting for the recorder and **13.781 ms** across the submission API calls. Close, reset and signal operations are generally much shorter than these calls.

The location is established; the underlying reason for the long API elapsed time is not. An elapsed CPU zone includes time spent blocked or descheduled. It cannot distinguish expensive driver processing, queue contention, presentation backpressure or Windows scheduling by itself. [PIX timing captures](https://learn.microsoft.com/en-us/windows/win32/direct3dtools/pix/articles/timing-captures/pix-timing-captures) can collect CPU samples, context-switch call stacks and GPU scheduling information to separate those cases.

The remaining >20 ms frame, **7,048**, is different: **24.743 ms** frame time, **21.012 ms** game-query wait, approximately **10.504 ms** measured GPU work and **13.609 ms** between timed lists, with only **0.072 ms** across renderer submission calls. That gap can contain SDK presentation work or scheduling delays on the shared queue. It is not proof that the physical GPU sat idle.

## Why average FPS remains limited

Drawing accounts for approximately **70%** of measured GPU time, and resolves about **19%**. The frame interval correlates much more strongly with measured GPU time than with the recording-worker wait. Optimizing CPU translation alone is unlikely to produce a large average-FPS improvement at these settings, although removing the submission stalls could improve consistency.

Texture reuse is already working: the final 300-frame log interval reports **8,002 reloads avoided** and **1,964 scaled-mirror updates**. Remaining texture loads average only **0.175 ms** in the capture. The older estimate of saving another approximately 1 ms by enabling resolve-through does not apply unchanged to this run.

There is no evidence here that the recurrent gameplay hitches are caused by full shader preparation, an exhausted upload/descriptor ring or VRAM-budget pressure. The end-of-run log reports about **3.56 GB used against a 15.28 GB budget**, no upload/descriptor ring waits and no shader-hold time. The bounded query-event waits remain approximately 2–3 ms; shortening their timeout again does not address the long submission calls.

## Changes worth pursuing, in order

1. **Test the already-built presentation candidate with its new Present measurements.** It makes the previously unconditional `DXGI_PRESENT_RESTART` optional, defaulting off, while retaining `--d3d12_present_restart=true` for comparison. Microsoft documents RESTART as discarding queued presentations; that makes this a reasonable experiment, not proof of a performance gain. Compare the same heavy scene with both settings and inspect native Present intervals as well as renderer frames. [DXGI flag documentation](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present).

2. **Separate submission from recording if the comparison confirms the stall remains on the critical path.** The current worker both records and executes lists. A bounded pool of independently owned command lists, with a submission worker, could allow recording to continue while execution calls block. NVIDIA specifically recommends keeping CPU-intensive recording separate from `ExecuteCommandLists`. This is a substantial renderer change: each closed list, allocator, descriptor/upload allocation and referenced resource needs a safe lifetime; query signals, guest-memory releases and presentation must retain their ordering. Merely moving the existing pointer onto another thread would be unsafe. It may hide a stall without eliminating its cause, so measure the result. [NVIDIA CPU guidance](https://developer.nvidia.com/blog/advanced-api-performance-cpus/).

3. **Optimize the expensive GPU drawing passes for average FPS.** Capture per-pass/shader costs in the affected area; the existing draw report at frame 3,000 precedes this trace. Then target the most expensive translated shaders and redundant rendering/state transitions. A wholesale switch from DXBC to HLSL is not established as an optimization by this capture. A controlled 2x/3x comparison can also help distinguish pixel-scaled work from fixed costs; it should be a diagnostic comparison rather than an automatic settings change.

4. **Validate selective fused scaled resolves.** Resolves still cost approximately 2.14 ms/frame, giving this path a meaningful target. The optional implementation and minimum-area guard already exist, but remain off. Restrict by supported layout/format and complete coverage, then compare output, exposure, shadows and overlapping writes in the same scene. Size alone does not prove equivalence. Its savings overlap with texture reuse and cannot be added to old estimates as independent gains.

5. **Batch only submissions whose dependencies permit it.** The capture has roughly two actual backend execution calls per renderer frame, and roughly three visits to the higher-level Submit function. It does not show twenty actual submissions per frame. Some Submit visits find no open list. Do not merge query submissions blindly: they deliberately make visibility results available earlier. Microsoft recommends batching executions where safe, but query/fence ordering remains necessary. [ExecuteCommandLists documentation](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-executecommandlists).

GPU-upload memory and coarse timestamps remain optional A/B experiments. Previous comparisons were approximately neutral, and this capture does not establish that either would fix these hitches. Broad async-compute or lazy guest-memory-copy changes require more dependency analysis than this data provides.

## Separate earlier upload burst in the same log

The full log contains earlier stalls that are outside the supplied Tracy interval. Frame **2,228** takes **83.479 ms**, uploads **65.80 MB**, processes **6,355 draws**, and spends **29.248 ms** in texture handling. The following frame takes **159.686 ms**, with a **158.860 ms** query wait and a very large GPU sample. This is a separate asset/upload burst; improving ordinary submission pacing will not necessarily remove it. Investigating resource creation/preparation and upload scheduling there requires a capture that includes that transition.

## Source locations and evidence

- Recording worker submission: `src/layer0_xenos/renderer/d3d12_backend.cpp:1011`.
- Final submission drain: `src/layer0_xenos/renderer/d3d12_backend.cpp:3019`.
- Presentation handoff: `src/layer0_xenos/renderer/shadow_renderer.cpp:3028`.
- Fused resolve option: `src/layer0_xenos/renderer/shadow_renderer.cpp:765`.
- Portable SDK candidate: `patches/sdk-patch45-present-queue.diff`.
- Exported events, aligned frames and summaries: repository-root `work/tracy-analysis-gowj5/` (`events-and-plots.csv`, `analysis.json`, `timeline.csv`, `summary.txt`, `focused-stats.txt`).

No new runtime optimization was installed during this analysis. The previous presentation candidate and its Tracy instrumentation remain unvalidated in-game. Differences between gowj4 and gowj5 are not an A/B result: the installed binaries did not change, and scene/workload coverage differs.
