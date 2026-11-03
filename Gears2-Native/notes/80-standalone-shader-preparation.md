# Standalone shader preparation

The launcher has a per-game **Settings > Graphics > Shader preparation** choice:

- **Quick - recorded pipelines** reads the installed game and title-update packages and prepares the shader/layout/pipeline combinations in that game's recipe pack.
- **Deep - game files and variants** also processes every unique vertex and pixel shader variant the package reader discovers, retaining both pixel microcode forms. On DirectX 12 these stages are put through the driver in temporary pipelines. The exact recorded pipelines are saved in the normal pipeline library. Deep preparation currently requires DirectX 12; Quick remains available for Vulkan.

**Prepare shaders now** opens a dedicated progress screen. The hidden `CogBoxShaderCompiler.exe` process owns the device, package readers, translation coordinator and pipeline workers. It creates no ReXApp, guest runtime, game window, presenter or game render loop. The launcher reads small progress snapshots four times per second.

With Deep selected, **Restart Deep from scratch** starts a new pass with a fresh per-game COGBox cache. The same action is available on the completed/cancelled Deep progress screen. Stop an active pass first. The worker holds the cache lock while moving the previous cache into the sibling `shadercache-backups/restart-<session>` folder. The lock and current session's cancellation channel remain in place. Unrecognized cache contents are rejected before moving files, and a failed move rolls back earlier moves. Graphics-driver-managed caches may still be reused; this action does not force a completely cold driver compilation.

## Coverage

The scan covers `.xxx`, `.upk`, `.udk`, `.u` and shader-cache `.bin` files in the game, update and enabled mod folders. Paths are deduplicated by their full normalized identity so a same-named package in the update folder is also read. Unique cooked material shader permutations are included when their containers are recognized by the reader. Recipe vertex programs are rebuilt with their recorded layout words; recipe tessellation variants use their recorded segment counts.

An exhaustive pipeline set cannot be inferred from shader blobs alone. Pipeline creation also needs vertex layouts, render-target/depth formats, sample counts, topology, blend/depth/raster state and shader pairings. Exact pipelines are prepared where recipe data exists. Temporary stage warming may help other combinations through the driver's shader cache, but reuse depends on the driver. Unrecorded runtime combinations can still compile on first use. Unrecognized package formats or content inside unextracted DLC containers are not an exhaustive source of shader coverage.

Raw speculative variants use `ShaderCache::GetForPreparation` and do not enter the game's normal recorded-microcode startup list. Only actual recipe demands are recorded. This avoids making every subsequent game start eagerly warm the entire deep-scan inventory. The recording writer is drained before a successful or cancelled compiler exit.

## Progress and cancellation

The versioned protocol is defined in `preparation_progress.h`. Each launch has its own progress/cancel filenames, with numeric fields for packages, variants, exact pipelines and failures. Snapshots are replaced atomically; partial or invalid reads preserve the last good display. A final summary is also written to `prepare-progress.txt` for diagnostics.

Completion can be `done`, `partial`, `cancelled` or `failed`. Missing recipes/shaders and unsuccessful driver warm-ups are reported as gaps. A partial result does not set the successful pipeline count to the recipe total. Stop requests halt new work, let submitted driver jobs finish, drain recording writes and save the pipeline library. A Force stop button becomes available after ten seconds if stopping does not finish.

An exclusive `prepare.lock` prevents concurrent compiler instances from writing one cache. The launcher also holds this lock during game sessions it manages, and excludes Play while its preparation worker runs. Directly starting a port executable outside the launcher does not participate in this launcher-level lock.

## Build and packaging

Build the Windows CMake target `CogBoxShaderCompiler` from Gears2-Native. It links the common engine/renderer and patched SDK; no recompiled game source is required. Place it beside `CogBox.exe` with `rexruntime.dll`, `dxil.dll`, the normal shared DXC/upscaler files, and the matching `D3D12/D3D12Core.dll`. The packaging script checks and copies the worker and its runtime dependencies.

The local installation provides `work/build-shader-feature.ps1` to build the compiler, run core tests, install its runtime files, and rebuild the launcher.
