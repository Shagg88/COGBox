# Judgment performance and renderer optimization pass

October 29, 2026. This pass starts from `dc25b45`, which already includes the earlier occlusion/frame-pacing revision and texture batching. The changes are built into the normal Gears 2 base/TU6, Gears 3 base/TU6 and Judgment Release executables. All three games share the modified renderer.

**Result:** removed redundant CPU wake operations and improved DirectX diagnostics. Normal average FPS is essentially unchanged in the matched Judgment Museum check. Two GPU experiments are available but remain **off by default** because their results were mixed. Large streaming/GPU hitches and an isolated renderer-wait hitch remain unresolved.

## Latest player logs

Statistics exclude frames 1–600. Different player sessions follow different routes, so their average FPS cannot establish a debug-layer or code-change comparison.

| Game / session | Debug layer | Average FPS | 99th-percentile frame | Largest frame | Frames over 50 ms |
| --- | --- | ---: | ---: | ---: | ---: |
| Judgment / `20261029-134750` | Off | 76.54 | 33.96 ms | 157.58 ms | 14 |
| Judgment / `20261029-135110` | On | 84.82 | 33.89 ms | 127.18 ms | 12 |
| Gears 3 / `20261029-135324` | On | 86.05 | 26.29 ms | 96.81 ms | 20 |
| Gears 2 / `20261029-135906` | On | 107.98 | 20.30 ms | 78.04 ms | 17 |

Judgment's debug-off frame 1,298 spends **29.92 ms in texture work** and uploads **65.35 MB**; frame 1,299 then takes **157.58 ms**, with a 148.99 ms GPU sample. In the debug-on session, frame 912 uploads **65.27 MB** and takes 127.18 ms; the following frames have GPU samples of approximately 105 and 122 ms. Gears 2 shows a similar sequence: frame 1,431 spends 35.22 ms preparing textures and uploads 78.48 MB, followed by long GPU frames.

These examples have **zero shader-hold time and no new translated shaders**. Some preceding texture-burst frames create one pipeline, but mean pipeline waits are only about 0.07–0.09 ms/frame in these sessions. Launch shader preparation does not remove texture creation, guest-memory uploads or GPU rendering work.

Gears 3's largest frame is also accompanied by long GPU work, with no new shaders or pipelines. The logs establish where time was spent waiting; they do not identify a particular driver, residency operation or GPU instruction as the underlying cause.

In representative later Judgment intervals, measured render-thread CPU execution is approximately **2–3 ms/frame**, while GPU work is approximately **10–12 ms/frame** at the player's 3x setting. Other scenes cost more CPU time. The old `renderer_own_cpu_ms` field is elapsed time after selected waits are subtracted and can include recorder/presentation waits; it is not total CPU execution. Worker CPU time is separate. CPU, GPU and waiting measurements overlap and must not be added together.

## Enabled changes

- **Avoid redundant command-recorder wakeups.** The producer now notifies the recording worker only when that worker is sleeping. Sleeping state and queued commands use the same mutex and condition-variable predicate, preserving wake ordering. The worker still records commands in order.
- **Publish completed occlusion reports together.** Every ready, guarded query report is completed as before, followed by one guest-write notification for the ready batch. Museum intervals with approximately 225 reports per frame now publish approximately one completion notification per frame. Real sample counts, visibility decisions and report-reuse guards remain intact.
- **Expose useful counters.** Periodic logs now show recorder wakeups/avoided notifications, occlusion completion notifications and guest-memory UAV barriers.
- **Connect `d3d12_debug` to custom-renderer messages.** Enabling the SDK debug layer now also forwards the custom backend's warning/error messages. Previously this required the separate `gow2_renderer_debug` flag. Error severity and message IDs are preserved.

These reduce redundant operations; the measurements below do not establish a substantial average-FPS improvement from them.

## Matched Judgment measurements

