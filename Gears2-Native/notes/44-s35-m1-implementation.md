# 44 - S3.5 steps 1-2: our renderer's first code, aiming at M1 in shadow mode

Status: written 2026-10-21. Step 1 (upl-n096a) built and ran on the PC in jobs 095/096. Step 2 (upl-n096) has only been
syntax-checked in the container. Nothing here has drawn a frame yet. Plan: notes/42 section 4 items 1-2. Decisions it
relies on: D23, D26, D29, D30.

## 1. What it is

This is our own D3D12 renderer. It lives in layer 0, in `src/layer0_xenos/renderer/`, with headers in
`include/xenos/renderer/`. It runs in **shadow mode**:
- It sees every draw through the draw observer and draws it on its own D3D12 device, into its own targets.
- The emulator keeps rendering and presenting, so the player sees no change.
- After each depth resolve, it compares its result with the bytes the emulator wrote, and logs the result.
- It is off by default.

| Part | Files | What it does |
|---|---|---|
| Command layer | `gpu.h` | An interface that doesn't depend on the graphics API: textures, shaders, pipelines, guest-memory upload, draws, compute, readback, submit and fences. It follows the translator's binding contract (notes/39). The Vulkan backend will implement the same interface later. Its types live in `xenos::renderer::gpu`, so they don't clash with the names in pipeline_state.h. |
| D3D12 backend | `d3d12_backend.cpp` | See "The D3D12 backend" below. |
| DXC | `dxc_compiler.cpp` | Loads `dxcompiler.dll` from beside gow2.exe at run time (POST_BUILD copies it and `dxil.dll` there from `third_party\dxc-1.9.2607`). Uses the pinned arguments `-HV 2021 -Gis -denorm ftz`, SM 6.6. There is no import library: if the DLL is missing, the renderer logs it and only shaders already in the disk cache work. |
| Shader cache | `shader_cache.cpp` | Turns microcode into decoded HLSL (our S3.2/S3.3 code) and then into DXIL. See "The shader cache" below. |
| EDRAM model + render-target cache | `render_target_cache.cpp` | See "The EDRAM model and render-target cache" below. |
| Register state | `pipeline_state.h` (upl-n097, by the second helper) | Depth/stencil, rasteriser, viewport/scissor, blend, alpha test, topology and skip rules, taken from Canary. |
| Shadow renderer | `shadow_renderer.cpp`, `builtin_shaders.h` | The draw observer: what happens to each draw, clear, dummy point and resolve (section 2). |

**The D3D12 backend:**
- It creates its own device, queue and fence, a pool of command allocators, a shader-visible view heap and a sampler heap, CPU heaps, a 256 MB fence-tracked upload ring, readback buffers, a 512 MB GPU copy of guest physical memory (t96), and a pipeline cache.
- It never touches the SDK's device.
- It never calls `EnableDebugLayer`: doing that after the SDK's device exists would remove that device. `--gow2_renderer_debug` only forwards messages when the SDK's own `--d3d12_debug` already turned the debug layer on.

**The shader cache:**
- It keeps every shader in memory, keyed by the microcode hash.
- On disk it stores them under `out\shader-cache` as `<vs|ps>-<hash>-<key>.dxil`. The key hashes the HLSL, the compiler version and the compiler arguments, so a new translator or compiler simply misses the old files.
- Translation runs on the GPU worker thread and takes about 0.1 ms. DXC runs on a pool of worker threads: by default the number of logical processors minus 2, set with `--gow2_shader_threads`. Each worker has its own compiler.
- A draw whose shader is still compiling is skipped, never waited for.

**The EDRAM model and render-target cache:**
- Host render targets are keyed by (EDRAM base, storage class, pitch, MSAA), as D29 decided.
- 7e3 and its 16-bit blendable view are one storage class.
- Every depth target is D32_FLOAT + S8.
- For each of the 2,048 tiles, it remembers which target wrote it last. When a draw reuses tiles that another target wrote last, that is reported as an **alias**.

## 2. What happens to each draw (M1)

| Draw | Treatment |
|---|---|
| 24/48-point no-op groups (point list, one vertex, VS B6C9863F710683EC) | Skipped and counted as "dummy points". |
| Clear: VS 1E6883FCCDE1F688 + PS A4A965C189287B99, rectangle list, 4x MSAA | See "Clears" below. |
| Tessellated patch (0x10-0x12 with explicit major mode and `path_select` = tessellation) | Skipped and counted as "patches". |
| Canary's skip rules (copy mode, pitch 0, no rasterisation, both faces culled, unsupported primitive such as lines) | Skipped and counted by reason. |
| Writes neither depth nor stencil | Not drawn (colour is stubbed), counted as "colour-only". The colour targets' tile ownership is still recorded. |
| Writes depth or stencil | Drawn. See "Depth draws" below. |
| Shader still compiling | Skipped and counted as "pending shaders". The resolves after it say INCOMPLETE. |
| Shader failed to translate or compile | Skipped. A `gow2_renderer: SHADER-FAIL <file>: <message>` line is logged once per shader, and the source is kept beside the cache as `.failed.hlsl`. |

**Clears.** The clear is drawn at 2x resolution scale onto the 1x target with the same base and twice the pitch, which
covers the same EDRAM tiles (notes/35). It is then drawn again onto every other existing target of the same storage
class and tile pitch whose tiles overlap. A colour-only clear is just counted.

**Depth draws:**
- The vertex shader always runs. The pixel shader runs only if it kills, writes depth, or alpha test or alpha-to-mask is on. Otherwise no pixel shader is bound.
- Vertex buffers are uploaded from guest memory into the GPU copy of guest memory. Index buffers are byte-swapped on the CPU, and the guest reset index becomes 0xFFFFFFFF for strips.
- Fans become lists. Quad lists become triangles in Canary's geometry-shader order (0 1 3)(1 2 3).
- Rectangle lists go through our geometry shader: Canary's longest-edge rule, 4th vertex = -a + b + c.
- The constants follow notes/39: b0 system constants (W/XY/Z flags, polygonal, alpha test, index offset, interpolator mask, ps_param_gen), b1 = registers 0x4000/0x4400, b2 = 0x4900, b3 = 0x4800.

**Resolves.**
- The rectangle and destination come from `xenos/resolve.h` (Canary GetResolveInfo).
- A depth resolve runs a compute shader. It turns our depth + stencil into the guest word, 24-bit depth << 8 | stencil:
  - D24FS8 uses 20e4 with truncation, Canary's default.
  - D24S8 uses unorm24 with round-to-nearest-even.
- The words are written into an R32_UINT host texture keyed by (address, format, size): region to region, as D29 decided.
- Guest memory is **not** written: only the emulator writes it, because this is shadow mode.
- Clear-on-resolve is applied for depth. Colour resolves are counted only.

**Comparison.** It happens on every Nth frame (`--gow2_renderer_compare_every`, default 300):
1. Our region is read back.
2. At the next observer callback, the emulator's destination bytes are copied out of guest memory. The emulator has executed the copy by then, and `--readback_resolve=full` writes it to memory synchronously.
3. Each pixel's word is compared at its tiled address (Tiled2D, endian swap applied), so the comparison is byte for byte.

## 3. How to run

Build with the snippet in `cmake-snippet.txt` and the two edits in `EDITS.md`, then run the game with:

```
--gow2_renderer=shadow --readback_resolve=full
```

Optional switches:
- `--gow2_renderer_compare_every=300`
- `--gow2_renderer_log_every=300`
- `--gow2_shader_threads=N`
- `--gow2_frame_times=300`: the hub's existing timing. It already includes our callbacks' cost.
- `--gow2_renderer_debug`: needs `--d3d12_debug`.

