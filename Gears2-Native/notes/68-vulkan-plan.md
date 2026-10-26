# 68. Vulkan back end - plan (Shagg 2026-10-23: "one renderer, D3D12 + Vulkan"; overnight work)

Goal: the same renderer (shadow_renderer.cpp and everything above gpu::Backend) drawing through Vulkan as well as
D3D12, chosen at start-up; D3D12 stays the default until Vulkan matches it picture for picture. Why (notes/65, D35):
AMD / Intel cards, Linux and Steam Deck; the test PC (RTX 3080 Ti) gains nothing directly. Both games get it (layer 0).

## What exists [V, read 2026-10-23 18:50-19:00]
- **Our side**: gpu::Backend (include/xenos/renderer/gpu.h) is already API-neutral - ~60 virtuals: textures, imports,
  shaders, graphics / compute pipelines (async workers, library, backlog, 0062 promotion, 0063 PipelinePending),
  uploads (guest memory, transient ring, scratch), render targets, clears, viewport / scissor / stencil / blend,
  constants (incl. split), 96 texture views + 32 samplers, draws (plain, indexed, kept / persistent index buffers),
  occlusion queries, dispatch, readbacks, submit / fences, debug events, native escape (BeginNative / NativeTexture for
  DLSS / FSR / XeSS), video memory, stats. Only implementation: renderer/d3d12_backend.cpp (~205 KB).
  gpu::ShaderIL already exists (kDxil today).