RTX 4080 SUPER, player's 3x DirectX 12 settings, warm shared caches, fixed seed, fixed 30 Hz simulation, matching scripted camera turns. **Rendering is uncapped:** the game's benchmark mode bypasses the normal frame cap. Each complete comparison samples frames 601–3,000, excluding initial loading. No screenshots, compilation or second game ran during performance samples.

| Museum opening / Baird's Testimony | Average FPS | 99th-percentile frame | Mean frame-to-frame time change |
| --- | ---: | ---: | ---: |
| Before this pass, debug off | 93.14 | 15.80 ms | 0.83 ms |
| Before this pass, debug on | 83.73 | 17.39 ms | 0.74 ms |
| CPU synchronization candidate, debug off | 93.05 | 15.83 ms | 0.80 ms |
| Final normal executable, debug off | 93.39 | 15.16 ms | 0.53 ms |

The matched debug-layer comparison loses about **10% average FPS** and increases command-recorder waiting from approximately **0.62 to 2.63 ms/frame**. Leave `d3d12_debug` off for normal performance testing and playing; enable it when collecting validation messages. The player's saved debug setting was preserved.

The final normal build averages only 0.3% more FPS than the starting build, within ordinary run variation. Its lower frame-time variation is encouraging, but the earlier candidate repeat is almost unchanged. This is not evidence of a universal frame-pacing fix.

An Aftermath interior comparison was effectively unchanged: **115.50 versus 115.52 FPS**; its 99th-percentile frame time varied from 15.94 to 16.95 ms. The Academy test with both GPU experiments measured **97.61 versus 98.34 FPS**, but draw counts differed by about 3%, so the lower tail latency there cannot be attributed confidently to the experiments.

### Longer route and remaining hitch

After the matching Museum sample, the final run turns back and walks forward. Across 3,960 post-warmup frames it encounters one **222.84 ms** hitch at frame 3,601. That frame reports **219.60 ms of guest query waiting**, **218.51 ms of GPU-thread waiting for the renderer**, only **17.70 ms of sampled GPU work**, no new shaders/pipelines and no large upload burst.

This is a separate unresolved CPU/wait-side case. It follows a periodic diagnostic-summary boundary. The SDK logging path is synchronous and flushes information-level messages, so logging or scheduling is a possibility, not a demonstrated cause. A repeated route with much less renderer/PM4 summary logging completed **4,110 post-warmup frames**, with a **25.88 ms maximum** and no frames over 33.34 ms. Failure to reproduce one isolated hitch does not prove that logging caused it or that it is fixed. Default logging settings were retained.

## Optional GPU experiments — both off by default

These are native command-line flags, not new launcher checkboxes. Normal launches retain their existing upload and barrier behavior.

| Flag | Behavior | Local result / decision |
| --- | --- | --- |
| `--gow2_renderer_gpu_upload=true` | Use CPU-writable GPU-local memory for the 256 MB upload ring when `GPUUploadHeapSupported` is available. Creation/mapping failure falls back to normal upload memory. | Museum 93.73 versus 93.14 FPS: less than 1%, within variation. Uses additional local video memory; leave off by default. |
| `--gow2_renderer_guest_uav_defer=true` | Place guest-memory UAV ordering at the next dependent UAV access; resource-state transitions already order accesses when the state changes. | Removed approximately 39–48 immediate post-dispatch barriers/frame in tested Museum views, but average FPS was 91.78 in its isolated comparison. Leave off by default. |