The first run compiles shaders in the background. Early frames therefore have "pending shaders" and INCOMPLETE
resolves. The second run loads everything from the disk cache.

## 4. Log lines (all start with `gow2_renderer:`)

| Line | Meaning |
|---|---|
| `shadow mode on (M1: depth passes): D3D12 <GPU>... \| DXC ... N compile thread(s) \| cache <dir> \| rect-list GS ok \| depth resolve ok \| compare every N frames ...` | Start-up. "MISSING" means a built-in shader failed. Look for SHADER-FAIL. |
| `new render target #id D24FS8 base B pitch P msaa 1x (WxH host)` | Our render-target cache created a target. "(bookkeeping only)" = a colour target that is tracked but not drawn in M1. |
| `resolve frame F copy K (draws A-B, N drawn by us): depth <fmt> base .. pitch .. WxH at (x,y) -> <dest> fmt <f>: bytes equal E/T (p%), pixels equal ..., depth max diff D (24-bit units) at (x,y), stencil diffs S -> VERDICT` | One depth resolve. K counts the copies in the frame, so it is the pass index. Draws A-B are the draws since the previous copy. See the verdicts below. |
| `... NOT COMPARED (<why>)` | The resolve couldn't be compared. Reasons: we never drew into that target, MSAA source, destination format, and so on. |
| `alias: frame F draw D: <target> reuses N tile(s) last written by <target> - no transfer` | A depth draw lands on tiles a different target (key) wrote last. Its old content is not converted, as D29 decided. At most `--gow2_renderer_log_aliases` distinct pairs are logged. |
| `SHADER-FAIL <file>: <first line>` | Per-shader failure, logged once. |
| `frames A-B: draws .. drawn .. (clears .. + .. repeated on aliases, colour clears ..) \| dummy points \| patches \| colour-only (stubbed) \| texture-dependent kills/alpha \| aliases \| pending shaders \| not drawn: shader/pipeline/other \| copies (depth, colour) \| resolves compared/matched/incomplete \| vertex/index MB \| shaders translated/disk hits/compiled/queued/in flight/rejected \| targets \| our CPU ms/frame \| GPU submits \| skipped N: <reason>...` | Summary every `--gow2_renderer_log_every` frames. "our CPU" is our own time inside the observer per frame. |

The verdicts on a resolve line:
- **MATCH**: every word is equal.
- **DIFF**: some words differ. The max difference is in 24-bit depth units, and stencil is counted separately.
- **INCOMPLETE (N draws skipped: ...)**: some depth draws of this frame weren't drawn because a shader was still pending or had an error, so a difference is expected.
- **NO REFERENCE / NO DATA**: the snapshot or the readback is missing.

## 5. What is stubbed or missing (on purpose, for M1)

**Stubbed:**
- **Colour.** Colour targets are tracked for tile ownership but never drawn or resolved. Colour-only draws are skipped.
- **Textures.** Every texture slot reads a 1x1 opaque white texture, and the sampler is linear/wrap. A pixel shader that kills or alpha-tests using a texture therefore keeps all its pixels. These draws are counted as "texture-dependent kills/alpha". Masked foliage in the prepass or shadow maps will show up as DIFF until the texture cache (M2) exists.

**Not done yet:**
- **Tessellation.** Patches are skipped. If chapter 18/20's tessellated draws write depth, those passes will differ.
- **Transfers between EDRAM keys.** Aliases are only logged.
- **MSAA depth resolves.** Resolves from MSAA depth targets are not compared. Gears uses none except the clear trick, which never resolves.
- **Lines** (Unsupported in pipeline_state.h; the census found none).
- **Memexport**: those draws are skipped.
- **Vertex index min/max clamp**: not applied.
- **Guest-memory write-back of our resolves**: not needed in shadow mode.

## 6. Risks and what is guessed

| Item | Status |
|---|---|
| D3D12 calls, barriers, root signature | Syntax-checked against DirectX-Headers. Step 1 parts ran on the PC (self-test). The draw and resolve paths have **never run**: a device removal or a debug-layer error on the first real draw is possible. Run once with `--d3d12_debug --gow2_renderer_debug` first. |
| Big vertex buffers (256 KB or more) uploaded once per frame | Guess: buffers that big aren't rewritten between draws of one frame. Small buffers are uploaded on every draw. If a mid-frame rewrite matters, the symptom is a DIFF in one pass with no shader or alias reason. |
| D24S8 rounding | Guess. The emulator's host format for D24S8 may be D24_UNORM, while ours is D32_FLOAT rounded at the resolve. Expect differences of at most 1 unit there, not exact equality. D24FS8 (scene depth) uses the same truncation as Canary. Canary stores 20e4 depth halved (0-0.5), which may cost ±1 unit in rare pixels. |
| Clear = draw at 2x scale onto the full-pitch target | Inferred from notes/35 (same tile pitch). It is exact only if the clear value is uniform, which a clear's is. |
| Interpolator mask | Follows Xenia's rule, using our decoder's register bound. |
| Shader compile threads | Worker threads never touch the backend. Only the GPU worker thread creates D3D12 objects. |
| Shadow mode's own GPU cost | Unmeasured. We wait for our GPU at most two frames behind. |

**Verified:**
- Everything compiles with clang 18 `-fsyntax-only -Wall -Wextra` (0 warnings) against the SDK and DirectX-Headers.
- The built-in geometry and compute shaders compile with DXC 1.9.2607 in the container.
- Step 1 ran on the PC (jobs 095/096): 276 shaders, 0 failed, disk-cache hits on the second run, self-test ok.

**Not verified:** any step-2 behaviour.

## 7. Next

1. A PC run of step 2 on chapter 2 with `--readback_resolve=full`. Read the resolve lines for copy 0 (scene depth) and copies 1-7 (shadow maps).
2. Fix what differs, pass by pass.
3. M2: the texture cache and the colour path.

## 8. Changes after job 097 (upl-n098)

Job 097 was the first step-2 run, on chapter 2. Scene depth (copy 0) was a byte-exact MATCH at frame 300. At frames
600, 900 and 1800 it differed on 0.1-0.25% of pixels. The shadow maps differed by exactly 1 unit on 1-2% of pixels.
Copy 1 differed on 9% of pixels, with 93k stencil differences and only 7 of its 328 draws drawn by us.

- **D24S8 is stored as D24_UNORM_S8_UINT**, the format the reference uses.
  - Canary's EDRAM dump turns it into the 24-bit value with round-to-nearest-even of d × 0xFFFFFF. Our resolve does the
    same, so the GPU's own depth quantisation now matches the reference.
  - The pipeline_state.h integer depth bias for D24S8 is in unorm24 units. It was too small by a large factor on a D32
    target. That was probably part of the shadow-map differences, not only the ±1 rounding.
  - `--gow2_renderer_d24s8_exact=true` keeps the old D32 + rounding variant. That variant has the depth-bias caveat.
- **Coverage mask on comparison frames**, to explain differences.
  - Each depth target gets an R8 mask, cleared to 0 at its first use in the frame.
  - Every drawn draw writes the blend constant into it: 128 for a draw with real inputs, 255 for a draw whose kill or
    alpha test read our white stand-in textures. Depth-only draws get a stand-in pixel shader so they write the mask
    too.
  - DIFF lines now say:
    - `DIFF (unexplained N px, max diff M at (x,y); texture stand-in T px, not written this frame S px)`, or
    - `DIFF (all explained; ...)`.
  - "Not written this frame" means stale EDRAM from other targets, which we don't transfer (D29). This is the expected
    explanation for copy 1: its 768x480 rectangle is larger than what the clear and the 7 shadow draws cover, and its
    tiles were last written by the half-res targets at base 1440 pitch 640. To be confirmed on the PC.
  - The summary line counts "explained diffs".
