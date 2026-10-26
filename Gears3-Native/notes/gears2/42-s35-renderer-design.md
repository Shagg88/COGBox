# 42 - S3.5 renderer design (our D3D12 device; Vulkan second)

Status: design, written 2026-10-21 while S3.4 is being built. Nothing here is implemented yet. Decisions it relies on:
D23 (pass test: no unexplained differences), D24 (resource tracking = hooks + page watch), D26 (D3D12 first, untiled),
D28, notes/35 (Stage 2 inventory), notes/36 (S3.5 milestones M1-M6), notes/38-39 (translator rules, binding contract).

## 1. Shape: a shadow renderer first

- **Input:** the existing draw observer (SDK patch 2): `OnDrawShaders` (vertex/pixel `Shader`, microcode) and `OnDraw`
  (full register file, primitive, index buffer) for every draw and every EDRAM copy, plus `OnSwap`. Guest memory is read
  through the runtime's `Memory` (physical addresses), as the Stage 1 check does.
- **Shadow mode** (`--gow2_renderer=shadow`): our renderer draws every draw into its own targets on its own D3D12 device
  while the emulated renderer keeps presenting. After each pass it compares its resolve outputs with the emulator's
  (`--readback_resolve=full` + the buffer dump decoding, notes/35). Nothing the player sees changes.
- **Takeover** per milestone (`--gow2_renderer=ours`, later): our output is presented; the emulated GPU's work for the
  passes we own is skipped. Final state (Stage 6): no emulated GPU in the shipped build; it stays in developer builds as
  the reference.
- The observer runs on the GPU worker thread, so recording our command lists there keeps ordering trivially correct; the
  cost is measured with `--gow2_frame_times` from the first milestone.

## 2. Layers

| Layer | Part | Notes |
|---|---|---|
| 0 | `xenos/ucode`, `xenos/hlsl` (done), shader cache (microcode hash -> DXIL/SPIR-V, on disk) | game-free |
| 0 | EDRAM model: tile ranges (80x16 samples per tile), bases, formats, views | the aliasing rules live here |
| 0 | render-target cache: host textures per (EDRAM base, format, pitch, MSAA); two views of one range (7e3 and its 16-bit blendable view) are one storage with conversions, never Xenia's "drop the higher slot" | trap 3 |
| 0 | resolve engine: EDRAM range -> guest texture memory (tiled 2D), with the exponent bias (2^bias / samples), endian, format conversion; destinations keyed by (address, format, size) | Stage 2 aliasing |
| 0 | clear detection: the fixed shader pair on a 3-vertex rect at half pitch 4x MSAA -> a tile-range clear on every aliasing target | trap 1 |
| 0 | dummy draws: 24 single points that write nothing -> skipped, state kept, counted | trap 2 |
| 0 | texture cache: guest textures from fetch constants (Tiled2D untiling, formats, swizzle, gamma curve), invalidated by the Stage 1 table (D3D hooks + page watch) | |
| 0 | pipeline state from registers: blend, depth/stencil, raster, viewport/scissor, primitive types (rect lists, quad lists) | |
| 0 | backend interface: one API-independent command layer; D3D12 backend first, Vulkan backend second (same binding contract) | P1 |
| 1-3 | game knowledge only where needed (e.g. naming passes in logs) | |

## 3. Milestones (notes/36) and what each needs

| M | Pass | Needs first | Check |
|---|---|---|---|
| M1 | depth prepass + shadow maps | EDRAM model, depth formats (D24S8, D24FS8 20e4), clears, depth resolve | scene depth + shadow resolves match |
| M2 | base pass (7e3) | colour RT 7e3 path, texture cache, blend off/on | HDR resolve matches |
| M3 | lights | 7e3 <-> 16-bit view of one EDRAM range, additive blending | HDR resolve after lights matches |
| M4 | AO + velocity | k_16_16(_FLOAT) half-res targets, previous-frame data | both half-res resolves match |
| M5 | post + UI | bloom chain, tone map, 8888 final, gamma | front buffer matches |
| M6 | full frame | everything | FLIP on all routes, no unexplained differences |

## 4. Order of work

1. Backend skeleton (device, queues, descriptor heaps, upload/readback rings) + shader cache using S3.3 output.
2. EDRAM model + render-target cache + clear detection + depth resolve -> M1 in shadow mode.
3. Texture cache + pipeline state -> M2, then M3 (aliasing), M4, M5.
4. M6 sweep; then the Vulkan backend repeats M1-M6.

Each step ships behind its switch, off by default; every comparison is logged per pass so a failure names the pass.

## 5. Research answers (Brief 10, 2026-10-21 04:28) and what changes

Sources: Xenia Canary f42147a5b, Edge 8fb2772, UnleashedRecomp cf829a9, LostOdysseyRecomp 7dd7f76 (GPL: ideas only).
Labels [V] verified in source, [D] documented in a Xenia comment, [I] inferred.

**EDRAM.** 2,048 tiles of 80x16 samples (10 MiB), 11-bit tile bases wrapping at 2,048 (split ranges that cross the
end), pitch in tiles `ceil((pitch_px << (msaa>=4x)) / 80)` doubled for 64bpp, 4x MSAA doubles width and height in
samples, 2x doubles height; exact sample address formula in Canary `shaders/edram.xesli` (BSD, borrowable); depth tiles
have their two 40-sample halves swapped [V]. 64bpp storage on hardware is unknown [D].

