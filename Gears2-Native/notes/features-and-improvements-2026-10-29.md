# COGBox — features and improvements from this work

**Gears of War 2, Gears of War 3 and Gears of War: Judgment**  
October 28–29, 2026. This summarizes the changes made in this chat, rather than every feature already present in COGBox. Shader preparation and the new automatic occlusion behavior target DirectX 12.

## Full shader preparation before launch

- Added whole-game preparation before gameplay starts, covering installed game files, title updates, DLC/user content and enabled mods.
- Included vertex and pixel shaders and both stored shader representations. Fixed compressed-package handling and nested/raw shader discovery so valid records are retained.
- Removed silent queue overflow that could discard preparation work. Identical output is deduplicated to avoid unnecessary compiler work.
- Kept live compilation for shaders patched/generated during play and previously unseen pipeline combinations.

**Control:** each game's **Graphics → Compile all shaders at launch** checkbox. It is on by default. Turning it off skips full preparation and preserves existing caches; first-use hitches or pop-in may increase.

## Visible compilation progress

- Added a responsive **Preparing game shaders** panel before the first game frame.
- Shows the current finding, scanning, compiling or saving stage, progress, completed/total work, percentage and elapsed time.
- Fixed panel attachment and notification races that could leave preparation running without a visible progress display.

## Faster repeated launches and lower compilation memory use

- Added validated package indexes and a source inventory so unchanged content reuses completed work.
- Parallelized startup preparation, deduplicated equivalent driver work and prioritized pipelines needed by live draws.
- Changed large compiled-stage storage to stream to disk and release temporary pipeline objects. Large legacy caches are mapped rather than copied wholesale into memory.
- Added recovery for corrupt, truncated, interrupted or driver-incompatible caches, with checksums and settings/code identity checks.

## Fewer avoidable stalls during gameplay

- Removed speculative package-compilation holds that had frozen frames for up to **120 seconds**, including a case with no draw pipelines pending.
- Fixed a missed-wakeup window in GPU/query CPU waits across all three games.
- Moved pipeline-record writes to a background writer.
- Added earlier background restoration of cached shader stages, with bounded queues and live-draw priority.
- Used a bounded asynchronous pipeline wait to reduce missing first-use draws while keeping required correctness waits.

## Lower GPU memory pressure while streaming

- Reuse matching temporary resolve textures as guest destination addresses change, instead of repeatedly allocating and destroying them.
- Protect recent/in-use resources and known presentation buffers.
- Added reuse and memory-pressure counters to verify that the fix is active.

## Lens flares and hidden-object occlusion

- Replaced fake-visible answers with each query's actual GPU visibility result. Walls can now hide light flares and objects behind them.
- Guarded query results against reused report addresses and kept unfinished draws from incorrectly hiding objects.
- Batched query submissions and returned results earlier, avoiding the initial performance penalty from waiting for the rest of a frame's GPU work.
- Gears 3: installed and checked in Gridlock Horde.
- Judgment: installed and checked in Baird's Testimony/Museum opening, the Academy street and an Aftermath interior. The opening movie was skipped only in isolated test settings.

## Better logs and verification

- Added clearer timing for presentation, submission, frame throttling, texture work, readbacks and occlusion results. Some previously unexplained game-CPU time is now attributed to the renderer work that caused it.
- Aligned captured frame labels with game swaps and scripted input.
- Reviewed recent logs for all three games and separated shader holds, streaming/texture bursts, renderer work, GPU waits and game-CPU pauses.
- Built all five game configurations across this work. The latest shared suite passed **138 tests / 818,000 assertions**; launcher setting checks also passed.

## Measured results on this PC

| Check | Result |
| --- | --- |
| Representative cached launches, total process-to-ready | Gears 2 **4.24 s**, Gears 3 **14.97 s**, Judgment **14.93 s** |
| Full Judgment preparation memory | Approximately **44+ GB** in the older process versus **2,913 MB peak private memory** in the new completion run |
| Gears 2 scripted streaming check, temporary resolve cache | **4,104.5 MB → 654.5 MB**, about **84% lower** in the initial checks; a later final-version run held about **690 MB** |
| Gears 3 Gridlock, final real occlusion versus occlusion off | About **19% fewer logged draw calls**, with essentially unchanged average rendering rate: **117.00 versus 116.92 FPS** |
| Installed game/update discovery validation | **6,528 packages**, zero compressed-chunk failures |

These are local measurements with different scopes. Cached launch times require prepared caches. The Gears 2 memory checks reached different views and do not establish an FPS gain. The Gridlock result is a short fixed-seed test; its final 99th-percentile frame time was slightly higher than the off comparison. None is a guarantee for every chapter or PC.

## Remaining limits

- A fresh driver-cache build can still take minutes. A recorded Judgment preparation completed in **12 minutes 45 seconds**; it was not a controlled, completely cold driver benchmark.
- New runtime shader/pipeline combinations, UE3 level loading, texture streaming and GPU stalls can still cause hitches or pop-in. All stuttering has not been eliminated.
- The Gears 3 instant-death trigger remains unproven at code level. It also occurs at 30 FPS. Restarting the entire chapter is the current workaround to try for the suspected checkpoint/encounter-state issue; the original save was left unchanged.
- Vulkan has not received the same validated automatic startup-preparation and occlusion changes.

The existing **CogBox.exe** launcher remains the entry point. Detailed evidence is retained in the accompanying shader, performance, stutter and occlusion reports, with test logs and executable backups under the project's `work/` directory.