- **Each resolve line also lists what was not drawn since the previous copy, by reason.** This answers why copy 1 had
  7 of 328 draws drawn. The code already draws every draw that writes depth or stencil, with colour writes on or off
  (no colour target bound). Draws that write neither are listed as "colour-only".
- **Buffers are created in COMMON**: this fixes the 5 debug-layer warnings. The guest-memory buffer starts each command
  list in COMMON, because buffers decay to COMMON after ExecuteCommandLists.
- **The round-3 translator** (upl-n095 hlsl.cpp, md5 d484c0b3) is bundled unchanged. Its binding contract is the same.

Still open (next drop, upl-n099):
- The M2 texture cache.
- The per-frame upload cost: job 097 measured about 28 MB of vertex data and 3.8 MB of indices per frame, and our
  observer at about 14 ms/frame of the GPU worker thread. Planned fix: key uploads by guest range plus the Stage 1
  page-write watch, so unchanged data is not uploaded again, and log upload MB and CPU ms per pass.

## 9. Draw-ID diagnostics (upl-n099a)

**The n098 coverage mask was wrong.** It wrote (shader output × blend constant) instead of the constant. Depth-only
draws, which get our stand-in pixel shader whose output is 0, were therefore marked "not written this frame". The job
098 classification, "all explained" for copies 1 and 5-8, is withdrawn.

**What replaces it.** On comparison frames each depth target gets an R32_FLOAT draw-ID target, with no blending.
- The pixel shader writes `(record index + 1) × 2 + (1 if the draw read a stand-in texture)` to colour 0. This value is
  exact as a float.
- Translated pixel shaders do this through one line the shader cache inserts before their final return:
  `if (xe_sys1.w & 1) o.color0 = asfloat(xe_sys1.z)`. hlsl.cpp itself is unchanged. On other frames
  `xe_sys1.w = 0`, so the shader is unaffected; colour 0 isn't used in M1.
- Depth-only draws use the stand-in pixel shader, which writes the same value.

**Per differing pixel**, the ID says whether it was:
- not written this frame;
- last written by a draw that read stand-in textures;
- last written by a draw with real inputs ("unexplained").

For the 6 unexplained pixels with the largest difference, a line follows the resolve line:
`unexplained px (x,y): ours XXXXXXXX guest XXXXXXXX (depth diff D) - last written by our draw N (vs .. ps .. prim P
depth func F write W bias B slope S target #T)`.

