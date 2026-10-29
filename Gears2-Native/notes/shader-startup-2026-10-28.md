# Startup shader preparation and freeze investigation — 2026-10-28

The Direct3D 12 shader prebuild now scans the installed game before its main thread runs. The startup wait holds the
guest main thread, leaving the native UI and progress display responsive. All three games share this implementation.
The launcher option is **Compile all shaders at launch**, enabled by default for supported builds.

## Cause found in the existing logs

The previous compile step waited on the render thread for speculative package shaders, even when the current frame
needed no pending pipelines. Its 120,000 ms budget explains the largest freezes directly:

| Existing session | Largest frame interval | Time inside the shader hold |
| --- | ---: | ---: |
| Gears 2, `20261028-174236` | 54.586 s | 54.536 s |
| Gears 3, `20261028-175326` | 122.540 s | 120.000 s |
| Judgment, `20261028-174616` | 123.296 s | 120.000 s |

The Gears 3 log explicitly reports a 120-second hold with **0 of 2,003 draws pending and 0 pipelines**, waiting for
6,272 package shaders instead. It subsequently records additional holds of 18.6, 9.5 and 58.3 seconds. Gears 2 and
Judgment also repeatedly held frames for package work. Across the three frame logs, 22 of 23 frame intervals over
250 ms were classified as shader holds.

`DecideLoadHold` and `HoldForPipelines` now wait only for pipelines demanded by that frame. The old speculative hold
flags remain recognized for compatibility but no longer block gameplay. This removes the cause identified above;
it does not establish that every possible freeze or first-use pipeline hitch is eliminated.

## Coverage and speed changes

- Recursively scan the actual runtime game, update and user-content roots, plus every enabled mod root. Keep the
  file-open observer for additional packages encountered later. Deduplicate paths by their full canonical path,
  preserving identically named packages in different DLC/mod folders.
- Retain both vertex and pixel records and both stored whole-region/code-only variants. Previous draw frequencies
  never suppress variants. Queue backpressure replaces the previous silent overflow eviction.
- Share the project's XMemCompress LZX decoder across all three games. It supports odd stored blocks without a
  padding byte, padded stored blocks, and streams longer than their history window. Scan decompressed chunks,
  nested shader streams, and raw regions outside compressed chunks. Distinguish ordinary uncompressed UE3 package
  headers from the compressed format, which shares the same tag.
- Cache extracted package records by the hash of the entire input file, parser version and a payload checksum.
  Invalid indexes are re-scanned; packages with unreadable compressed chunks are reported and never cached as
  successful scans.
- Create a compatible warm pipeline template before any guest draw. Use the pipeline worker pool during startup;
  afterwards live draw requests keep priority. Track in-flight work until translation and driver jobs finish.
- Deduplicate identical final bytecode/state independently of the source microcode hash. Preserve all source
  variants while avoiding equivalent driver builds.
- Persist warm driver pipelines in `pipelines-d3d12.bin.prebuild`, separately from the ordinary live pipeline
  library. Save the large startup library before guest execution. Driver/GPU changes or corrupt libraries cause a
  rebuild. Keep backing blobs alive for the lifetime of each D3D12 library.
- Synchronize loads of the same pipeline key with striped locks; unrelated pipeline loads remain parallel.
  This follows Microsoft's documented [pipeline-library thread-safety requirement](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device1-createpipelinelibrary).

## Validation performed

- Built Gears 2 Release and Release-TU6, Gears 3 Release and Release-TU6, Judgment Release, and the launcher.
- Shared test suite: **112 test cases, 816,386 assertions passed**. Nine new parser/compression tests cover both
  stages and variants, nested streams, mixed raw/compressed regions, malformed containers, stored-block padding,
  streams larger than the history window, and the shared UE3 tag.
- Launcher self-checks passed, including startup/off command-line behavior for all three games.
- Full installed-game/update scan: **6,528 packages**, **3,290,598 pixel** and **323,653 vertex** records, **0 compressed
  chunk failures**, approximately 173 seconds in the independent validation tool. These counts include duplicates
  between packages and stored variants; they are not unique shader or driver-pipeline counts.
- Runtime startup samples: Gears 2 **178** source variants / **114** distinct driver pipelines; Gears 3 **392 / 221**;
  Judgment **544 / 300**. Every source variant was translated/queued, with zero translation or driver failures.
  Each repeated run loaded all its distinct pipelines from the persistent cache and kept its UI responsive.
- Corrupt package-index and driver-library tests rebuilt successfully without losing any sample records.
- A DLC/mod fixture with three different full paths sharing the same basename scanned all three packages and all
  534 records, then correctly deduplicated them to 178 source variants.
- A 25-second Gears 2 live diagnostic deliberately disabled the startup barrier while leaving the old speculative
  hold flag enabled. Over 660 logged frames, the longest interval was **291.816 ms**, including a **222.648 ms** wait
  for pipelines actually needed by that frame. No speculative package hold occurred. This was a short boot/menu
  check, not a campaign playthrough or a controlled before/after performance benchmark.
- Whitespace/diff checks passed. Existing staged work was preserved; no commit was created.

## Limits and use

Restart COGBox to see the updated option. The installed launcher executable is replaced, with its previous binary
kept as `CogBox.before-shader-startup-20261028.exe` beside it. Current launcher settings already enable preparation.

The first complete preparation may take several minutes and use substantial CPU and cache space. Later launches
reuse package indexes and driver pipelines. The runtime tests above used representative global shader caches;
the discovery scan covered every installed package, but this session did not compile the full installed-game
corpus through the driver or play every level.

Xbox vertex shaders may be patched at bind time, and the game can generate new variants and pipeline combinations
during execution. Those cannot all be enumerated from stored package data in advance. Recorded shaders/pipelines
continue warming at startup and the normal live fallback remains enabled. Preparation applies to the Direct3D 12
DXBC path; the Vulkan path remains unchanged.

## Shader progress display update

The shader note was removed from ImGui's active dialog list in its constructor while its `attached_` flag remained
true. Subsequent worker notifications therefore did not reattach it. The constructor now records the hidden state,
creation replays any already-active preparation, and idle detachment rechecks for racing notifications.

Startup now displays an opaque centered panel before the first guest frame: **Preparing game shaders**, the current
finding/scanning/compiling/saving stage, a progress bar, completed/total counts, percentage and elapsed time. Discovery
uses an animated indeterminate bar until the package total is known. Scanning percentages refer to packages;
compilation percentages refer to shader preparation work, including pending recorded pipelines. Cache serialization
has a separate completed progress phase. The FPS bar stays hidden during startup and ordinary shader notes still
detach when idle, preserving normal frame-counting behavior.

Validated visually in the native game window as the count advanced from **259 / 4,234 (6%)** to **4,214 / 4,234 (99%)**.
That representative package completed all 4,234 source variants / 2,023 distinct driver pipelines with zero
translation or driver failures, in 75.2 seconds using a deliberately limited two-worker test pool. The isolated
preview was then closed. Cached startup checks passed for all three games with responsive UI windows.

Five new progress tests cover unknown totals, scanning while shader counts grow, handoff counter clamping, a real
100% saving phase and large counters. The full shared suite now passes **117 tests / 816,404 assertions**. All five
game configurations were rebuilt. The user's already-running Gears 3 compilation was left running; its old executable
was retained as `gow3.before-shader-progress-20261028.exe` while the normal launch path received the updated build.
