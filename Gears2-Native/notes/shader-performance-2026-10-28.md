# COGBox performance update — October 28, 2026

The installed game executables used by COGBox have been updated for Gears 2, Gears 3 and Judgment. All five configurations built successfully. The existing launcher remains the entry point: `Gears2-Native/cogbox/build/CogBox.exe`.

## Measured cached startup

| Game | Total process-to-ready time | Shader preparation | Unique source variants | Packages |
| --- | ---: | ---: | ---: | ---: |
| Gears 2 | 4.24 seconds | 1.1 seconds | 67,101 | 1,767 |
| Gears 3 | 14.97 seconds | 5.0 seconds | 208,806 | 2,512 |
| Judgment | 14.93 seconds | 4.2 seconds | 259,374 | 2,249 |

Both complete installed-game checks reported zero package, translation and driver failures. Every enumerated source variant was accounted for. These are cached launches on this machine, not cold-driver compilation estimates. The old Gears 3 log recorded a 1,150-second first build and a later 225-second preparation after loading its large native cache.

Judgment's full preparation completed in **12 minutes 45 seconds** (768.49 seconds total process-to-ready), with **2,912.9 MB peak private memory**, zero translation/driver/package/save failures and a responsive window. This confirms the memory reduction, but a fresh complete driver build still takes substantial time. Its new stage store reported:

`2249 packages, 2249 cached indexes, 1450659 shader records, 0 package failures, 765.4 s; runtime-created variants retain live fallback`

`259374 unique shaders, 259374 translated/queued, 123249 distinct driver pipelines (985 cache hits), 0 translation failures, 0 driver failures; live fallback enabled`

`123249 pipelines, 0 save failures`

Its subsequent cached check reported:

`2249 packages, 2249 cached indexes, 1450659 shader records, 0 package failures, 4.2 s; runtime-created variants retain live fallback`

The full Judgment run reused the machine's existing driver cache and an interrupted short test's stage records. It therefore measures completing the new on-disk cache; it is not a controlled, entirely cold GPU-driver benchmark.

## Causes addressed

- The old Judgment process exceeded 44 GB of committed private memory on a 32 GB machine. This is consistent with severe paging pressure. The new whole-game stage cache streams compiled blobs to disk and releases their pipeline objects, rather than retaining the complete collection in a native pipeline library. The new full run peaked at about 2.85 GiB private memory (2,912.9 MB).
- Cached startup formerly recreated every warm pipeline. A checksummed source inventory now avoids that repeated translation and driver work. It is tied to translation settings/code identity, the driver-validated library, and stage-store metadata. Missing or incompatible metadata falls back to preparation.
- Every launch still enumerates the game, update, user DLC and enabled mod roots. Unchanged packages reuse checksummed indexes using file ID, volume, length, write time and change time. Changed/replaced files are reread and hashed. Gears 2 reused all 1,767 packages; Gears 3 reused all 2,512.
- Large legacy native caches are mapped instead of copied byte by byte. Their existing completed work is preserved. The one-time Gears 3 inventory upgrade took 532.4 seconds, including memory pressure while restoring the old cache; that upgrade has already completed locally. Its following launch is the 15-second result above.
- Four startup translation workers replace the single worker. Driver workers use the startup pool, with a condition-variable capacity notification replacing fixed polling delays. Gameplay retains reduced-priority speculative work.
- Rendering no longer flushes pipeline-list records to disk synchronously. A background writer preserves the records.
- All three games' CPU wait loops now use the write sequence sampled before the preceding guest completion check. This removes a lost-wakeup window that could turn a completed GPU/query operation into an unnecessary timer wait.
- Live pipelines restore only the cached stages they need. Concurrent requests for the same stage wait for the first restore off the rendering thread. A bounded 3 ms per-frame asynchronous pipeline wait reduces skipped draws without compiling pipelines on the rendering thread. Export draws retain their required correctness waits.

The stage-store mechanism uses the cached pipeline blobs supported by [Direct3D 12](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_cached_pipeline_state). Blob checksums are checked when read; interrupted final records are discarded while complete records remain reusable. Driver incompatibility starts a new store and retains the old file as an incompatible-cache backup.

## Gameplay checks and limits

A 60-second Gears 3 diagnostic launched `SP_Nightmare_P`. After its first 300 frames, 5,100 logged frames had a 12.644 ms 99th percentile, a 28.426 ms maximum, no frames over 50 ms and no skipped draws. A 1.522-second initial level-loading pause remained, attributed by the frame log to game CPU work. This is a short map/cinematic check, not proof that every campaign encounter is stutter-free.

Correction from the subsequent stutter investigation: the Gears 2 diagnostic used ue3_launch, but Gears 2 requires gow2_launch. Its numbers below describe a title-screen check, not the requested hospital map. Do not treat them as campaign gameplay validation.

The earlier Gears 2 diagnostic attempted to launch `SP_Hospital_P` with normal whole-game startup enabled. After frame 1,200 it recorded 4,170 frames: 99th percentile 9.184 ms, maximum 9.409 ms, 0 frames over 50 ms, and 0 skipped draws. Initial level streaming and GPU waits are separate from shader preparation. The earlier startup-disabled diagnostic had CPU and texture-loading spikes while background package scanning was allowed, so it is not a normal-startup comparison.

Frame diagnostics now name slow presentation, submission, next-frame, texture-finalization, eviction and occlusion-result steps. Remaining engine asset loading, texture streaming, GPU scheduling or display stalls must be assessed from their own evidence. No claim is made that all CPU stalls or all blurry-to-sharp texture transitions are eliminated. The game's memory-limited texture pool was not raised further without evidence that it is safe.

Stored vertex/pixel stages and both package representations remain included. Bind-time patched shaders, generated variants and previously unseen pipeline combinations retain live compilation. Arbitrary future runtime combinations cannot all be enumerated from package data. The preparation changes target Direct3D 12 DXBC.

## Validation

- Shared tests: **132 cases, 816,491 assertions passed**.
- Cold stage-store and cached startup samples passed for all three titles with zero source/driver failures and responsive windows.
- Tests cover large file mappings, concurrent/drained metadata appends, option/cache invalidation, corrupt or truncated source inventories, stage-cache restart/recovery, corrupt blob rejection, concurrent stage saves, driver incompatibility, and changed/replaced package identities.
- All five game configurations built. Existing staged work was preserved and no commit was created.

The first preparation after new content or a driver/cache change can still take minutes. Cached launch measurements should not be read as a promise that a completely fresh GPU-driver build will finish in seconds. Keep “Compile all shaders at launch” enabled in COGBox; the progress panel remains active.
