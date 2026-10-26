# 70 - Renderer overview: what is done, what is left (Shagg 02:38, 2026-10-24: "surprised there is nothing in the notes about a renderer overhaul")

One page over the renderer notes, so the overhaul and its open parts can be found in one place. Details stay in the
notes named here. [V] = measured, [G] = guess.

## Done

| Work | Where | Result |
|---|---|---|
| Native renderer (stages 0-6): our own Xenos renderer replaces the SDK's GPU emulation ("takeover") | notes/28, 42, 44-51 | Every chapter drawn by us; shots checked against the reference |
| Speed overhaul ("use as many cores as possible"): PM4 split onto its own thread, threaded recording, 12 pipeline workers, direct vertex / index reads, texture and transfer memos, constant trims and compaction | notes/52 (jobs 156-251) | ch10 D3D12 ~17 fps -> ~80 fps [V]; stopped at the 3% rule (section 28) |
| Graphics options: resolution scale, SMAA, HDR, uncapped frame rate, ultrawide, DLSS / FSR / XeSS | notes/53-57, 64 | Shipped behind launcher options |
| Canary / Edge items for textures and guest memory | notes/66 | Ported where marked (BSD notice) |
| Stutter work: shader pack, async pipelines, holds, shader prebuild at package open | notes/67, 69 | Warm-cache hitches mostly gone [V, jobs 330+] |
| DXBC shader path ("overhaul option B"): our own DXBC instead of HLSL through DXC | notes/68 (V5a), code | Shaders ready at once; default on D3D12 |
| Vulkan renderer (Shagg: to become the main renderer) | notes/68 | Picture exact; ch10 18.4 vs 18.2 ms at best [V, job 383] |
| Start-up shader warm-up on several threads | notes/68 (0083, 0084) | D3D12 81 s -> 17 s [V, job 387]; Vulkan pending job 388 |

## The overhaul: old Xenia -> Canary / Edge (Shagg 02:48 "the overhaul I'm referring to")

The plan lives in two Claude Docs (not in this folder until now): "Renderer update map: old Xenia -> Canary / Edge"
and "Renderer: port pieces vs rebuild on Canary/Edge" (both 23 Oct). Decision there: keep our renderer and port the
Canary / Edge pieces it lacks in tested batches, each behind a switch. Its order (Shagg 23 Oct 14:43-14:53): (1) the
shader freeze - our own DXBC back end; (2) Vulkan; (3) spread the renderer's CPU work across cores. Done [V, PC
commits]: cleanup 1 (comparison system removed, 0035), group 1 Canary shader rules on (0037), memexport readbacks
merged (0042-0043), the held batch 0039-0055 (9df6d2f: Canary resolve rules, texture-fetch fixes, Edge stand-in
waits, bigger descriptor heaps, the DXBC back end), DXBC the default; step 2 (Vulkan) in progress. Still to do:
shader translator catch-up S1 (maths: mulsc rounding, 21-bit rcp/rsq/exp/log/sqrt), S2 (texture fetch: MSAA sample
positions for UE3, filtered fixed rounding, integer fetch scaling, locked-mip size, wide 1D / two-component tfetch1D),
S3 (float16 extended range in RT packing, stacked-layer clamp) - now in dxbc_gen, so each fixes D3D12 and Vulkan at
once; Edge's 7e3 -> 8888 tile value conversion; every texture format loaded on the GPU; size checks before creating
targets; transfers between all format pairs; cleanup 2 (split the 7,500-line shadow_renderer.cpp); step 3 (V5c).
The vertex explosions are gone (Shagg 02:53), so mulsc is a precision item now, not a bug fix.

## Left, in the agreed order (Shagg 02:37 "sounds good", 02:53 "your recommended order ... make sure nothing breaks")

1. **Native resolution ("Match my monitor")** - notes/72: whole-number render scale chosen from the monitor, then a
   linear-light exact-size resample in the present pass - done (0086 = e9a7730, job 390b: screen checks exact on both
   APIs [V]). Next: the game's shape on non-16:9 monitors and DLSS / FSR output at the exact size. Gears 3 has the
   display-size API (mail 24 Oct 03:25).
2. **Start-up: one shader file** - done (0085, job 389): warm-up D3D12 16.6 s -> 0.5 s, Vulkan 20.6 s -> 4.7 s [V].
3. **Overhaul catch-up**: every newer Canary / Edge GPU fix checked against ours in notes/73 (most were already in).
   Patch 0087 (job 391) ports the rest of S1-S3 that apply (tfetch1D Y, float16 NaN, mulsc rounding off, packed mip
   tail at base, k_8_A resolves). Left: MSAA sample positions and predicated-tiling offsets (Gears 3 / 1 only - Gears 2
   draws no MSAA targets), the Edge tile conversion, GPU-loaded formats, target size checks, all-pair transfers,
   cleanup 2.
4. **Vulkan GPU time in light scenes** - ch15 8.3 vs 5.7 ms [V, job 389]. Ruled out by measurement (jobs 391-392):
   colour compression, robustBufferAccess, NoContraction (5-10% at most). Found (job 393): ~110 render passes and
   ~310 full barriers a frame in ch15 (passes end on new targets 50, dispatches 49, clears 10). Done: pass barriers
   only when needed (0090/0091, default) - shadow target 0.30 -> ~0.2 ms in every run; clears inside the pass
   (0092/0093, default) - ch10 passes 90 -> 76. Left: the dispatches' barriers (they order real dependencies -
   stage-limited barriers or fewer transfers), and V5c. Measuring note: the ch15 scene target varies ~10% between
   identical runs.
5. **Step 3 of the overhaul: recording on several cores (Vulkan stage V5c)** and GPU-state decoding on workers.
6. **First-visit stutter** - shaders and pipelines built mid-game on a first visit; worse on Vulkan (cold driver
   pipelines ~20 ms each vs D3D12 ~4.5 ms [V, job 380]).
7. **Frame pacing** - 0088: the finest Windows timer at start (was 1 ms, now 0.5 ms); 60 fps cap p99 22.6 -> 20.5 ms,
   worst 36.7 -> 25.0 ms [V, one run each, every window better]. Further: measure in play.
8. **Gears 2 memory gauge** - done (0088, --gow2_memory_log_seconds=10): ch10 uses 275-310 of 512 MB, lowest free
   202 MB [V] - room for a bigger texture pool than 196 MB if play shows blurry textures (not changed).
9. **MipFilter=Linear ini idea** - after Gears 3 has tested it.
10. **Make Vulkan the default** - when it matches D3D12 in every chapter; D3D12 stays as the fallback (notes/68).

Later, outside this renderer list: the Gears 1 and Judgment ports (the Judgment chat is preparing). Everything in the
shared layer 0 renderer carries over to them.

## Unexplained, being watched

- The patch-0066 shader-creation hooks fire on Vulkan but not on D3D12 in the same run [V, job 376]; harmless so far,
  cause unknown.
