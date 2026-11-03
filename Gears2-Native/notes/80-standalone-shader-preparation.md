# Standalone shader preparation

The launcher has a per-game **Settings > Graphics > Shader preparation** choice:

- **Quick - recorded pipelines** reads the installed game and title-update packages and prepares the shader/layout/pipeline combinations in that game's recipe pack.
- **Deep - captured pipelines** prepares complete pipeline descriptions from shipped recipes plus gameplay captures. The existing game's cache is used automatically; **Use gameplay capture...** can add another cache. The optional **Also translate every stored shader variant** checkbox adds the full recognized vertex/pixel inventory, retaining both pixel microcode forms; it is off by default because it has no established gameplay benefit. Deep is experimental and currently requires DirectX 12; Quick remains available for Vulkan. The variant counter describes translation, not a claim that every possible driver pipeline is compiled.

The developer's repeated-play measurements did not establish a gameplay gain from the previous Deep implementation. Accordingly arbitrary stage pairings are no longer driver-warmed by default. The old behavior is available for comparisons with `--gow2_renderer_prepare_warm_variants=true`. Exact pipelines contain shader pairings, vertex layouts, render-target/depth formats, sample count, topology and blend/depth/raster state; a standalone stage cannot substitute for this description.

`--gow2_renderer_prepare_profile=<gameplay shadercache>` reads its `pipelines.list` and `ucode` data. Captured shaders are retranslated with the current renderer settings and persisted into the destination cache. Captured records are deduplicated against the shipped recipes and contribute to the same verified pipeline coverage totals. The external source is read without changing it; compiled driver binaries are not imported. A malformed/incompatible pipeline header is rejected. Selecting the destination itself uses its normal recorded warm-up instead of reimporting it.

A fresh restart retains raw capture inputs and pipeline descriptions from the previous cache backup when no external capture is selected. Compiled binaries are rebuilt in the fresh destination. This preserves learned coverage across a reset; the driver's own global caches may still be reused. Captures cover only previously encountered combinations and do not establish coverage of unseen scenes or settings. Pipeline-layout version checks reject incompatible records.

Local Gears 2 validation on 0.4.3: the full input set contains 1,767 packages. Without a capture, 12,201 of 12,372 recipe pipelines were ready and 171 unavailable. The supplied local gameplay capture added 82 complete records beyond the recipe pack and supplied shaders for additional existing records: 12,414 of 12,454 pipelines were ready, with 40 unavailable and zero failed builds. With that same capture and pipeline coverage, default preparation took 13.91/14.08 seconds, while the optional full 67,101-variant inventory took 47.73 seconds. Each used a fresh destination COGBox cache, without clearing global driver caches. These are preparation/coverage results, not evidence of fewer gameplay stutters. No game-derived capture data belongs in the source patch.

**Prepare shaders now** opens a dedicated progress screen. The hidden `CogBoxShaderCompiler.exe` process owns the device, package readers, translation coordinator and pipeline workers. It creates no ReXApp, guest runtime, game window, presenter or game render loop. The launcher reads small progress snapshots four times per second.

With Deep selected, **Restart Deep from scratch** starts a new pass with a fresh per-game COGBox cache. The same action is available on the completed/cancelled Deep progress screen. Stop an active pass first. The worker holds the cache lock while moving the previous cache into the sibling `shadercache-backups/restart-<session>` folder. The lock and current session's cancellation channel remain in place. Unrecognized cache contents are rejected before moving files, and a failed move rolls back earlier moves. Graphics-driver-managed caches may still be reused; this action does not force a completely cold driver compilation.

## Coverage

The scan covers `.xxx`, `.upk`, `.udk`, `.u` and shader-cache `.bin` files in the game, update and enabled mod folders. Paths are deduplicated by their full normalized identity so a same-named package in the update folder is also read. Unique cooked material shader permutations are included when their containers are recognized by the reader. Recipe vertex programs are rebuilt with their recorded layout words; recipe tessellation variants use their recorded segment counts.

An exhaustive pipeline set cannot be inferred from shader blobs alone. Exact pipelines are prepared where recipe or captured data exists. Unrecorded runtime combinations can still compile on first use. Unrecognized package formats or content inside unextracted DLC containers are not an exhaustive source of shader coverage. Captured data is game-derived and stays local; a public source patch must not include the player's captures.

Raw speculative variants use `ShaderCache::GetForPreparation` and do not enter the game's normal recorded-microcode startup list. Recipe demands and actual captured gameplay shaders are recorded. The recording writer is drained before a successful or cancelled compiler exit.

## Progress and cancellation

The versioned protocol is defined in `preparation_progress.h`. Each launch has its own progress/cancel filenames, with numeric fields for packages, variants, exact pipelines and failures. Snapshots are replaced atomically; partial or invalid reads preserve the last good display. A final summary is also written to `prepare-progress.txt` for diagnostics.

Completion can be `done`, `partial`, `cancelled` or `failed`. Missing recipes/shaders and unsuccessful driver warm-ups are reported as gaps. A partial result does not set the successful pipeline count to the recipe total. Stop requests halt new work, let submitted driver jobs finish, drain recording writes and save the pipeline library. A Force stop button becomes available after ten seconds if stopping does not finish.

An exclusive `prepare.lock` prevents concurrent compiler instances from writing one cache. The launcher also holds this lock during game sessions it manages, and excludes Play while its preparation worker runs. Directly starting a port executable outside the launcher does not participate in this launcher-level lock.

## Build and packaging

Build the Windows CMake target `CogBoxShaderCompiler` from Gears2-Native. It links the common engine/renderer and patched SDK; no recompiled game source is required. Place it beside `CogBox.exe` with `rexruntime.dll`, `dxil.dll`, the normal shared DXC/upscaler files, and the matching `D3D12/D3D12Core.dll`. The packaging script checks and copies the worker and its runtime dependencies.

The local installation provides `work/build-shader-feature.ps1` to build the compiler, run core tests, install its runtime files, and rebuild the launcher.
