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

## CPU scheduling

Standalone preparation defaults to Windows **Above normal** process priority. Its pipeline and driver-warming pools default to one worker per logical CPU, up to 32, instead of capping driver warming at eight workers. On the local Ryzen 7 9800X3D this permits 16 concurrent driver workers. Pipeline and package-reader threads use normal relative thread priority within the elevated process, replacing the lowest-priority driver-warming/readers used during gameplay.

Package readers default to half the logical CPU count, capped at eight, and are reduced when available physical memory is low. The scheduling allowance reserves 4 GiB and allows one reader per remaining 2 GiB, with at least one reader; this does not impose a hard limit on individual package allocations. A failed memory query selects one reader. Gameplay retains its existing defaults.

Explicit advanced CPU flags are respected. `--gow2_renderer_prepare_priority=-1|0|1|2` selects Below normal, Normal, Above normal, or High, respectively. Real-time priority is not used. Worker counts can be overridden with `gow2_renderer_pipeline_threads`, `gow2_renderer_shader_prebuild_workers`, and `ue3_shader_prebuild_threads`; relative priorities use the corresponding existing pipeline/warming flags and the new `ue3_shader_prebuild_thread_priority` (-2 lowest, -1 below normal, 0 normal). Startup logging records the requested and actual process priority, selected counts and available memory. Other Windows scheduling software can override the requested priority.

Deep translation offers each newly translated variant to the bounded driver-warming queue immediately. Previously driver workers received new variants only after the coordinator's entire 20 ms translation slice, leaving gaps on a warm driver cache. Cache mutation and device shader creation still occur on the coordinator; driver pipeline compilation remains on its worker pool.

## Build and packaging

Build the Windows CMake target `CogBoxShaderCompiler` from Gears2-Native. It links the common engine/renderer and patched SDK; no recompiled game source is required. Place it beside `CogBox.exe` with `rexruntime.dll`, `dxil.dll`, the normal shared DXC/upscaler files, and the matching `D3D12/D3D12Core.dll`. The packaging script checks and copies the worker and its runtime dependencies.

The local installation provides `work/build-shader-feature.ps1` to build the compiler, run core tests, install its runtime files, and rebuild the launcher.