**Each resolve line also gives this pass's cost:** `this pass: uploads vertex X MB (Y MB unchanged, skipped) index Z MB
textures T MB, our CPU W ms`.

**Also in this drop, switched off by default in n099a and on by default in n099:**
- **`--gow2_renderer_textures`**: the texture cache.
- **`--gow2_renderer_upload_watch`**: vertex ranges of 64 KB or more are copied only when the guest wrote them since.
  This uses GuestWatch:
  - It registers a guest-memory invalidation callback and asks the memory system to watch the ranges we copied
    (`Memory::EnablePhysicalMemoryAccessCallbacks`, the same mechanism the emulator's shared memory uses).
  - Resolve destinations are marked as written.

Constant buffers whose data didn't change since the previous draw are no longer uploaded again. This is on in both
drops.

## 10. upl-n099: textures, transfers, upload savings, clear stencil, precise positions

Everything below is on by default, and each item has its own switch.

**Texture cache (`--gow2_renderer_textures`).**
- Guest textures come from the fetch constants: 1D, 2D, stacked, cube and 3D, with all mips and packed mip tails.
- Tiled2D/3D or linear layout goes through `texture_layout.h`. The fetch constant's endian swap is applied.
- Host formats follow Canary's table:
  - DXT1/3/5 become BC1/2/3, DXN becomes BC5 and DXT5A becomes BC4.
  - DXT3A becomes R8 and CTX1 becomes R8G8, both decoded on the CPU.
  - The 16-, 32-bit, float and 8888/2_10_10_10 formats are copied, and 565/1555/4444 have red and blue swapped.
  - 10:11:11 formats become RGBA16; 24:8 depth becomes R32_FLOAT.
- The fetch swizzle is composed with the format's channel map in the view, and signed components get an SNORM view.
- Samplers come from the fetch constant: filters, anisotropy, clamp modes, border colour, LOD bias, min/max mip.
- Gamma, the bias and exp_adjust stay in the translated shader.
- A texture is re-uploaded when GuestWatch says its guest bytes changed. There is no eviction yet: the host memory
  total is in the summary.
- **Not done yet:** BC textures whose size isn't a multiple of 4 would need decompression, so they get a stand-in. So
  do formats not in the table, and mixed signed/unsigned components (which get a signed view). Every stand-in reason
  is counted; see the log lines below.

**EDRAM transfers (`--gow2_renderer_transfers`).** Before a depth target is drawn or resolved, every tile another
target wrote last is handled one of three ways:
- **From a depth target (1x):** copied by a transfer pixel shader.
  - Each destination pixel looks up its EDRAM dword (1x, 32bpp, depth halves swapped, 2,048 tiles wrapping).
  - It finds the source pixel holding that dword and rebuilds the source's guest word (24-bit depth << 8 | stencil).
  - It writes that word back in the destination format: depth through SV_Depth, and stencil in 8 one-bit passes, so
    SV_StencilRef isn't needed.
- **From a colour target whose last write was a clear with a known constant colour:** filled with that value.
  - "Known" means the clear's pixel shader is just oC0 = c[k], and the target is 8888, 2_10_10_10, 16_16_FLOAT,
    32_FLOAT, or zero in any format.
- **Anything else:** counted as "unknown source".

Clears now skip the transfer only when they write depth AND all 8 stencil bits. Job 099 showed a depth-only clear
keeping old stencil (FFFFFF00 vs FFFFFF01 on copies 1 and 7). On comparison frames, transferred pixels get draw ID 1,
so a pixel that still differs is reported as "transferred but different".

**Upload savings (`--gow2_renderer_upload_watch`).**
- **Vertex ranges of 16 KB or more** (was 64 KB) are copied only when GuestWatch says they were written.
- **Converted index buffers of 16 KB or more** are kept between draws with the same check, which saves the CPU
  conversion.
- **Self-check:** on comparison frames every skipped range is re-hashed (XXH3) and compared with its hash at copy time.
  The result is logged as
  `frame F upload-watch self-check: N skipped vertex/index ranges re-hashed, stale S (X MB)`; S > 0 means the watch
  missed writes.
- **Texture-cache keys** are now integers (it used to format a string per slot per draw).

**Precise positions.** Vertex shaders get `precise` on the register file and on the position output: a text
post-process in the shader cache, so hlsl.cpp is unchanged. This keeps clip-space positions bit-identical between
shaders that draw the same geometry (notes/42 "Depth"). It was checked with DXC on a small test shader, and the output
has no fused multiply-adds.

**Unexplained-pixel lines now also give:** whether the vertex shader uses a0/aL-relative registers, whether its
positions are precise, and the vertex-fetch formats it reads.

**New log lines:**
- **The summary** adds:
  - `upload watch: index X MB unchanged, self-check N ranges, STALE S (X MB)`;
  - `alias tiles T transferred C from colour clears U unknown`;
  - `textures N (M MB host) uploads X MB, stand-ins S`.
- `texture stand-ins by reason: ...` lists the stand-in reasons since the last summary.
- **Each resolve line's "this pass" part** adds the alias tile counts.

## 11. upl-n100: tile ownership by rectangle, raw unorm24 decode, colour-source and rule labels

**What went wrong in job 100.** Tile ownership was marked too widely. Every depth or colour write claimed whole tile rows
across the full pitch, from the target's base down to the scissor bottom. A scissor of 8192 (the "unlimited" value) was
cut only by the target height, which is all of EDRAM from the base. So one scene draw claimed all 2,048 tiles,
including the shadow map's. The shadow map's next draw then "transferred" the scene's depth over its own content,
although the reference had kept it. That gave the "3376 transferred + 720 unknown" per copy and the 0xFFFFE-unit
differences on row 0.

**Ownership is now by rectangle** (D23: on the hardware a draw changes only the samples it covers).
- **The rectangle** is scissor ∩ viewport. A clip-disabled draw has no viewport bound, so its rectangle is the scissor
  alone; those draws are counted in the summary as `unclipped depth draws bounded by scissor only`.
- **Tile mapping** is done per target from edram.h: tile = base + row × pitch_tiles + column (mod 2048), with each
  target's own pitch_tiles (64bpp doubled). x is ×2 at 4x MSAA and ×2 again at 64bpp (80 samples per tile), and
  y is ×2 at 2x/4x (16 sample rows). Every tile the rectangle touches counts, rounded outwards.
- **Who takes ownership.** Only draws that write depth or stencil take the depth target. Colour targets are taken
  when they are written.
- **Primitive bounds are not used.** They need the vertex shader's output, which we don't have before the draw.
- **Resolves only read.** The tiles they read are transferred into our texture first, but ownership doesn't change.
- **Resolve clears** (depth_clear_enable) take the rectangle's tiles. A tile the clear covers only in part first
  receives its old content.
- **Clears that replace depth and all 8 stencil bits** skip the transfer only for tiles they cover completely.

**Two ownership books, with a switch.** `--gow2_renderer_ownership=rect` (the default) acts on the rule above.
`--gow2_renderer_ownership=canary` acts on the reference's estimate. That estimate comes from Canary e6cd2e4
render_target_cache.cc Update and draw_extent_estimator.cc:
- It takes whole tile rows from the base down to min(scissor bottom, viewport bottom); the viewport bottom applies only
  to clipped draws.
- Each bound target's range ends at the next bound target's base.
- Depth counts as bound whenever the depth or stencil test is on, even for draws that don't write.
- Canary runs the vertex shader on the CPU for clip-disabled draws with an unlimited scissor; we use the scissor
  there.

The other rule is tracked in a second book. On comparison frames, tiles where the two books disagree (on whether old
content comes in, or from which target) are labelled. Their differing pixels are reported as `reference approximation
(Canary ownership heuristic)`, not as unexplained. The summary gives `rules disagree N` (tiles, comparison frames).

**Raw bits in transfers (checked against Canary ~3560-3590, ~4140-4235).**
- The transfer shader rebuilds the 24-bit guest word from the source: round-to-nearest-even of d × 0xFFFFFF for D24S8
  (now `precise`), or 20e4 truncated for D24FS8. Stencil goes into bits 0-7.
- It decodes the word in the destination format. A D24S8 destination now uses (n + (n >> 23)) × 2^-24, which is exact
  in float and gives n back from D24_UNORM; it used to be n / 16777215. D24FS8 uses 20e4 → float32.
- Known colour-clear values use the same decode on the raw word (depth24 = bits >> 8, stencil = low byte).
- The alias mapping (dest pixel → tile → offset from the source base → source pixel, with the 40-sample swap of depth
  tiles) was already as Canary ~3130-3330, for 1x only.
- Canary's "keep the full-precision host copy" step is a host-side fix, not hardware behaviour, so it is not used.

**Tile origin (comparison frames).** Each depth target has a second R32_FLOAT diagnostic texture saying where a
tile's content came from at its last ownership change:
- 1 = transferred;
- 2 = known colour clear;
- 3 = unknown, from a colour target;
- 4 = unknown, other source (MSAA, or transfers switched off);
- 5 = the two ownership rules disagree.

A differing pixel is put down to the origin when the last writer did not write the bits that differ (for example a
depth-only clear and a stencil difference, as in copy 1), or when nothing wrote the pixel afterwards. The verdict then
adds:
- `unknown (colour source, needs M2 colour bits)`;
- `unknown (other unmodelled source)`;
- `reference approximation (...)`.

None of these count as unexplained. The real fix for colour sources is M2: colour targets must keep their EDRAM bits so
that a colour → depth transfer can take depth24 = word >> 8 and stencil = word & 0xFF, with the column swap.

**Still open.**
- Copy 0's 4 scene-depth pixels (parked: formats 37/57 are passed through, exp_adjust only).
- SV_Depth clamps 20e4 values above 1.0 in D24FS8 destinations (none seen so far).
- MSAA transfers.
- CPU cost with transfers on (44 ms/frame in job 100; it should fall now that far fewer tiles transfer).
- Texture upload volume.

## 12. upl-n101: lossless D24FS8 round trips, tile trace, CPU split

**What jobs 102/103 show.**
- In ch02, copies 2-8 differ with transfers on and match with them off, under either ownership rule.
- Every copy's max depth difference is exactly 1048575 = 0x0FFFFF. That is what a D24S8 word at or above 0xF00000
  becomes after a round trip through a D24FS8 target:
  - The raw bits are read as 20e4, giving a value ≥ 1.0.
  - Our host D32 depth is written through SV_Depth, which clamps it to 1.0, stored as 20e4 0xF00000.
  - On the way back, 0xF00000 is read as unorm24.
  - A cleared shadow map (0xFFFFFF) therefore comes back as 0xF00000.
- The per-pass counts point the same way: 1328 transferred + 720 unknown = 2048 is one claim of the whole EDRAM.
  3376 = 2048 + 1328 is that claim plus one more full claim, i.e. another target's unclipped full-scissor access
  (see job 097's list: #4 is D24FS8 at base 1440, the shadow map's base) taking the shadow map's tiles and handing
  them back.

**Root cause: our D24FS8 host format couldn't hold the whole 20e4 range.** The reference (rexglue SDK,
util/draw.cpp:513) runs with full_float24_in_0_to_1 = true whenever host render targets are used. It keeps D24FS8 depth
as guest × 0.5 on the host, and its transfer and resolve shaders use the remap_from_0_to_0_5 forms
(d3d12/render_target_cache.cpp 3006, 3195, 3531, 5356; clear value × 0.5 at 4569). [0, 2) then survives every
reinterpretation, which is also what the hardware does (EDRAM keeps the bits). We now do the same,
`--gow2_renderer_float24_remap` (default on):
- **Draws into D24FS8** use a viewport depth range × 0.5. This is exact; constant and slope depth bias scale with it.
- **Pixel shaders that write depth** get, in the shader cache post-process, oDepth = saturate(x) × 0.5 behind system
  flag 64; the initial value is doubled back. A depth-writing shader without that hook is skipped and counted, not
  drawn wrong.
- **Resolve and transfers.** Resolve, transfer source and transfer destination use × 2 / × 0.5 (shader flag bit 1).
- **Fills and clears.** Known-clear fills and resolve clears use × 0.5.

**About the clear key (coordinator's theory).** The fast clear is already drawn and owned under the redirected 1x key
(pitch × 2, 1x): the draw records of clear draws 989/1059 say target #11, the shadow map. So no 4x texture exists to
transfer from. The trace below will confirm the order of events.

**Tile trace (`--gow2_renderer_tile_trace`, default 1440-1441,1450-1451,1460-1461,1470-1471: ch02 copy 2's rect).**
- **What it logs.** On comparison frames, every event touching those tiles, in order:
  - claims under both rules (previous owner → new owner, or read only);
  - transfers, known-clear fills and refused transfers (with source key and run);
  - draws into the tile (rect, depth and stencil state);
  - resolve reads and resolve clears.
- **Our value after each event.** Each event carries our 32-bit guest word at one pixel of the tile after it
  (`--gow2_renderer_tile_trace_pixel`, default 7,0). It is taken with the resolve shader into a 4096-wide trace
  texture and logged when the readback is ready, as `trace ... -> ours XXXXXXXX`.
- **Draw records.** They now give stencil write, mask, ref, func and pass op, plus the tile origin, so copy 1's stencil
  pixels show why they are, or aren't, put down to the colour source.

**CPU split (first part of the CPU-cost work).**
- **The split line.** A second summary line gives exclusive ms/frame per part: state, ownership, transfers, textures,
  constants, vertex, index, record, resolve, compare, poll, frame, other. It also gives how much of the total was
  blocked on our own GPU (backend fence waits), texture tables reused, state calls skipped and constant uploads
  skipped.
- **Backend changes.** The backend no longer re-records unchanged render targets, viewport, scissor, stencil ref or
  pipeline, and keeps the descriptor table when the same textures and samplers are bound again (rebuilt if one of
  them left the SRV state).
- **Next.** The split from the next run decides the next step: per-range ownership bookkeeping, the alternate rule
  only near comparison frames, and batching.

**Still open:** colour sources (M2, notes/45), copy 0's few scene-depth pixels, MSAA transfers.

## 13. upl-n102: CPU cost, and the stencil of copies 1 and 7

**Copies 1 and 7: where the stencil comes from (job 105 trace).**
- **The chain.** Tile 1440 belongs to colour target #8 (k_16_16_16_16_FLOAT, base 1624) and then passes, by transfer,
  through #2 and #1 to #11:
  - Draw 0, #2's fast clear: claims the whole EDRAM.
  - Draw 193, #1's fast clear: claims the whole EDRAM.
  - Draw 988 (the clear shader pair at 1x, depth only): claims the tile for #11, the shadow map.
- **Who writes stencil.** The shadow map's own clears (988, 989, 1059) don't write stencil, so the shadow map's stencil
  is whatever arrived through that chain.
- **Our bug.** Both fast clears write depth and stencil (mask FF, ref 00), so we skipped the transfer, treating the
  clear as replacing the whole claimed area. That test used scissor ∩ viewport, and for unclipped clears that is
  unbounded. The real clear geometry covers only the visible area, and the reference transfers the old content below
  it.
- **Fix: a draw never skips its pre-draw transfer.** Transferring first is always correct, because the draw then
  overwrites what it really covers. Only resolve clears, whose rect is exact, still skip.
- **Content taint.** The content at the start of that chain is colour bits we don't keep yet. Each tile now carries a
  taint in the acting book: 1 = some of it came through transfers from a colour target, 2 = from another unmodelled
  source.
  - Transfers pass the taint on.
  - A known-clear fill or a complete resolve clear resets it.
  - Tainted tiles get tile origin 3 or 4.
- **Classification.** A differing pixel whose last writer did not write the differing bits is reported as
  `unknown (colour source, needs M2 colour bits)`, even when the last event there was a transfer. The per-pass line
  adds `N carrying colour-derived content`.

**CPU (job 105 split: textures 8-10, transfers 1.4-13, ownership 5, vertex 2-2.4, index 1.5, record 2 ms/frame).**
- **Textures.** Each fetch slot keeps its last result (sampler, views, stand-in count). It is reused while the six
  fetch-constant dwords, the dimensions the shaders read and the guest-watch epoch are unchanged. GuestWatch keeps the
  same version sums per 256 KB and per 4 MB, so a Version over a large texture reads a few counters instead of one per
  4 KB page. That was most likely the cost: a check whenever any callback had arrived, on every bind.
- **Transfers.**
  - Runs are merged down columns as well as along rows, so a full claim is one rectangle per source instead of about
    200 tile rows.
  - Where the device supports PSSpecifiedStencilRefSupported, one pass writes depth and SV_StencilRef instead of
    1 + 8 bit passes.
  - The pipelines are created once per destination format.
- **Ownership.**
  - **Fast path:** a target claiming tiles inside the rect it last took, with nothing in the book changed since, is
    O(1).
  - **Alternate rule:** the other ownership rule is only tracked on comparison frames (`--gow2_renderer_alt_rule`:
    compare by default, or always / off), started from a copy of the acting book. Disagreement labels therefore only
    cover that frame.
- **Index.** Cached index buffers are no longer copied per draw.
- **Summary.** The split line adds `ownership fast path`, `transfer runs` and `alternate rule`.

**Next:** M2 colour (notes/45, using the shared colour maths in upl-n103), then copy 0's 4 pixels.

## 14. upl-n104: M2 build A - colour targets, colour draws, colour resolve comparison

**Colour targets.**
- **Host storage:** colour targets now have real storage in the reference's host formats (SDK
  GetColorDrawDXGIFormat):

  | Guest format | Host format |
  |---|---|
  | 8888 | R8G8B8A8_UNORM |
  | 2_10_10_10 | R10G10B10A2_UNORM |
  | 7e3 (and its 16-bit view) | R16G16B16A16_FLOAT, unclamped |
  | 16_16_FLOAT | R16G16_FLOAT |
  | 16_16_16_16_FLOAT | R16G16B16A16_FLOAT |
  | 32_FLOAT | R32_FLOAT |
  | 32_32_FLOAT | R32G32_FLOAT |

- **Not modelled yet (build B):** 8888_GAMMA, 16_16 and 16_16_16_16 keep tile ownership only. Draws into them are
  counted in the new `colour (M2 A)` summary line.
- **Switch:** `--gow2_renderer_colour=false` gives back the M1 behaviour.

**Colour draws.**
- **Which slots.** Every target the pixel shader writes with a non-zero mask is bound in its own slot (SV_Target i).
  A slot repeating a lower slot's EDRAM base is dropped, as Canary does.
- **Blending:**
  - per target, from pipeline_state, with all factors and ops;
  - the guest's constant alpha uses BLEND_FACTOR, as the reference's D3D12 path has no ALPHA_FACTOR;
  - plus the write mask and the blend constant.
- **Exponent bias:** colour exp bias is 2^RB_COLOR_INFO.color_exp_bias per target, an exact power of two.
- **Depth.** Draws that only test depth bind the depth target read-only:
  - its tiles get their current content first, but ownership doesn't change (rect rule);
  - the Canary book still claims them.
- **Fast clears** redirect colour exactly as they do depth (pitch × 2, 1x, 2x scale).
- **Backend:**
  - real blend states;
  - five colour slots, with a null RTV for unused slots below a used one;
  - colour-only draws without a DSV.
- **Draw-ID diagnostic.**
  - It now has its own output, SV_Target4, for every translated pixel shader: a shader-cache post-process adds the
    member and the write. Depth-only draws use the stand-in shader, which also writes slot 4.
  - It lands in the R32F diagnostic texture of the first colour target drawn, or of the depth target.
  - Other MRT targets' pixels therefore read "not written this frame" in comparisons.

**Colour ownership transfers (build A part).**
- **Same storage class:** tiles a colour target takes from another colour target of the same storage class are
  copied through the EDRAM mapping (kColourTransferPs).
  - The copy has no 40-sample swap, and 64bpp tiles are 40 pixels wide.
  - The value goes through float in the same host format, which is exact.
- **Anything else** (from depth, or from another colour format) is counted (`colour tiles ... unknown source`),
  tainted and labelled "unknown (source not modelled yet)". That is build B.

**Colour resolves (kColourResolveCs, notes/45 section 3).**
- **Order:** our host value → the bits the reference dumps → decoded → × 2^exp_bias → R/B swap → encoded → words.
  - The dump step is the only lossy one: 7e3 targets are quantised to 7e3 RGB (round to nearest even, clamped to
    [0, 31.875]) and 2-bit alpha. It is exact for the other formats.
  - The words are written before the endian swap.
- **Destination formats:** 8888, 2_10_10_10, 16_16_FLOAT, 16_16_16_16_FLOAT, 32_FLOAT, 32_32_FLOAT. That covers
  notes/35's list except k_16_16 (#14), which is logged as NOT COMPARED.
- **Comparison.**
  - Bytes at the tiled destination addresses (64bpp: both dwords, with a diagnostic count of pixels that match with
    the dwords swapped). The max channel difference is in format units (integer steps for unorm, value for float).
  - It uses the same draw-ID/origin classification as depth, and up to 6 unexplained pixels per resolve with the
    draw that wrote them.
  - Destinations are keyed by (address, format, size), as 0x14550000 is reused within a frame.

**Not in build A:**
- MSAA colour resolves;
- resolve clears of colour;
- 8888_GAMMA, 16_16 and 16_16_16_16;
- colour↔depth and cross-format colour transfers;
- MRT "no write" semantics. A path that skips a written target writes 0, as Canary does; build B uses the
  merged-output approach.

**Also in this build.**
- **Texture loads:** tiled 2D raw copies move 16-byte runs (8 bytes at 8bpp), the largest pieces Xenos tiling keeps
  contiguous, instead of single blocks. Job 106 showed textures at 8 ms/frame even on light frames: about 3.5 MB/frame
  of re-uploads of textures whose pages change (resolve destinations). The real fix is untiling on the GPU from the
  guest-memory buffer, or using our own resolve outputs; that comes after build B.
- **Job 106's small depth differences** concentrate on two vertex shaders: 1D445FD4CAAE56B4 (shadow passes, D24S8,
  func LessEqual) and 709923D07FA2DDD8 (scene). Both fetch k_8_8_8_8 plus float3 (formats [6,57]), most likely
  skinned meshes: bone indices and weights. Suspect the order of the skinning arithmetic against the reference's
  compiled shader; to be checked after build B.

## 15. upl-n105: M2 build B - the rest of notes/45 that Gears 2 uses, built on upl-n103's maths

**Shared colour maths.**
- **Source.** xenos/colour_formats.h and xenos/renderer/colour_hlsl.h come from upl-n103 (with its tests, golden
  file and tools). `kHlslColourAll` is pasted in front of every colour shader, followed by our `kHlslHostWords`.
- **kHlslHostWords:** a host render-target texel ↔ the guest EDRAM word(s), which is the reference's dump/load.
  - It is exact for every modelled format.
  - 7e3 is quantised to 7e3 bits on the way out, as in both emulators.
  - 16_16 / 16_16_16_16 host SNORM bits are the guest bits.
- **Comparisons follow the SDK (`Reference::kSdk`).** The colour resolve shader is now `XeColourResolvePixel`
  (decode, average, 2^bias, swap, encode), or the words themselves on the SDK's raw path
  (`ColourResolveIsRaw(..., kSdk)`, marked " raw" in the source part of the line).

**16-bit fixed point (k_16_16, k_16_16_16_16).**
- **Storage and drawing:** R16G16(B16A16)_SNORM; the shader output is scaled by 2^(bias - 5), as the SDK does with
  snorm16_render_target_full_range.
- **Blending:** draws that blend multiplicatively into these formats are counted (`16-bit fixed draws blending
  multiplicatively`). Those are the only blends the 1/32 scaling gets wrong.
- **Resolve:** the SDK rule, decoded as max(-1, i × 32/32767) and always encoded unorm.
- **Number format in the log:** each colour resolve line now shows `number fmt N` (RB_COPY_DEST_INFO bits 13-15). For
  the velocity resolve (#14) that is what tells D23 whether the SDK throws away data the game needs.

**Transfers.**
- **Colour → depth:** depth = word >> 8, stencil = word & 0xFF of the colour's guest word, with the 40-sample column
  swap. It covers 32bpp sources in every modelled format. These tiles are no longer "colour source, needs M2".
- **Depth → colour:** word = depth24 << 8 | stencil, decoded in the colour format, with the swap (kWordTransferPs).
- **Colour → colour, different storage class, same bits per sample:** through the guest words (kWordTransferPs).
  Same class still uses the direct copy.
- **Not transferred (counted, labelled "unknown"):** 32bpp ↔ 64bpp, and 64bpp ↔ depth.
- **Job 107 (a) and (b).** The k_16_16_FLOAT green channel and the 7e3 HDR copy both had tiles from sources build A
  couldn't transfer: 2048 / 184 colour unknown-source tiles. The 7e3 target shares its base (720) with the D24S8
  target #1, so its content goes through depth and back. These transfers should be what (a) and (b) need; the next run
  will show.

**Colour resolve-clears.**
- **What happens:** RB_COPY_CONTROL bit 8 takes the rectangle's tiles, transferring partly covered ones first, then
  clears it.
- **Value order:** the SDK's, CLEAR | CLEAR_LO << 32.
- **Host value:** HostColourClearValue. The raw 16-bit and 32-bit values are given as the float that stores exactly
  those bits through our typed views.
- **In Gears 2:** none seen (notes/35), so the code path is untested.

**Comparisons.**
- **Colour verdicts:** the last writer explains a pixel only if it wrote a channel that differs.
  - Otherwise the pixel is put down to the tile's origin: "unknown (source not modelled yet)".
  - Or, if the last draw masked those channels, it goes into its own count, "older content in channels the last
    draw masked". That count keeps the copy out of "all explained".
- **Unexplained colour-pixel lines:** now also give the write mask, blend state, depth/stencil state and stand-in flag
  of the last writer.
- **Tile trace:** it samples colour targets too, giving our guest word(s) after each event. To follow copy 16's worst
  pixel, use `--gow2_renderer_tile_trace=1126 --gow2_renderer_tile_trace_pixel=51,6` (base 720, pitch 16 tiles,
  pixel 531,406).

**Not in build B:**
- **8888_GAMMA:** none in Gears 2.
- **MSAA colour resolves:** none in Gears 2.
- **MRT "no write" for outputs a path skips.** The reference writes 0 there (D3D10), and dropping the higher of two
  aliased slots matches it already. The D23 behaviour, leaving the pixel untouched, needs the translator to track
  written outputs: an hlsl.cpp change, for when a Gears 2 shader is shown to need it.
- **GPU texture untiling** for the texture-load cost (job 107 (f), 15.9 ms/frame while loading). It is the next build.

## 16. upl-n106: GPU texture loads

**The problem.** In job 107, texture loading cost 8-16 ms/frame on the GPU thread, which slowed the game. The cause was
CPU untiling and conversion of every reloaded texture: about 3.5 MB/frame in play, and more while loading.

**What changes: uncompressed 2D textures load on the GPU** (`--gow2_renderer_gpu_texture_load`, default on).
- **Which textures:**
  - one layer, not cube or 3D;
  - a raw-copy format whose host format has guaranteed typed UAV stores: R8, RG8, RGBA8, R16, RG16, RGBA16 (unorm),
    R16F, RG16F, RGBA16F, R32F, RG32F, RGBA32F, RGB10A2.
  - Those textures are created with UAV access. A typeless resource keeps its signed views, and the UAV is typed.
- **How:**
  1. Each load copies the guest bytes (base and mip ranges) into the GPU's guest-memory mirror, the buffer vertex
     fetch already reads.
  2. Then kUntileCs runs once per mip level and writes the host texture through a per-mip UAV.
  - It uses the same tiled addressing as the CPU path (TiledOffset2D/TiledCombine), and packed mip tails use
    PackedMipOffset as before.
  - The endian swap is applied to the aligned 32-bit word holding the texel, as the memory controller does. For 8- and
    16-bit texels that is more correct than the old per-texel CPU swap; the 16-byte-run CPU fast path from n104 already
    did the same.
- **Still on the CPU:** BC formats, converted formats (565/1555/4444, DXT3A, CTX1, 24-bit depth, 10:11:11), cube, 3D
  and stacked textures.
- **CPU cost left:** the memcpy into the upload ring (about 0.1 ms per MB) and the watch bookkeeping.

**Backend:**
- ComputeBindings gains `guest_t3` (the guest-memory raw buffer at t3) and `uav_mip`.
- Storage textures get typed UAVs, per mip on demand.
- Storage no longer prevents a typeless resource.

**Summary line:** it adds `GPU texture loads N (X MB guest bytes)`. The texture-upload MB figure counts only CPU loads.

## 17. upl-n107: stale textures within a frame, next-base clamp, descriptor tables, page-level texture copies

**Green channel (job 109 (1), ch02 copies 12-14 / ch10 8-10).**
- **Where G comes from.** PS 2B32497E9685ABE2 ends with `max oC0.xy00, r0.xwww`, so G = r0.w, the .y of `tfetch2D
  r0.__xy` from tf0. That texture is the previous half-res ping-pong resolve.
- **The bug: TextureCache::Lookup.** It skipped its re-check while `GuestWatch::Callbacks()` was unchanged, and our
  own resolves (GuestWatch::Bump) don't count as callbacks. A texture checked earlier in a frame therefore never
  noticed a resolve that rewrote it later in the same frame.
- **Fix.** Use the watch epoch, which every Bump advances, read before the load.

**HDR copy 16 (job 109 (2)).**
- **The trace** (full log, tile 1126). Draw 1295, the clear pair on #10 k_16_16 at base 1624, unclipped with the 8192
  scissor, takes the whole EDRAM under the rect rule. #3's 7e3 colour then goes 7e3 → 16_16 → D24S8 (#7, draw 1296) →
  D24FS8 (#2, draw 1308) → 7e3 (copy 16 reads it back).
  - For the traced pixel the chain is exact, as all its steps are designed to be. But the very first step quantises
    #3's fp16 content to 7e3 bits.
- **The reference.** It ends each bound target's range at the next bound target's base (SDK cache.cpp 569-659). The
  canary book in our log disagreed on exactly those tiles (184).
- **Fix.** The rect rule now applies the same clamp, `--gow2_renderer_clamp_next_base`, default on. A draw cannot
  sensibly write into the EDRAM of another target bound with it, and without primitive bounds this is the only limit
  an unclipped draw has.
- **Diagnostics.**
  - Colour comparisons now log up to 6 "transferred-but-different colour px" lines with ours and the guest's words,
    the source pixel, its EDRAM tile, and the tile-trace switches to follow it.
  - Trace lines for claims that don't change hands are dropped, roughly halving the trace.

**CPU (job 109 (5)).**
- **What job 107 already showed:** the record cost and the GPU waits were already there before GPU texture loads.
  Every textured draw took a 97-descriptor table (t0-t96), so about 1000 colour draws a frame wrapped the 65536-entry
  ring several times a frame, each time waiting on our GPU.
- **Fixes:**
  - **The guest-memory buffer (t96) is a root SRV.** The texture table covers t0 up to the highest slot the draw's
    shaders read; `SetTextures(..., view_count)`; transfers use 3.
  - **The shader-visible heap grows to 500k** descriptors.
  - **GPU texture loads re-copy only changed pages:** the 4 KB pages whose watch version changed since the texture's
    last copy, coalesced into runs.
  - **The split line adds** `descriptors N (ring waits W) | pipelines P | upload-ring waits U`.

## 18. upl-n108: CPU (record, GPU waits, textures) and the HDR copy-16 leg

**CPU (job 110 (4) and the coordinator's (a)-(c)).**
- **GPU waits by site.** Every blocking WaitForFence is now charged to a site: renderer (frame pacing, comparisons),
  readback release, sampler table, upload ring, descriptor ring, or other. A new line, `CPU detail`, prints count and
  ms/frame per site.
- **Sampler tables (the likely source of the 418-1325 waits).**
  - **Before:** the heap held 64 tables of 32 samplers, with a linear search. Once a command list used 64 different
    sampler sets, the next new set waited on our GPU.
  - **Now:** the heap holds the static table, 16 tables of 32 samplers, and 369 tables of 4 for draws that only read
    s0-s3. Every table is bound as a 32-sampler range, so each starts at slot 2016 or lower, and the whole heap is
    filled with the default sampler at start-up.
  - **Lookup and bookkeeping:** lookup goes through a hash map. A per-table flag replaces the open-list search.
    `SetTextures(..., sampler_count)` passes 32 minus the leading zeros of the fetch-slot mask, and transfers pass 4.
- **Pipeline states asynchronously.**
  - Game-draw PSOs are created on 1-4 worker threads (D3D12 devices are free-threaded). Pipelines live in a deque with
    an atomic `ready` flag; workers hold a stable pointer to their slot.
  - A draw whose PSO is still pending is skipped and counted as a pending shader, so the resolve says INCOMPLETE as
    for shader compiles. Such a draw has already taken tile ownership and transferred, which is the same as a draw
    with no visible pixels.
  - Internal pipelines (transfers, clears, untile) stay synchronous. `--gow2_renderer_async_pipelines`, default on.
- **Persistent index buffers.** Cached (unchanged) guest index buffers are copied once into a GPU buffer and drawn
  from there, instead of being copied into the upload ring on every draw. When the guest bytes change, the old
  buffer is released after our GPU is past it. Converted indices (fans, quads) still use the ring.
  `--gow2_renderer_static_index_buffers`, default on.
- **Record profile.** The backend times state application per draw (texture tables, constants, the rest). The
  `CPU detail` line prints:
  - record µs per drawn draw;
  - pipeline creation ms on the recording thread, async creations, and pending skips;
  - sampler tables written;
  - live persistent index buffers.
- **Texture validation by content (O(touched pages)).**
  - Job 110: texture cost grew with the cache because a 4 MB-block bump (the memory system unprotects whole blocks)
    changed the version of every texture in the block, and each was reloaded.
  - Now each entry keeps the version and XXH3 of every 4 KB page of its base and mip ranges. A version change re-arms
    the watch and re-hashes only the pages whose version moved. If they all hash the same, the texture is kept.
  - The first load hashes every page once. GPU loads still copy pages by version, so the mirror is never stale.
- **LRU eviction.** `--gow2_renderer_texture_cache_mb`, default 512 (0 = no cap). At each swap past the cap, textures
  not bound this frame or the last are destroyed, oldest first, down to 90% of the cap. The backend defers the
  release, drops a destroyed handle from the bound table, and the slot caches are reset.

**HDR copy 16 (job 111's trace, tile 1411 pixel 49,0 = (289,688)).**
- **Colour samples of #3 at that pixel:** C0000000 (draw 194) → 00702404 (546) → 01407C0F (1076, blended).
  - #3's tiles then go to #4 D24FS8 at base 1440 (draw 1103, an unclipped claim that wraps).
  - #5 k_16_16_FLOAT (draw 1141) gets back 01407FFF.
  - So the word changes on the colour → D24FS8 → colour leg: the stencil byte 0F becomes FF and the low 2 depth bits
    change.
  - Everything after that only carries the wrong word: #5 → #7 → #3 at draw 1197, and the clears 1295, 1296 and 1308
    move it on to copy 16. In 7e3 the word has R = 0x3FF (31.875). The blends that follow keep R at its maximum, so the
    resolve has R 3.98 where the guest has 0.0034.
- **Not the clears' claims.**
  - With `execute_unclipped_draw_vs_on_cpu=false` (the SDK default) and the D3D 8192 scissor, SDK cache.cpp 569-659
    gives these clears a height of GetRenderTargetHeight. That is the whole EDRAM, wrapping, which is the same as our
    claim.
  - The claims only matter if a leg isn't bit-exact.
- **The 20e4 maths is exact for this value.** Checked offline with the shaders' functions: XeFloat20e4To32, ×0.5,
  ×2 and XeFloat32To20e4 bring 0x01407C back unchanged, and the same holds for 0, 0x123456 and 0x000001.
- **The cause: the depth -> 16_16_FLOAT leg, not the depth leg** (found from the code after Brief 15, notes/48).
  - Exhaustive check: our shader encode/decode, ported to C++ (tests/layer0_transfer_words_test.cpp), is bit-exact over
    all 2^24 patterns, both with the x0.5 remap and through D24S8, and matches Canary bit for bit.
  - The next leg is #4 -> #5 k_16_16_FLOAT (draw 1141). As a 16_16_FLOAT word, 01407C0F has its low half 0x7C0F: an fp16
    NaN with a payload. kWordTransferPs wrote it through an R16G16_FLOAT render target as float, and the GPU
    canonicalised it to 0x7FFF. That is exactly 01407C0F -> 01407FFF.
  - SNORM16 has the same problem class: -32768 and -32767 both read as -1.0.
- **Fix (as Canary GetColorOwnershipTransferDXGIFormat).**
  - Formats: 16/32-bit float and SNORM16 render targets (k_16_16, k_16_16_16_16, k_16_16_FLOAT, k_16_16_16_16_FLOAT,
    k_32_FLOAT, k_32_32_FLOAT).
  - These targets are created typeless with an extra UINT RTV and SRV (`TextureView::bits_view`,
    `GraphicsPipelineDesc::color_bits`, `SetRenderTargets(..., bits_view)`).
  - Transfers into and out of them go through the UINT views (XeBitsToGuestWords / XeGuestWordsToBits; `XE_BITS_OUT`
    variants of kWordTransferPs and kColourTransferPs; t3 bits source in kTransferPs and kWordTransferPs).
  - Game draws and resolves still use the float/SNORM views.
  - `--gow2_renderer_transfer_bits`, default on; off gives the n107 behaviour.
  - Still open: colour resolves and the trace read these targets through float, so a resolve of a NaN-payload texel
    may still differ.
- **The trace sampled depth targets at the wrong column (fixed).** Depth tiles have their 40-sample halves swapped,
  so every depth value in n101-n107 traces was a different EDRAM sample than the colour lines (e.g. the 00100C01 lines
  in job 111). Depth samples now use column (px + 40) mod 80.
- **Transfer self-test** (`--gow2_renderer_transfer_selftest`, default on).
  - At the first swap, 1280 known words go through the real transfer passes on one-tile scratch targets, starting
    from an 8888 source:
    - D24S8; D24FS8;
    - 7e3;
    - 7e3 → D24FS8 → 7e3 (job 111's leg); 7e3 → D24S8 → 7e3;
    - 16_16_FLOAT;
    - D24FS8 → 16_16_FLOAT → D24S8.
  - The words: every stencil value with 16 depth patterns, the job-110/111 words, walking bits, and xorshift values.
  - Each chain logs `all 1280 words bit-exact`, or the count and the first 8 `in -> out` pairs. With the bits fix all
    chains are expected exact; `--gow2_renderer_transfer_bits=false` should show the 16_16_FLOAT chains failing on
    NaN-payload words.
- **gn_tests:** tests/layer0_transfer_words_test.cpp, header-only, under a second in -O2:
  - 2^24 x (decode matches Canary, Canary round trip, our round trip, the x0.5 remap round trip, our CPU
    xenos/formats.h);
  - 2^24 D24S8 round trips;
  - the bits-view channel packing.

## 19. upl-n109: complete comparison frames, sampler heaps per frame, texture-table cache, volatile textures

**Job 114 result (n108).**
- Every transfer self-test chain is bit-exact, including job 111's leg and 16_16_FLOAT.
- With `--gow2_renderer_async_pipelines=false` (job 115): the shadow copies match again, and so do the AO, half-res
  and velocity copies. HDR copy 16 drops from max 3.98 to 0.74 but still has 599,599 transferred-but-different pixels.
- But with async on, every frame-600 comparison was INCOMPLETE (313-1069 draws skipped). The CPU detail line showed
  sampler-table waits of 17-65 ms/frame and texture-table time of 18-67 ms/frame.

**Comparison frames are complete.**
- PSOs are created asynchronously only on frames that are not compared, and not the frame before one. Comparison
  frames create synchronously and wait for pending ones.
- Their draw-ID variants (slot 4 R32F) are new state combinations, which is why frame 600 was hit hardest. So when a
  normal frame meets a new combination, its comparison variant is queued too (`comparison variants pre-warmed` in
  the CPU detail line).

**Sampler tables.**
- **Size classes:** 4, 8, 16 and 32 samplers (88, 48, 40 and 20 tables). The 32s come last, so the last one starts at
  2016 and every 32-wide window fits the heap.
- **Three heaps,** rotated by `Backend::NextFrame()` after each frame's Submit. A heap is reused only when the frame
  three back is done, so one frame's tables never wait on the frame before.
- **Per-heap content-hash cache:** a set seen before in the heap is reused without writing (`found`). A full class
  spills into a larger one before waiting.
- **Diagnosis:** n108's "texture tables" time contained these waits (17.2 vs 18.5 ms/frame on ch02).

**Texture-table cache.**
- **Key:** the views of t0..count-1, by texture serial (so a reused handle is a new texture) and view key.
- **Reuse:** a hit reuses the shader-visible descriptors written for that set before, as long as fewer than half the
  ring's descriptors have been allocated since. In-flight work is never more than a few frames' worth (about 2000
  descriptors a frame against a 250k half-ring).
- **Cost:** a hit still records the SRV transitions but copies no descriptors. CPU detail: `texture tables reused N
  written M`.

**Volatile textures.**
- Job 114 hashed 1.4-2.7 M pages per 300 frames, but only 2976 re-checks found unchanged bytes. Most of the hashing
  was textures that really change every frame (resolve targets: about 10 GPU loads and 14 MB a frame).
- A texture whose bytes changed at 2 checks in a row is now volatile: it reloads on a version change without hashing.
  Every 32nd check of it hashes again, so one that settles returns to content validation.
- CPU detail: `volatile reloads N (M pages not hashed)`.

**HDR copy 16 (job 115).**
- The 599,599 "transferred-but-different" pixels cover almost the whole target. They are labelled so because copy 16
  reads #3 back from #2 (D24FS8, base 0), whose draw-1308 claim took every tile. With the transfers now bit-exact,
  that label hides the real last writer.
- **Next diagnostic step (not in n109):** carry the draw-ID coverage through transfers (copy the source target's
  coverage with the same EDRAM mapping), so the verdict names the draw that last wrote the word.
