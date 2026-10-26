# 66 - Canary / Edge inventory: textures and guest memory (Shagg 09:32 "do everything you recommend")

Compared: Xenia Canary e6cd2e4 (snapshot), Xenia Edge 5045c19 (2026-10-23), with our layer 0 at 8e50f40.
Scope here: texture cache, texture loading / formats / samplers, shared memory (guest memory on the GPU), resolve
readback, memexport visibility. Render targets / EDRAM / resolve rules: the Gears 3 chat's half (to be merged).
Marks: [V] read in both codebases, [G] guess. "Bug" = which known bug it may touch.

## A. Items that may be the known bugs (do first)

| # | Canary / Edge | Ours | Bug |
|---|---|---|---|
| T1 | Base address 0 with a mip address = texture streaming: kept, sampled with min LOD 1 (texture_util.cc GetSubresourcesFromFetchConstant) [V] | refused: "no base address" -> stand-in texture (texture_cache.cpp) [V] | NOT the Gears 3 squad: ch22 090 logs show stand-ins 0 in every window [V, Gears 3 09:46]; still a correctness gap |
| E1 | Edge `readback_resolve` = resolve read-watch: output stays on the GPU; a read watch on the destination copies it into guest RAM only when the game's CPU first touches it (command_processor_resolve_readwatch.inc) [V] | 0020 none / fast / full; full = copy + wait every resolve, 13.7 ms/frame [V] | Gears 3 rainbow (fixed by full in 089) if the reader is the game CPU [G]. Needs SDK read callbacks (rexglue xmemory.h: "data providers" TODO; Canary memory.cc has them) |
| E3 | Edge memexport: pixel-shader exports awaited at fence / interrupt / coherency (default on), host buffer = guest RAM in place [V] | 0007/0012 readback of export pages [V] | Gears 3 NPC vertex explosions [G] |
| M3 | RequestRange before every GPU write and texture load: uploads only pages not valid (a valid bit per page until a CPU write) [V] | prepare_mirror (0020) off by default; when on it re-uploads every time: 6659 MB per 300 frames in 311 [V] | partial-page resolves leave stale bytes next to them [G] |
| M7 | vertex / index fetch from the GPU copy of memory, always [V] | guest CPU memory; mirror only for memexport pages, only while exports_marked_ [V] | data a resolve wrote, used as vertices [G] |

## B. Correctness gaps (port, each behind a switch where it changes pictures)

- T2 base == mip address with mip_min != 0: base dropped. Ours: no rule.
- T3 packed tail at level 0 with mip address 0: mip = base. Ours: mip_start = (packed level 0 && mip_base) ? base : mip_base - recheck.
- T4 mip min/max levels clamped to log2(size). Ours: max level from the fetch constant.
- T5 BC textures not 4-aligned: Canary uses UnalignedBlockTexturesSupported or decompresses (DXT -> RGBA8 load shaders). Ours: refused.
- T6 mixed signed / unsigned components: per-component signs + a separate signed host texture. Ours: signed view for all (0024 tex_sign_by_swizzle opt-in).
- T7 integer scale bits: fixed-format precision rules, Q16 rounding of normalized unsigned, texel centres for point-sampled 2D (fixes seams). Ours: none; Xenia's old 1.5/1024 epsilon.
- T8 zero-address texture with a constant swizzle (Bink dummy alpha): kept. Ours: refused.
- T9 1D textures wider than 8192: mapped to 2D. Ours: not handled.
- T14 per-instruction filter override (tfetch mag / min / mip / aniso / volume): used. Ours: ignored (hlsl.cpp Simplify tex_filter_override).
- T15 kBaseMap: MaxLOD = MinLOD + 0.25 without anisotropy (keeps magnify vs minify). Ours: max = min.
- T16 border colours: 4 kinds + force_bc_w_to_max. Ours: 3, no force.
- T17 / E7 host formats the device can't filter: point sampling instead. Ours: not checked.
- T19 formats ours lacks: k_1_REVERSE, k_1, k_6_5_5, the two 4:2:2 YUV formats, k_32 integer variants, the AS_ variants, k_DXT3A_AS_1_1_1_1 -> stand-in.
- T20 16-bit unorm / snorm loaded as float for the full range. Ours: R16 unorm / snorm directly.
- Other shader simplifications in ours: getGradients, getBCF, getWeights (cube), LOD query in VS, stacked-as-3D (hlsl.cpp Simplify list).
- E4 a CPU write into any page of a scaled resolve unmarks the whole resolve (Edge). Ours: per page.
- E6 scaled resolve textures with mips: Edge generates the mips. Ours: falls back to unscaled.

## C. Speed and robustness tricks

- T21 every format loads on the GPU (one load shader per format). Ours: BC, 32bpp cubes, 3D on the GPU (0021/0025/0026, opt-in); the rest untiled on the CPU - the 30-40 ms "textures" hitches in 330 [G].
- T10 texture memory limits: soft 384 MB unused 30 s, hard 768 MB, + 24 MB x scale. Ours: one cap, evict to 90%.
- E4 only USED outdated bindings are re-checked (Edge); textures bound twice load once (Canary); lockless all-up-to-date pre-check.
- E5 RequestRange lockless all-valid fast path; dirty-chunk copy of the valid flags (Edge).
- E4 unwatch destroyed textures under the global lock (Edge aa94c6a, a crash fix).
- E2 Edge `enable_host_buffer`: guest RAM imported as a second D3D12 buffer (separate view of the physical mapping). Zero-copy (all of shared memory = guest RAM) is default only on ARM64 - slow over PCIe.

## D. Same in both [V]

- 3D tiling (TiledOffset3D) = Canary texture_address.h Tiled3D; 2D tiling, packed mip offsets, guest layout (ported).

## E. Licence

Canary and Edge are BSD-3-Clause (Copyright (c) 2015 Ben Vanik). Files with ported code carry the notice; files
that only describe Canary behaviour don't need it. THIRD_PARTY_NOTICES.md gets a Xenia Canary / Edge row.