The GPU upload implementation checks support and performs CPU writes only; CPU reads from this memory are deliberately avoided. This follows the [DirectX GPU upload specification](https://microsoft.github.io/DirectX-Specs/d3d/D3D12GPUUploadHeaps.html) and [AMD's implementation guidance](https://gpuopen.com/learn/using-d3d12-heap-type-gpu-upload/). UAV dependencies still require ordering; the experiment uses the guarantees described in [Microsoft's resource-barrier documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12).

A translated pipeline-state memoization prototype showed few useful hits and no measured CPU/FPS improvement, so it was removed. Disabling detailed GPU timing also failed to improve the Museum comparison, so that default was retained. Existing copy-worker defaults were preserved; simply increasing worker counts has previously cost performance in this renderer.

## Threading implications

The original developer's larger CPU-renderer project remains relevant. This pass improves synchronization around the existing recording worker. It does **not** distribute one frame's recording across several independent command lists, and does not add asynchronous compute/copy queues.

Parallel recording requires independent command-list/allocator state, bounded descriptor/upload allocation and ordered handling of resolves, occlusion and guest-memory releases. [Microsoft's multithreading sample](https://learn.microsoft.com/en-us/samples/microsoft/directx-graphics-samples/d3d12-multithreading-sample-win32/) explains the independent recording-state requirement. [NVIDIA's command-buffer guidance](https://developer.nvidia.com/blog/advanced-api-performance-command-buffers/) also cautions against excessive recording/submission overhead. GPU overlap would need genuinely independent work and explicit dependencies; this renderer's shared guest-memory/EDRAM operations cannot safely be overlapped merely by increasing a thread count.

The earlier [Flippage occlusion reference patch](https://github.com/Flippage/gears-of-war-3-recomp/blob/master/sdk/patches/0062-occlusion-queries.patch) remains a useful comparison for real query behavior. COGBox's custom renderer still needs its own submission and completion handling. No unrelated renderer was copied wholesale.

For the remaining large GPU frames and the isolated CPU/wait hitch, a [PIX timing capture](https://learn.microsoft.com/en-us/windows/win32/direct3dtools/pix/articles/timing-captures/pix-timing-captures) can correlate CPU scheduling, file I/O, residency and GPU execution. The existing CSV alone cannot distinguish those causes conclusively. No PIX trace was collected in this pass.

## Validation and evidence

- Built normal **Gears 2 Release / Release-tu6, Gears 3 Release / Release-tu6 and Judgment Release** successfully.
- Shared regression suite: **139 test cases / 818,002 assertions**, all passing. Final output: `work/judgment-performance/final-shared-tests.log`.
- Judgment exercised the Museum opening, Academy street and Aftermath interior. Separate control/candidate Museum image checks showed consistent wall, lighting and player rendering in the inspected views; NPC poses differed. Screenshots were excluded from FPS comparisons.
- Final Judgment debug run with both GPU experiments: no D3D12 warnings/errors, guest exceptions or device-loss messages. A benign GPU-upload initial-state warning found earlier was corrected before the final normal builds.
- Final Gears 3 TU6 Gridlock check used **only `d3d12_debug`** for validation-message forwarding, with both experiments enabled: no errors/device loss. It reported two warning-971 pipeline-library misses followed by working fallback. Its sampled check measured 117.00 FPS / 9.04 ms p99; this is a compatibility check, not a before/after gain.
- Final Gears 2 TU6 Hospital check with normal defaults: no errors/device loss, 116.99 FPS / 11.25 ms p99 in the sampled interval. It did not reproduce the player's transition freeze.
- Query reports in the reported strict-mode intervals showed no incomplete/refused queries, stale completions or dropped results. Short scripted checks cannot prove every encounter or camera position correct.
- Basic D3D12 debug-layer validation was used; **GPU-based validation was not used**. Other GPUs and Vulkan were not tested. Feature-unsupported GPU-upload fallback was reviewed but not physically tested on unsupported hardware.
- No shader translation/cache identity changed, so this pass does not require full shader recompilation. Player saves and launcher settings were preserved. Test-only user folders were used, and all test game processes were closed.
- Backups of all five pre-pass executables: `work/judgment-performance/backups/`.
- Arguments, logs, per-frame CSVs and results: `work/judgment-performance/`, plus `work/optimization-round2/judgment-pass-g3-validation/` and `work/framepacing-regression/judgment-pass-gears2/`. Image-validation and removed-prototype runs remain in that evidence directory; they are not shipped-FPS comparisons.

For the cumulative feature list from the whole chat, see [features-and-improvements-2026-10-29.md](features-and-improvements-2026-10-29.md).