**The aliasing case is simpler than feared.** `k_2_10_10_10_FLOAT_AS_16_16_16_16` is the same 32-bit EDRAM bits as 7e3
(Canary `GetStorageColorFormat` folds it); only the hardware blend unit's precision differs ("16-bit fixed point at half
speed, with full blending"), which Xenia doesn't model [V/D]. So: one storage class, no conversion when switching. The
"drop the higher slot" rule exists because of **Gears of War 1's lighting pass** (4D5307E6), where the shader writes one
of two aliased outputs dynamically - D3D9 leaves an unwritten output untouched, D3D10+ doesn't. Our translator can do
this properly: merge aliased outputs into one target, track which one the taken path wrote, and leave the pixel untouched
(discard / blend trick) if neither was. To check: whether Gears 2's light shaders do the same.

**The reference is approximate.** The SDK (like Canary) defaults to the RTV path: 7e3 stored as fp16 (different range,
2-bit alpha lost), 16-bit fixed formats divided by 32 (`snorm16_render_target_full_range`), gamma RTs stored as linear
unorm16. ROV ("highest accuracy", slower) exists as `--render_target_path_d3d12=rov` in our SDK too. Rule: when a
shadow-renderer difference is in 7e3, 16-bit fixed or blending precision, re-check against the ROV path before calling it
our bug (D23: the emulator is a reference, not the truth).

**Depth.** Host D32 (Xenia maps guest 0-1 to host 0-0.5 so every 20e4 value round-trips; converts only on transfers).
Our plan: D32_FLOAT during rendering, convert to 20e4/24-bit only at depth resolves (AO reads depth) and
reinterpretations. More important than 20e4: depth equality between the prepass and the base pass needs bit-identical
clip-space positions from different vertex shaders -> mark position maths `precise` in the translator (S3.3 follow-up).
Depth bias: slope = scale/16 (x resolution scale), constant D24S8 `ceil(|o|*(2^24-1))`, D24FS8 `ceil(|o|*2^21)<<3`.

**Resolves.** Rectangle from vertex fetch constant 0 (3 vertices, byte-swapped, +0.5 for kD3DZero pixel centres,
16.8 fixed-point top-left rounding), window offset, scissor clamp, then expanded to 8x8 alignment. Destination base
below 4 KB folds into x/y in 32x32 macro tiles. Conversion order: unpack samples (7e3 2-bit alpha) -> PWL degamma for
8888_GAMMA before averaging -> sample select / average (depth never averaged) -> x 2^exp_bias -> copy_dest_swap (R/B)
-> pack (2_10_10_10 destinations always fixed point) -> endian. Clear-on-resolve uses the same 8-aligned rectangle.
Partial copies keep the rest of the destination. Ours: host RT -> host texture **region to region**, guest memory
written back only if the CPU reads it (the Stage 1 write watch tells us).

**Rasterisation.** Pixel centre: only `PA_SU_VTX_CNTL.pix_center` (+0.5 for kD3DZero); viewport per `PA_CL_VTE_CNTL`
enables, window offset, 1/W and pre-divided formats, z remap when `dx_clip_space_def` is clear. Rect lists: 4th vertex
mirrored across the longest edge, attributes `v4 = -v[a] + v[b] + v[c]`; quad lists as strip 0,1,3,2; Canary drops
`kPolygon`. To add to the census: primitive types of ordinary draws.

**Textures.** DXT1/3/5 -> BC1/2/3, DXN -> BC5, DXT5A -> BC4, DXT3A -> R8, CTX1 -> R8G8, 8888 -> RGBA8 (+SNORM view),
2_10_10_10 -> R10G10B10A2_UNORM, 16_16_FLOAT -> R16G16_FLOAT. Untiling: `texture_address.h`, `shaders/texture_load_*`
(BSD). Packed mips from min dimension <= 16. Gamma textures: Xenia converts after filtering because host samplers
force it; we can decode the PWL curve at upload into a linear format so filtering happens in linear space (one-time
cost).

**Prior art without full EDRAM emulation.** Unleashed maps engine surfaces to host textures (hooks, per-game aliasing
fixes; loses partial/biased resolves, 7e3, 20e4). LostOdyssey keys host RTs by (base, format class, pitch) with
transfers between 32-bit classes. Both GPL - ideas only.

**Design decision taken from this (D29):** a hybrid scoped to Gears 2 - host render targets keyed by (EDRAM base,
storage class, pitch, MSAA); 7e3 and its 16-bit view share one storage class; depth D32; transfers only for the
reinterpretations the Stage 2 breakdown shows Gears doing, anything else logged as an unknown alias; resolves region to
region on the GPU with Canary's conversion order; Canary BSD code borrowed with notice (edram.xesli addressing,
resolve.xesli conversion, texture_address/texture_load untiling, rect-list expansion).

## 6. Open questions (no hardware to settle them, D30)

Rules Xenia itself marks as unknown or guessed. Each is implemented the way Canary does it unless a better reason turns
up; where our output and Xenia's differ on one of these, real-console footage of the same scene decides.

| Question | Canary's choice | Source |
|---|---|---|
| Blend precision of `2_10_10_10_FLOAT_AS_16_16_16_16` ("16-bit fixed point, full blending") | not modelled (host fp16 blend) | xenos.h, Doggett talk |
| How 64bpp formats are stored in EDRAM | 40x16 64-bit samples per tile, "not known" | edram.xesli:72-77 |
| Gamma textures: degamma before or after filtering | after (host sampler limits) | dxbc fetch :2331-2333 |
| `max4` comparison order (`>` with X,Y,Z,W priority vs `>=`) | FIXME | dxbc alu |
| Texture fetch offsets at mips > 0 | applied at mip 0 | dxbc fetch :766 FIXME |
| 21-bit rounding of exp/log/rcp/rsq | rounds (Ace Combat 6 change, Aug 2026) | 3a44f20c7 |
| Quad-list split into triangles | strip order 0,1,3,2, "unknown" | primitive_processor TODO |
| 16-bit float format for textures and fast-path render targets (extended range or IEEE) | IEEE on those paths | Brief 9 |