- **D3D12 assumptions outside the back end** (must become per-API):
  - shadow_renderer.cpp Init: `gpu::CreateD3D12Backend()`; with --gow2_present=ours it takes the SDK's D3D12Provider
    device + direct queue and its D3D12Presenter.
  - PresentOurs: D3D12Presenter::D3D12GuestOutputRefreshContext (imports the presenter's resource, unpack dispatch).
  - Guest shaders: DXBC (dxbc_gen.cpp, D3D12 only) or HLSL -> DXC -> DXIL (shader_cache.cpp, dxc_compiler.cpp).
  - Built-in shaders (builtin_shaders.h, SMAA, colour_hlsl.h): HLSL compiled by DXC to DXIL.
  - Upscalers: DLSS (Streamline / NGX D3D12), FSR (FidelityFX dx12 DLL), XeSS (D3D12) through BeginNative.
  - The 0063 hold and the "Building shaders" note are API-neutral already.
- **SDK side** (third_party/rexglue-install-llvm23-rwdi/include/rex, Xenia-derived):
  - rex/ui/vulkan: VulkanProvider (instance, device, UI samplers), VulkanDevice, VulkanPresenter with
    VulkanGuestOutputRefreshContext (a VkImage + view to write the guest output into - the Vulkan twin of what
    PresentOurs uses on D3D12), immediate drawer (ImGui overlay: FPS bar, shader note), mem_alloc (VMA),
    upload_buffer_pool, submission_tracker, spirv_tools_context.
  - rex/graphics/vulkan: the SDK's own Vulkan GPU emulation (command processor, pipeline cache, render target /
    texture caches) - not used by us (we take over drawing), but proof the provider / presenter work.
  - rex/graphics/pipeline/shader: spirv_builder.h, spirv_translator.h - Xenia's direct ucode -> SPIR-V translator
    (the Vulkan twin of Canary's DXBC path; no compiler step).
  - Source tree third_party/rexglue-sdk/thirdparty: vulkan-headers, vulkan-loader, vulkan-memory-allocator,
    spirv-headers, spirv-tools, glslang. DXC (third_party/dxc-1.9.2607) can emit SPIR-V (-spirv).

## Shader strategy
1. **Bring-up**: guest shaders through our HLSL translator (hlsl.cpp, already verified against the corpus) compiled by
   DXC with -spirv, with a register-to-binding layout matching the Vulkan back end's descriptor sets. Slow to compile
   (the reason we left HLSL on D3D12), but correct and quick to get going; the 0058 backlog / 0063 hold cover it.
   The local harness already compiles our HLSL to SPIR-V (runps.sh: DXC SPIR-V vs vkd3d) - so the HLSL side is known
   to be SPIR-V-clean for the corpus.
2. **Speed**: a direct ucode -> SPIR-V emitter, our own (like dxbc_gen.cpp) on the SDK's spirv_builder, or the SDK's
   spirv_translator adapted to our bindings. Decide after bring-up by measuring both on the 2,474-shader corpus
   (runps.sh compares values on lavapipe).
3. Built-in shaders: DXC -spirv at build time (or start-up), same HLSL sources.

## Stages (each = build both games + unit tests + scripted screenshot check; commit only on pass)
- **V0 skeleton**: --gow2_renderer_api=d3d12|vulkan (default d3d12). CreateVulkanBackend() behind a CMake option that
  finds the SDK's Vulkan headers; Init from the SDK's VulkanProvider when the runtime runs Vulkan (how the runtime picks
  its provider: to find out - Xenia's --gpu=vulkan equivalent), else its own instance / device. Describe(),
  GetShaderIL() = kSpirv (new). Provider choice [V, notes/32 brief 4]: the xenos GPU plugin's rex_gpu_create takes
  GpuCreateInfo.backend = "d3d12" / "vulkan" / "any" and returns VulkanGraphicsSystem for "vulkan" (D3D12 for "any");
  which option sets that string in rex_app.cpp is still to find. Everything else logged as not implemented; the renderer stays off cleanly.
- **V1 resources**: textures (create / upload / destroy, formats table mirroring D3D12's), buffers, guest memory
  upload, transient ring, readbacks, submit / fences (timeline semaphore), debug labels.
- **V2 draws**: render targets via dynamic rendering (the SDK's device loads the VK_KHR_dynamic_rendering functions - device_1_3_khr_dynamic_rendering.inc), clears, viewport /
  scissor / stencil / blend constant, graphics pipelines from GraphicsPipelineDesc (async workers + VkPipelineCache
  saved to disk = the pipeline library), constants (push / uniform ring), descriptor sets for 96 views + 32 samplers,
  draws incl. index buffers. First milestone: the main menu drawn into our textures (checked by readback, no present).
- **V3 present**: PresentOurs through VulkanGuestOutputRefreshContext; ImGui overlay; frame shots. Milestone: Gears 2
  ch10 picture next to D3D12's (FLIP score from tools/flip, same frame numbers, scripted pad).
- **V4 the rest**: compute dispatch (resolves, unpack, transfers), occlusion queries, MSAA, scaled resolves, SMAA, HDR,
  then upscalers on Vulkan (FSR 3.1 Vulkan backend, DLSS via Streamline Vulkan, XeSS Vulkan) - each untested-hardware
  rule per notes/65.
- **V5 speed**: direct SPIR-V emitter, pipeline backlog / promotion / hold parity, stutter runs as jobs 358-361.

## Rules for this work
- D3D12 behaviour and shader text unchanged by default (every Vulkan line behind the API switch / CMake option).
- Gears 3 builds layer 0 by glob: new files must build there too (its chat told before each merge).
- No vendor SDK committed; Vulkan headers come from the SDK tree already on disk.
- Shagg doesn't play-test until V3 passes the screenshot comparison; then a launcher entry "Graphics API".

## Status
- 2026-10-23 21:40-22:10, stage V1 written (renderer/vulkan_backend.cpp, tools/vktest):
  - Built in: the loader opened at run time (no import library), device pick (discrete first), the timeline-semaphore
    fences, command-buffer slots, the upload ring (counted bytes, wraps, waits), textures (all 28 formats; mutable for
    the signed / bits views; D24S8 falls back to D32S8 where it isn't supported - AMD), uploads (2D / cube / 3D, mips,
    padded rows, BCn), full and rectangle clears (colour and depth/stencil), the guest-memory buffer and its uploads,
    readbacks of textures and of guest memory, deferred frees and debug labels.
  - Checks [V, lavapipe, Mesa 25.2.8]: gn_vktest 24/24 passed. The validation layer's only message is its own age
    (1.3.275 doesn't know the 1.4 feature struct).
  - Not built yet: shaders, pipelines, draws, descriptors (V2); present (V3); compute, queries, upscalers (V4).
  - --gow2_renderer_api=vulkan runs the renderer as shadow only (takeover and our present stay D3D12 until V3).
- Version (Shagg 21:46, "the most up to date vulkan"):
  - We ask for Vulkan 1.4 (the newest core version) and use its features where the driver has them: push
    descriptors, maintenance5 / 6.
  - The minimum stays 1.3, so drivers that stop there still run.
  - Headers: a Khronos Vulkan-Headers release in third_party/vulkan-headers-v1.4.* (the job fetches v1.4.365, the
    newest seen on 2026-10-23), else the SDK's 1.4.357.
- 2026-10-23 22:00-22:20, stage V2 written:
  - Committed on the PC as b903bdb (job 370): V1 [V, RTX 3080 Ti, NVIDIA 617.42, Vulkan 1.4.351, headers 1.4.365].
    gn_vktest 24/24.
  - Shaders: SPIR-V from DXC. Our fixed shaders are compiled with -fvk-{b,t,s,u}-shift 0/100/200/300 and moved into
    the layout by RemapBindings; the translator's set 1 / 2 bindings are kept; the pixel shader's b0-b3 go to set 0
    bindings 4-7.
  - Pipelines: dynamic rendering, one fixed layout, asynchronous workers, a Vulkan pipeline cache.
  - Descriptors: dynamic uniform buffers in the ring; texture / sampler sets cached per frame pool; views by swizzle /
    signed / bits / plane; white and zero defaults.
  - Passes: begun at the first draw; ended before any transfer; split with a full barrier when a draw samples a
    target written in the same pass.
  - Viewport: negative height, for D3D's y-up NDC.
  - Draws: plain, indexed through the ring, persistent index buffers.
  - Checks [V, lavapipe with synchronization validation]: gn_vktest 44/44. Covered: draw, depth, blend, texture +
    swizzle, render-to-texture then sample, guest-memory vertices (plain / indexed / index buffer), async pipelines.
  - --gow2_renderer_api=vulkan now allows takeover. Our renderer draws, but there is no picture until the resolves
    (V4 compute) and present (V3).
- 2026-10-23 22:05-22:20:
  - V2 committed as 0e458d5 (job 371) [V, RTX 3080 Ti: gn_vktest 44/44]. Its 60 s ch10 run on Vulkan in takeover:
    - no device loss;
    - the game's own shaders all compiled to SPIR-V and were accepted (the 6 SHADER-FAIL were our compute shaders,
      not built yet);
    - render targets created and drawn.
  - V4 compute written: per-shader layouts from the SPIR-V's descriptor types; Dispatch with constants, t0-t3 (depth /
    stencil planes), u0 / u1 storage images or the guest / scratch buffer; the BCn scratch copy. gn_vktest 48/48 on
    lavapipe with validation.
- 2026-10-23 22:25: --gow2_present=ours on Vulkan runs our whole present (gamma, SMAA, HDR) into our own image,
  with no presenter yet. The frame shots read it back, so a Vulkan run can be compared frame for frame with a D3D12
  run (FLIP). The window stays black until V3. Job 373 does that comparison on ch10 with a fixed clock.
- 2026-10-23 22:15-22:25, jobs 373 / 374 [V, RTX 3080 Ti]:
  - First Vulkan picture: frame 300 (pause menu) against D3D12's, FLIP mean 0.0015.
  - After Start the cold run made no frame for 60 s: 7 draws had waited 24.6 s for HLSL -> SPIR-V compiles. On
    373's shader cache (job 374) it got through: frame 316 (the chapter load) took 70 s, 53 s of it building
    pipelines, then ~65 ms per frame against D3D12's ~18 ms. Slow, not stuck; speed is V5 (direct SPIR-V from the
    translator instead of HLSL through DXC, narrower barriers, pipeline cache on disk).
- 2026-10-23 22:40, V3 (the window), patch 0074 + SDK patch 33:
  - The SDK must be built with REXGLUE_USE_VULKAN=ON (Shagg's build had OFF) for its Vulkan provider and presenter.
    SDK patch 33: device API 1.4; enables timeline semaphores, synchronization2, 1.4 push descriptors / maintenance5
    and (with GPU emulation) BC textures, gather with offsets, storage writes without format, where supported.
  - Our GPU system makes the SDK's Vulkan provider for --gow2_renderer_api=vulkan (D3D12 otherwise, or when the SDK
    has no Vulkan); the backend runs on its device (BackendOptions::vulkan_external: instance, physical device,
    device, graphics + compute queue 0 and its mutex, what was enabled). It never destroys that device and waits
    only for its own work at shutdown.
  - Present: the presenter's guest-output image is a colour attachment + sampled + transfer source only (no storage),
    so Backend::PresentPacked draws one triangle whose pixel shader unpacks our words (embedded SPIR-V,
    vk_present_spirv.h), and leaves the image in SHADER_READ_ONLY_OPTIMAL for the presenter (its contract).
  - gn_vktest: a device of its own as the SDK would, our backend on it, PresentPacked twice (first write, then from
    the presenter's layout), the image read back word for word: 57/57 on lavapipe with validation.
- 2026-10-23 23:04, Shagg: "I'd like vulkan to eventually become our main renderer." Decision: Vulkan is the target
  main renderer. Order: V3 window; V5a direct SPIR-V from our translator (as the DXBC writer did for D3D12; Canary's
  SpirvShaderTranslator as the reference); V5b narrow barriers; V5c recording on several cores (notes/52's next step);
  parity (HDR, SMAA, sharpen, resolution scale, upscalers, warm-up, pipeline cache on disk); gates: picture for picture
  across chapters in both games, speed >= D3D12, long sessions without crashes, an AMD card if available; then the
  default flips and D3D12 stays as a fallback switch. The launcher stays on D3D12 until the gates pass.
- 2026-10-23 23:20-23:55, V5a (patch 0075): direct SPIR-V, as a DXBC -> SPIR-V converter (dxbc_spirv.cpp) rather
  than a second translator: the Xenos maths stays in one place (dxbc_gen.cpp, played on D3D12) and Vulkan follows it
  instruction for instruction. It covers what dxbc_gen emits (78 instruction kinds; vertex + pixel shaders; SM 5.1
  range IDs; structured if / loop / switch; raw buffers; 2D / cube / 3D textures with separate samplers) and gives the
  HLSL path's interface exactly (bindings, locations, float4[16] TEXCOORD0), so converted and DXC shaders mix freely.
  D3D semantics where SPIR-V leaves results undefined: float -> int saturating with NaN -> 0, shift counts & 31,
  bit-field extracts past bit 31, udiv by 0 -> ~0; float maths NoContraction; discard = demote to helper.
  - Checks [V, local]: spirv-val (Vulkan 1.3 rules) on every converted shader - Gears 2 corpus 2,475 / 2,475 (normal
    and slim DXBC), second corpus 2,474 / 2,474, Gears 3 sample 100 / 100. Conversion is a fraction of a millisecond.
  - psdiff: each pixel shader drawn from the HLSL path (DXC) and from the converter on the same random inputs
    (realistic system constants, random float / bool / loop / fetch constants, 2D / cube / 3D textures), four RGBA32F
    targets compared bit for bit on lavapipe.
  - Wiring: ShaderCache's DXBC path runs on Vulkan too (--gow2_renderer_shader_backend=dxbc, the launcher's setting):
    DXBC, then SPIR-V; a shader the converter declines takes the HLSL path. --gow2_renderer_vulkan_direct_spirv=false
    keeps Vulkan on HLSL / DXC. D3D12 unchanged.
  - psdiff result [V, lavapipe]: bit-identical draws on every pixel shader of the corpus run so far, except one
    (ps-CE4DE0AECFE55CDE: 6 of 16,384 values in one 2x2 quad). Replayed with the same inputs: identical with
    single-mip textures, so a last-place rounding difference in the coordinates fell on a mip boundary (implicit LOD
    picks a different level for that quad). Not a conversion error; the converter follows the DXBC D3D12 plays.
- 2026-10-24 00:00, job 376 [V, RTX 3080 Ti]: 0075 committed (ca29449). Cold ch10 on Vulkan: 1,601 shaders through the
  converter, 0 left to the HLSL path; screen check exact; but frame 316 (the chapter load) still took 69.4 s, all in
  pipeline builds ("record"). Cause [V, from the log and the code]: the Vulkan backend had one FIFO pipeline queue and
  ignored SetPipelineBacklog, so 1,568 guessed pipelines (patch 0066: the game created 1,778 shaders, 5,160 more
  waiting) queued ahead of the draws' own pipelines, and a waiting draw waited its turn behind them.
- Patch 0076 (as D3D12's 0057 / 0062): live requests first, backlog jobs (start-up set, guessed pipelines) after them
  and on at most half the workers; a draw that needs a job still queued builds it at once (waiting draw) or moves it to
  the front (async). Also no DXBC rectangle-list GS on Vulkan (the job-376 "CreateShader got no SPIR-V" error).
  [G, unexplained]: the 0066 shader-creation hooks reported 1,778 shaders on Vulkan and 0 on D3D12 in the same run.
- 2026-10-24 00:15, job 377 [V]: 0076 committed (e61c2fe). ch10 chapter load (frame 316): cold Vulkan 69.4 s -> 1.9 s;
  warm (same cache, second run) 144 ms, mostly texture uploads, pipeline waits ~0. Both runs reached frame ~1500 in
  100 s (screen checks exact, 0 HLSL fallbacks). Frame time is now the gap: frames 901-1200 Vulkan 61.3 ms (CPU split:
  "frame" 38.9 - waiting at the swap, record 9.2, vertex 4.4, constants 2.7) against D3D12 15.7 ms (frame 4.2,
  record 3.1; GPU 17.3 ms). [G] Vulkan's GPU time is the wait - V1's full barriers at every transition and pass.
- Patch 0077: GPU timestamps on Vulkan (as D3D12 jobs 176-177): a timestamp when work of a new category starts, one
  at the end of each list, summed by category when the list completes; so the CPU-split line shows Vulkan's GPU ms
  by category like D3D12's. gn_vktest checks it (58).
- 2026-10-24 00:32, job 378 [V]: 0077 committed (95eb791). GPU time per frame (frames 601-900, ch10): Vulkan 63.4 ms
  (draws 60.1, EDRAM transfers 0.7, resolves 1.2, texture loads 0.5) against D3D12 17.5 ms (draws 14.7, 0.4, 1.0, 0.4):
  the draws themselves. Barrier calls per frame: Vulkan ~4,800 (40-57 image transitions), D3D12 ~380.
  Cause [V, code]: the Vulkan backend had no UploadTransient, so every small per-draw vertex range (job 160's direct
  ring reads on D3D12) fell back to UploadGuestMemory - a copy, which on Vulkan ends the rendering pass and costs a full
  barrier, then a new pass with another: about two full GPU drains per draw.
- Patch 0078: UploadTransient on Vulkan (ring allocation + memcpy; t97 is bound whole at offset 0). gn_vktest draws a
  quad from ring vertices at the returned offset (60).
- 2026-10-24 00:45, job 379 [V]: 0078 committed (d1b39b4). ch10 frames 601-900: Vulkan frame 22.1 ms (D3D12 ~18),
  GPU 18.9 ms (D3D12 17.5), barrier calls 325 a frame (D3D12 353); screen checks exact, 0 HLSL fallbacks. Left: hitches
  of 0.4-1.9 s around frames 2015-2120 (new areas; the test waits for pipelines on purpose, wait_ms=-1) and CPU cost
  (record 5.9 vs 3.0 ms, constants 2.8 vs 1.2 ms).
- Patch 0079: (a) the workers' build times reach the stats (pipelines_built_async, pipeline_async_ms / _max_ms /
  _bytes), so patch 0057's log line shows the driver's time per new pipeline on Vulkan as on D3D12 - whether Vulkan
  builds are really slower. (b) The VkPipelineCache is saved as pipelines-vulkan.bin next to pipelines-d3d12.bin and
  loaded at start (a file from another driver / GPU is ignored by the driver's header check; data it refuses -> empty
  cache). Saved every pipeline_save_seconds (15) when new pipelines were built, on a background thread through a .tmp
  file and a rename, since the harness ends the process without a shutdown; and once more at shutdown. [G] On NVIDIA
  vkGetPipelineCacheData of a large cache runs on the render thread every 15 s while building - job 380 measures it.
  gn_vktest: cold / warm / damaged-file runs (72).
- 2026-10-24 01:04, job 380 [V, RTX 3080 Ti]: 0079 committed (4debe04); gn_vktest 72 on NVIDIA. ch10 Vulkan twice on
  one cache (job 379's, no Vulkan pipeline file): run A built the 358 recorded pipelines in 7,230 ms of driver time
  (20.2 ms each, slowest 109.6) and wrote pipelines-vulkan.bin (43.5 MB); run B loaded it (42,449 KB) and built the same
  358 in 992 ms (2.8 ms each, slowest 94.8) - 7.3x less. D3D12 in the same job: 160 pipelines, 715 ms. No 0.4-1.9 s
  hitches in either run (frames 1900-2300: one 60 ms frame each); worst frame 64.6 / 63.9 ms. [G] the ~60 ms frames
  every 300 frames are the harness's screen checks (gow2_present_check_every=300), not the game. Frame time after frame
  1200: Vulkan 22.0 / 21.6 ms against D3D12 18.2; GPU 18.9 vs 17.4; CPU total 19.8-20.0 vs 16.0 ms, the gap in record
  (5.9 vs 3.0), constants (2.8 vs 1.1), vertex (2.9 vs 2.1) and index (1.0 vs 0.4). Screen checks exact, 0 HLSL
  fallbacks. Next: that CPU gap.
- Patch 0080 (the CPU gap, first part): as D3D12's SetConstants, a constant slot that already holds the same data in
  the command buffer being recorded is not uploaded again (a shadow per slot, dropped when a new command buffer
  begins), SetConstantsSplit copies its two pieces straight into the ring (was a heap vector per call), and the
  descriptor sets are rebound only when an offset changed. SetTextures marks the sets for a new lookup only when a
  view or sampler really changed; PrepareDraw's layout check walks only the slots holding a texture; each sampler slot
  keeps its last VkSampler while its description is unchanged. Also a latent bug: a cached descriptor set found again
  did not mark its pool as in use by the list being recorded, so the pool could be reset under it once its older
  submission finished; now a hit marks it, and the sets are looked up again at each new command buffer.
  gn_vktest 75 (repeats skipped, a change lands, a new list uploads again).
- 2026-10-24 01:21, job 381 [V, RTX 3080 Ti]: 0080 committed (2208972); gn_vktest 75. ch10 Vulkan warm, frames 1201-1500
  against job 380's run B: CPU 20.0 -> 17.5 ms a frame (D3D12 16.1); constants 2.79 -> 1.32 (D3D12 1.11), vertex 2.85 ->
  2.14 (2.10), record 5.94 -> 5.27 (2.94). Frame time after frame 1200: 21.6 -> 19.6 ms (D3D12 18.2). Vulkan is now
  close to its GPU time (18.8 ms vs D3D12 17.5: draws 15.3 vs 14.7, EDRAM transfers 0.72 vs 0.44 - depth 0.12 vs 0.06
  and colour->depth 0.20 vs 0.08 - resolves 1.24 vs 0.95); D3D12 waits 4.4 ms a frame on its GPU, Vulkan 0.2. Record
  per drawn draw 1.08 us vs 0.60. Screen checks exact, 0 HLSL fallbacks.
- Patch 0081: dynamic state (viewport, scissor, stencil reference, blend constant) recorded only when it changed;
  index reuse as D3D12's job 178 (DrawIndexedKeep / DrawIndexedAt: a repeated index range in the same command buffer
  draws from its ring copy instead of being read, converted and copied again - Vulkan's "index" 1.0 ms vs D3D12 0.4);
  the ring bound as the index buffer once per command buffer, each draw choosing its range with firstIndex, and an
  index buffer rebound only when it changes. gn_vktest 78.
- 2026-10-24 01:32, job 382 [V, RTX 3080 Ti]: 0081 committed (7a72ef0); gn_vktest 78. ch10 Vulkan warm, frames
  1201-1500, against job 381: record 5.27 -> 3.65 ms (D3D12 2.98), index 1.01 -> 0.42 (0.41), CPU total 17.5 -> 16.9
  (15.9); record per drawn draw 1.08 -> 0.75 us (0.61). GPU 18.8 -> 17.9 ms (D3D12 17.4): draws 15.3 -> 14.5 (14.7), so
  the rest of the GPU gap is EDRAM transfers (0.73 vs 0.44: depth 0.11 vs 0.06, colour->depth 0.21 vs 0.08) and
  resolves. Frame time after frame 1200: 19.6 -> 18.7 ms (D3D12 18.2). Screen checks exact, 0 HLSL fallbacks.
- Patch 0082 (the GPU gap): barriers are queued and sent as one vkCmdPipelineBarrier2 just before the next command
  that does GPU work, so a pass's attachment transitions plus its full barrier, a draw's texture transitions plus its
  barrier, and a dispatch's barrier after it plus the next dispatch's before it each become one wait (job 382: ~315
  barrier calls a frame, each a full ALL_COMMANDS wait, against D3D12's ~193 batches). An image already queued gets its
  new layout folded into the queued transition. Queued barriers are sent before vkEndCommandBuffer. The scopes stay
  ALL_COMMANDS (narrowing them is a later step). gn_vktest 79 (two dispatches: 2 barrier calls, was 4+).
- 2026-10-24 01:41, job 383 [V, RTX 3080 Ti]: 0082 committed (bd79087); gn_vktest 79. ch10 Vulkan warm, frames
  1201-1500, against job 382: barrier calls 316 -> 196 a frame (D3D12 193 batches); GPU 17.95 -> 17.69 ms (D3D12 17.40):
  EDRAM transfers 0.73 -> 0.66 (0.44), resolves 1.25 -> 1.12 (0.95), draws 14.53 (14.68). CPU 16.35 vs 16.01. Frame
  time after frame 1200: 18.72 -> 18.44 ms (D3D12 18.17). Screen checks exact, 0 HLSL fallbacks. Next: job 384, a
  five-chapter sweep on both APIs (the "checks" step) before more speed work.
- 2026-10-24 01:59, job 384 [V, RTX 3080 Ti]: five-chapter sweep, both APIs, fixed clock, 80 s, caches warm only for
  ch10. No errors, no crashes, no HLSL fallbacks; every Vulkan screen check exact (39). ch02 / ch05 Vulkan: the 80 s
  went on the first visit - shader warm-up 44 s (D3D12 32 s; both "0 loaded from disk") and pipeline waits of 29 s
  (ch02 frame 2; D3D12 17 s) or 16 + 19 + 11 s (ch05) with the harness's wait-for-every-pipeline setting - not a hang.
  ch10: Vulkan 18.67 vs 18.17 ms. ch15: Vulkan 8.26 vs 5.38 ms, GPU-bound (GPU 8.1 vs 5.2 ms; draws 5.3 vs 2.9 ms with
  ~670 draws a frame; barrier calls 220 vs 196 batches) - the light-scene gap, job 386 times it by render target. ch22
  (first visit on both): Vulkan 16.1 vs 5.5 ms with pipeline hitches up to 7.2 s; its shots differ from D3D12's by FLIP
  0.05-0.18 because the cutscene camera is at a different point (same scene, shifted - timing, not rendering).
- 2026-10-24 02:18, job 385 [V]: the same sweep on copies of job 384's caches, 100 s each. No errors, no crashes,
  every Vulkan screen check exact (62), 0 HLSL fallbacks. Frame time after frame 1200, Vulkan vs D3D12: ch05 18.18 vs
  11.66 ms (52 hitches >= 33 ms), ch10 19.95 vs 18.37, ch15 8.44 vs 6.11, ch22 9.64 vs 5.36 (one 2.9 s hitch); ch02
  Vulkan again never got past frame ~300. Cause of ch02 [V, log]: the shader warm-up - 12,498 recorded shaders in
  91.6 s on Vulkan; D3D12's was 81.3 s for 13,581 while its translation took 2.0 s in all. So the start-up time goes on
  reading thousands of small .ucode files (both APIs), and grows with every chapter recorded.
- Patch 0083 (both APIs, start-up): WarmFromDisk lists the files, reads them on up to 8 threads, then makes the shaders
  one by one as before (same order, same bytes); the log line splits the time into listing / reading / making so the
  next job shows where it goes.
- 2026-10-24 02:30, job 386: no data - both ch15 runs spent their 96 s in the start-up warm-up (job 385's caches hold
  ~13,600 recorded shaders); repeated after the warm-up fixes.
- 2026-10-24 02:30, job 387 [V]: 0083 committed (f4c06b4). Warm-up on copies of job 385's caches: D3D12 81 s -> 16.8 s
  for 13,585 shaders (listing 18 ms, reading 15.6 s on 8 threads, making 1.2 s); Vulkan 37.0 s for 13,513 (reading
  15.1 s, making 21.9 s - the SPIR-V conversion and modules, serial). ch10 D3D12 gate exact.
- Patch 0084 (both APIs, start-up): on the DXBC path the warm-up decodes, analyses and translates (and on Vulkan
  converts to SPIR-V) on a thread pool; only taking each result into the cache (CreateShader, the maps) stays on the
  renderer's thread, in file order, through one function shared with GetImpl (AdoptDxbc); shaders the DXBC path does
  not take go through GetImpl as before. The log line adds the translating time. Same bytes as before (pure function).
- 2026-10-24 02:46, job 388 [V]: 0084 committed (7f1b7b2). Warm-up on job 385's caches: D3D12 16.1 s (reading 15.6,
  translating 0.4, making 0.04), Vulkan 37.0 -> 20.1 s (reading 15.0, translating 4.2 on the pool, making 0.9). ch15 GPU
  time by render target, frames 1201-1500, Vulkan vs D3D12: 7e3 scene target 3.23 vs 1.89 ms (233 / 228 draws),
  k_8_8_8_8 full-screen pass 0.88 vs 0.41 ms (one draw), D24S8 shadow 0.31 vs 0.13, 16_16_16_16_FLOAT 0.22 vs 0.12,
  16_16_FLOAT 0.21 vs 0.10 - but the depth-only prepass target D24FS8 0.58 vs 0.53. Colour targets take ~2x, depth-only
  matches, even for a single full-screen draw: per-pixel cost, not barriers. Cause [G, strong]: every colour image was
  created MUTABLE_FORMAT with no format list (for its signed / bits views); NVIDIA turns off colour compression for
  such images. Depth images were never mutable.
- Patch 0085 (two parts). (a) Both APIs, start-up: ucode/recorded.pack - every recorded shader in one file
  ("GNUCPAK1", then {u64 hash, u32 flags, u32 dwords, dwords} records appended by the ucode writer thread; a record cut
  short ends the valid part and the writer trims the file back before appending). The warm-up reads the pack first
  in one read, then only the .ucode files not in it, and queues those into the pack so the next start reads them
  from it. The .ucode files are still written. (b) Vulkan: an image is mutable only when it has a signed or bits view
  format, and then with VkImageFormatListCreateInfo listing exactly its view formats. gn_vktest 80 (a signed view
  of RGBA8 reads 0x7F as 1.0).
- 2026-10-24 03:07, job 389 [V]: 0085 committed (1563106). recorded.pack works: warm-up on job 385's caches, first
  start (files read, 13,585 queued into a 10.0 MB pack) 16.6 s, second start 0.49 s on D3D12 (reading 0); Vulkan
  20.6 s -> 4.7 s (reading 0; translating 4.0 s, the SPIR-V conversion, is what is left). (The log's "recorded shaders"
  counted pack shaders twice - fixed in 0086.) The format-list half did NOT help ch15: Vulkan 7e3 scene target 3.05 ms
  vs D3D12 1.56, the one-draw k_8_8_8_8 pass 0.88 vs 0.38, the D24S8 shadow target 0.31 vs 0.12; frame 8.34 vs 5.72 ms.
  So my compression guess was wrong [V]; the depth prepass target (#3) still matches. Next diagnostic (job 390): the
  same ch15 frames on Vulkan with our DXBC -> SPIR-V shaders vs DXC's HLSL -> SPIR-V shaders (warm-up off, so only the
  ~1,000 drawn shaders compile) - whether the converted shaders' code is the cost.
- Job 390b (24 Oct 03:16, patch 0086 = e9a7730, "Match my monitor"): ch15 Vulkan with our shaders again 8.33 ms GPU
  (7e3 scene target 3.07 ms, the one-draw k_8_8_8_8 pass 0.88) [V]; the HLSL half produced no numbers (2 shots, no
  window lines) - the shader-code question stays open. Patch 0087 tests the other explanation directly: 0085 kept every
  colour target that has a signed or bits view mutable (with a list), and a list that mixes UNORM / SNORM / UINT may
  still turn compression off. --gow2_renderer_vulkan_compressible_targets=lazy makes colour render targets one-format
  images and remakes one mutable (contents copied, logged) only when such a view is first used; "never" measures the
  ceiling (wrong values, measurement only). Job 391 runs ch15 with always_mutable / lazy / never.
- Job 391 (24 Oct 03:51, patch 0087 = 9b55cfc) - compression ruled out [V]: ch15 GPU 8.47 ms always_mutable, 8.47 lazy
  (only 4 small targets ever needed a second format; the 7e3 scene target stayed one-format: 3.28 vs 3.26 ms), 8.40
  never (no second format at all; wrong values, measurement only). The colour targets' 2x is not compression.
  Two suspects left, measured by job 392: (a) our SPIR-V marks all float maths NoContraction (no FMA) - in-game A/B
  with --gow2_renderer_vulkan_precise=false; (b) the SDK creates its device with robustBufferAccess (with GPU
  emulation), which bounds-checks every constant / buffer read - gn_vktest --bench times a constant- and buffer-heavy
  pixel shader with robustBufferAccess off / on x fusable / precise maths on the real GPU. If (b), the fix is SDK patch
  34 (no robustBufferAccess, or pipeline robustness off for our pipelines).
- Job 392 (24 Oct 04:04, patch 0088 = c321233) [V]: gn_vktest --bench on the RTX 3080 Ti, a 64-constant / 64-load
  full-screen pixel shader at 2560x1440: robustBufferAccess off 1.53-1.67 ms, on 1.55 ms; precise vs fusable maths the
  same within noise. So neither robustness nor NoContraction explains the 2x in a plain DXC-made shader (the in-game
  precise=false A/B is in the same job). Next suspect, from reading the backend: every guest-memory upload, texture
  upload, dispatch and clear ends the render pass (Vulkan forbids copies inside one), and every pass starts with a full
  ALL_COMMANDS barrier (V5b, narrow barriers, was never done) - D3D12 has neither. Patch 0089 counts passes, full
  barriers and why each pass ended, per frame.
- Patch 0090 (V5b first step): --gow2_renderer_vulkan_pass_barriers=when_needed - a render pass starts with the full
  barrier only when something since the last barrier could conflict: GPU work outside a pass (EndPass with any reason
  but new targets marks it), a memory-export draw, or one of the pass's attachments sampled or written since (per-image
  barrier epochs); textures a draw samples that were written since keep PrepareDraw's split. gn_vktest --when-needed
  runs the whole suite in that mode: 87/87 with Khronos synchronization validation, 0 SYNC-HAZARD (lavapipe) [V].
  Job 394 measures ch15 / ch10 full vs when_needed (GPU by target, passes, FLIP).
- Job 394 (24 Oct 04:28, patch 0090 = e3b64f6) [V]: ch15 when_needed vs full - 73.7 of 110.5 pass starts a frame
  without a barrier (full barriers 310 -> 237, barrier calls 223 -> 201); GPU by target: 7e3 scene 3.21 -> 2.95 ms,
  the one-draw k_8_8_8_8 pass 0.88 -> 0.77, the D24S8 shadow target 0.30 -> 0.20; FLIP identical through frame 1200,
  <= 0.003 after (the characters' timing, as between any two runs). Patch 0091 makes when_needed the default (Vulkan
  only; D3D12 untouched). What remains: each dispatch (49 a frame in ch15, the EDRAM transfers) has a full barrier
  before and after; those order real dependencies (a transfer reads the target the pass just drew), so the next step
  is stage-limited barriers rather than skipping (V5b step 2).
- Patch 0092 (V5b step 2): --gow2_renderer_vulkan_clear_in_pass - a clear of a bound target runs inside the open pass
  (vkCmdClearAttachments; the pass is begun if needed; only within the pass's render area) instead of ending it,
  changing layouts twice and beginning a new pass (job 393: 10-15 such pass ends a frame). gn_vktest --when-needed now
  also turns this on: 87/87 with synchronization validation, 6 in-pass clears and 5 barrier-free pass starts exercised,
  0 hazards (lavapipe) [V]. Job 396 measures it in ch15 / ch10.
- Jobs 395-396 (24 Oct 04:35-04:45; 0091 = 2b25f8c when_needed default, 0092 = fad0d39 clear_in_pass switch) [V].
  Correction to job 394's reading: the ch15 scene target varies 2.95-3.29 ms between runs with the SAME settings
  (394 when_needed 2.95, 396 when_needed 3.28), so its "3.21 -> 2.95" was within noise. What holds in every run: the
  D24S8 shadow target 0.29-0.31 ms (full) vs 0.19-0.23 (when_needed), and fewer barrier calls (223 -> 201 a frame).
  clear_in_pass (396): 11 clears a frame stay in the pass, passes 110 -> 101, barrier calls 201 -> 179, shadow 0.19 ->
  0.17, scene 3.28 -> 2.99 (noise-sized); FLIP identical at frame 300, <= 0.011 later (timing).
  Measuring note: compare targets across >= 2 runs before reading a < 10% change.
  ch10 (396): passes 90.4 -> 76.0, barrier calls 186 -> 151 a frame, 17 clears a frame in the pass; frame time the same
  (CPU-bound); the frame-600 difference is the characters' positions only (mask checked), scenery identical. Patch
  0093 makes clear_in_pass the default (Vulkan only).
