# 29 - Stage 1 plan: resources (create / lock / unlock) - for review, nothing started (2026-10-20)

> **Carried into Gears2-Native on 2026-10-20 as a plan only.** File names such as `src/...`, `tools/...`, `patches/...` and job numbers refer to the deleted Gears2-Recomp project. They record what we learned there; none of that code exists in this project.

Part of `28-native-renderer-roadmap.md`. Planning only: no code or behaviour changes.
Sources: two read-only research passes over the recompiled game code (`generated/default/gow2_recomp.*.cpp`, PPC listing) on
2026-10-20, the D3D traces of Play-Test56/57/58 (menu / ch2 hospital / ch10 firefight, `out/play-logs/20261020-125518`,
`-125627`, `-125755`, including frames 1-600 for loading), `09-d3d-map.md` and `27-d3d-inventory.md`.
Labels: **[V]** read in the PPC listing or our source, **[T]** trace counts (menu / ch2 / ch10 per frame unless noted),
**[I]** inferred.

**Stage 1 goal (from 28).** Keep the game's own memory allocations and layout unchanged, and take over **knowing when resource
contents change**, so the replayer stops rediscovering changes by hashing guest memory at every draw. The original D3D functions
still run in Stage 1.

## Headline findings (they change the plan)
1. **The game creates almost nothing through D3D Create\*** [V, T]. UE3 builds resources itself, with Xbox XGraphics header calls
   over memory it allocated, then binds them to that memory with `829A0E58` (XGOffsetResourceAddress) / `829A0DF0`
   (XGOffsetBaseTextureAddress). CreateTexture `8297C298` runs about 9 times per run, only for render/resolve targets, and 0 times
   in the steady windows.
2. **Every CPU lock goes through one function, `82976600`, the lock core** [V]. Its only callers are the RHI VB lock `8280DFF0`, the
   RHI IB lock `828074A0` and texture LockRect `8297B730`. (`09` / `27` mislabelled it "BlockUntilIdle"; corrected.)
3. **Lock and Unlock counts match every frame** in all three scenes: 16.05 / 11.82 / 10.84 per frame [T]. About 1 per frame is a
   VB/IB lock; the rest are texture-level locks (mip streaming).
4. **Per-frame dynamic geometry does not use Lock at all.** It goes through the Begin*Vertices ring: **12 inlined EndVertices
   sites in 8 game functions**, never unlocked [V].
5. **Contents change without a lock in a few specific places:**
   - texture pool defragmentation `8280D150`
   - in-place mip reallocation `8280B0C0`
   - the dynamic VB "rename" in `8280DFF0`
   - two inline copies of Unlock in game code
   - two buffers locked and never unlocked
   - Resolve writing texture memory [V]
6. **D3D never frees game resources** (destructor `829763D0`: 0 calls) [T]. UE3 frees memory itself through the deferred free
   `828093B8`, which frees **immediately when the GPU is idle**. So the same address can be reused within one frame [V/I],
   matching the job 198 / 211 "rewritten within a frame" findings.

**Consequence for the design.** "Capture at create + unlock" is **not enough on its own, but the gaps are few and specific** (section
2). An owned copy is keyed by **resource header + version**, with a **memory-range index** beside it for aliasing and relocation.
Both research passes arrived at this independently, with opposite emphasis: one suggested keying by memory range, the other by header
+ version. The plan uses both indexes.

---

## 1. The resource functions

### 1a. Header layout as this binary uses it [V]
- **All types:**
  - +0 Common: type in bits 0-3, **lock count in bits 8-11**, 0x40000000 = child surface, IB index format in bits 29-31
  - +4 RefCount
  - +8 Fence (game code reads it, `828093B8`)
  - +12 read fence
  - +20 dirty range (high 16 bits = start line, low 16 = end line, 128-byte lines; empty = 0xFFFF0000)
- **Vertex buffer:** +24 = address | 3; +28 = size & 0x03FFFFFC | 0x10000000 | 2.
- **Index buffer:** +24 address; +28 size.
- **Texture (52 bytes):**
  - +24 mip dirty range
  - fetch constant at +28..+48
  - **base address = +32 bits 12-31**
  - level count = +44 bits 6-9
  - **mip address = +48**
  - dimension = +48 bits 9-10
- **Surface (48 bytes):** +24 parent (child surfaces); +28 low 12 bits = EDRAM tile base; +36 size; +40 format.
- **Shaders:** vertex shader microcode at +32, pixel shader microcode at +24.
- **Content hashes must ignore +8, +12, +20 and +24.** Game code writes the fence fields inline (`82802C40`, `82802F90`, `8280E8A0`
  write texture header +8) [V].

### 1b. Functions that matter for Stage 1
| Addr | Name | Args → return | Writes | Stage-1 action |
|---|---|---|---|---|
| **829A0C40** | XGSetTextureHeaderEx [V dataflow] | r3 W, r4 H, r5 Levels, r6 Usage, r7 Format, r8 ExpBias, r9 Flags, r10 BaseOffset; stack +84 MipOffset (−1 = contiguous), +92 Pitch, +100 pTex, +108/+116 size outputs → total size | 52-byte header built in `829A0200`, copied to pTex | **create:** remember (pTex, dims, levels, format, sizes); final only at `829A0E58` / `829A0DF0` |
| **829A0D78** | XGSetCubeTextureHeaderEx [V dataflow] | 11 args, the last three on the stack → total size | 52-byte header | as above (cube) |
| **829A0CB8** | header variant with shared mip tail [V] | - | header | as above; marks **aliasing** (two textures, one allocation: `8280B320`) |
| **829A00D8** | XGSetVertexBufferHeader [V] | r3 Len, r4 Usage, r5 Pool, r6 Offset, r7 pVB | memset 32; +0/+4/+20/+24/+28 | **create:** remember (pVB, Len) |
| **829A0170** | XGSetIndexBufferHeader [V] | r3 Len, r4 Usage, r5 Format, r6 Pool, r7 Offset, r8 pIB | +0/+4/+20/+24/+28 | **create:** remember (pIB, Len, format) |
| **829A0E58** | XGOffsetResourceAddress [V] | r3 res, r4 base | VB/IB/PS +24 += base; textures via `829A0DF0` | **bind / re-key:** new version of the resource at its final address; capture contents (except after defrag, see 2c) |
| **829A0DF0** | XGOffsetBaseTextureAddress [V] | r3 tex, r4 base, r5 mip | +32/+48 | as `829A0E58`; also called directly by UE3 (`8280B320`, `8280D150`) |
| **82976600** | lock core [V] | r3 res …, r9 locked address, r10 byte size, [sp+84] flags → address | waits on fence +8 (or +12 when flags & 0x1010); cache invalidate unless flags & 0x12; dirty range to +20/+24 only if flags & 0x10 or Common bit 0x00200000; Common += 256 | **"about to change":** mark (resource, range) pending |
| **829768C0** | Unlock [V] | r3 res, r4 base, r5 mip | Common −= 256; on the last unlock flushes (`82990018`) and resets +20/+24 | **commit:** capture the pending range, new version |
| 8297B730 | LockRect core [V/I] | r3 tex, r5 array index, r6 level, r7 flags, r8..r10 and [sp+84] outputs | via `82976600` | covered by the `82976600` hook |
| 8297BEE8 | LockTail [I] | r3 tex, r4 array index, r5 out, r6 flags | via `8297B730` | covered |
| 829831F0 | vertex-shader header init over caller memory [V] | r3 VS, r4 microcode address | +0=6, +4, +20, +32, fetch tables | **shader resource:** register the shader object (microcode is static) |
| 8298FE90 | vertex-shader Bind [V] | r3 VS, r4 flags, r5 decl, r6 strides, r7 PS | VS tables (+40+416k), +872 \|= 0x40, decl+48 id | shader group: observe only |
| 82809A38 (UE3) | pixel-shader header written by game code [V] | - | +0=7, +4=1, +20=0xFFFF0000, +24=microcode | register at first bind (no D3D call to hook) |
| 8297C298 | CreateTexture [V] | (W, H, Depth, Levels, Usage, Format, Pool, Type) | header + XMemAllocDefault memory | render/resolve targets only (`8280B4D0` when flags & 0x2/0x80, `8280C570`, `82806A80`): register as GPU-written |
| 8297C408 | CreateSurface [V] | (W, H, Format, MSAA, params) | EDRAM tiles via `82990208` | EDRAM surface: no memory contents; register only |
| 82976B20 | Release [V] | r3 → count | +4 −1; at 0 calls the destructor (never happened) | pass through; drop the entry if it returns 0 |
| **828093B8** (UE3) | deferred free [V] | resource | checks IsSet `82976C98` and fence via `8298C6D0`, frees via `82809320` (immediately when idle) | **lifetime end:** retire the owned copy; that address can be reused at once |

**Read-only queries, no takeover needed** (reuse for decoding) [V]: GetDesc `8297C570`, GetLevelDesc `8297B0B8`, GetType `82976C08`,
IsSet `82976C98`, XG layout helpers `8299EF80` / `8299EB48` / `8299EF78` / `8299E850` / `8299F278` / `8299F748` / `8299F7A8`.

**Moved out of Stage 1:** SetShaderGPRAllocation `82983698` (device state, writes dev+10920, to Stage 5) [V].

### 1c. Where the memory comes from [V]
- VB/IB and shader microcode come from GMalloc (`*0x8310B12C`) vtable slot +16 with args (size, 1), or from bulk-data arrays.
- Textures come from a **UE3 texture pool**: object 0x8313E670, set up by `8280D4C8` / `8280D030`, allocated by `8280DB18` (best-fit
  allocator `82220B28`), defragmented by `8280D848` / `8280DA10` → relocation callback `8280D150` [V/I].
- Physical pages come through `82AAA130` (MmAllocatePhysicalMemoryEx); write-combine/no-cache through `82AAA1B8`. The title overrides
  XMemAlloc / XMemFree (`8280F450` / `8280F478`). Attribute 0xA7820007 goes to the UE3 pool (`8280F288`).

### 1d. UE3 callers that feed these hooks [V]
- VB `8280DDF0`; dynamic VB lock / realloc `8280DFF0`.
- IB `82807350`; IB lock `828074A0`; game code `82334010` builds IBs directly.
- 2D textures `8280B4D0` → `8280AFB0` / `8280B320`; cubes `8280C570` → `8280C490`; other textures `8280CAB8`.
- Streaming `8280B0C0`; defrag `8280D150`; texture lock wrappers `8280BA18` / `8280C248`.
- Vertex shaders `828098D0`; pixel shaders `82809A38`.

---

## 2. Writes to resource memory without a matching Unlock

### 2a. The Begin*Vertices ring: never unlocked [V]
Rates [T]: BeginVertices `8297FD30` 27.8 / 3.7 / 9.2, BeginIndexedVertices `82980210` 91.8 / 90.9 / 21.9.
The 12 inlined EndVertices sites (`lwz 13444(dev); stw 48(dev)`):

| Function | End site(s) | Ring filled by |
|---|---|---|
| `82803830` DrawPrimitiveUP | `82803900` | memcpy |
| `82803A98` DrawIndexedPrimitiveUP | `82803B98` | memcpy |
| `826FB630` batched elements | `826FB96C`, `826FC3D0` | memcpy |
| `826FB630` | `826FBE08`, `826FC1F0` | direct stores |
| `826FD7A8` | `826FD8BC`, `826FD960` | virtual callback (bctrl at `826FD8A0` / `826FD944`) writes straight into the ring; likely particles [I] |
| `822D0900` | `822D0F98` | direct stores |
| `828044F0` canvas rect lists | `82804DC4` | direct stores |
| `8272B1F8` occlusion pass | `8272B874` | memcpy, then a D3D call (`82977ED0`) before End |
| `82336C00` | `82336D54` | memcpy |

`82803F38` (the 9th dev+48 writer in `09`) only emits a PM4 packet; it fills no vertices.
**Every site fills the ring before the next D3D call** [V], so "copy at the next D3D entry" (what `FlushPendingVertices` does
today) stays valid. Not checked: whether the bctrl callbacks in `826FD7A8` call D3D themselves.
**Stage 1 action:** Begin*Vertices data is "transient". Capture it at the next D3D entry, as today. Owning the ring itself is a
Stage 3 task.

### 2b. Buffers written without a `bl 829768C0` [V]
| Where | What | Covered by |
|---|---|---|
| `8259CE00` | locks a VB, a CPU loop animates vertices, **inline Unlock** at `8259D134..8259D1D4` | lock-count check at draw (2e) or patch the inline Unlock |
| `827BF6F0` | second IB ends with an **inline Unlock** at `827BF904..827BF9A4`; one-time grid builder [I] | as above |
| `82334010` | instanced IB, header built by hand (`829A0170` + `829A0E58`), locked at `82334134`, filled, **never unlocked** | lock-count check at draw |
| `82792FE0` | VB pair **locked once**, CPU pointers stored at obj+32 / +36, unlocked only in `82793090` on resize / teardown. **Writer not found.** | lock-count check at draw |
| `824E7FE0` / `824E7E80` | append-only index batcher: unlock, draw, re-lock | Unlock (per range and per version) |
| `8280DFF0` | **dynamic VB rename**: before locking, defers freeing the old memory (`828093B8`), rebuilds the header (`829A00D8`), allocates new memory, sets the base (`829A0E58`). The base changes at lock time. | key by header + version, read the base at Unlock |

All 38 game uses of a VB header's base address end in an Unlock [V].

### 2c. Textures
- **Mip streaming is covered** [V]. In `82358140`, each new mip is locked (`8297B730` at `823584E0`), async IO reads straight into
  that pointer (IO vtable call at `82358594` / `823585FC`, no memcpy), and finalize `823586D8` unlocks each mip (`823587B4`).
  Capture at the **last** unlock.
- **Pool defragmentation is not covered by Unlock** [V]:
  - Allocation `8280DB18` calls `8280D848` on failure. It waits for the GPU to go idle, changes pool protection and walks the
    allocator (`822207B0`), calling the relocation callback `8280D150` [I].
  - `8280D150` rebuilds headers and calls `829A0E58` **first**, then `memmove(new, old, size)` at `8280D448`.
  - So a capture at `829A0E58` would read the new address **before** the data arrives.
  - **Action: a post-call hook on `8280D150`** (or `8280D848`) moves owned copies to their new addresses. The defrag rate is unknown;
    a counter will show it. A full pool would explain the LODBias-0 purple sheen of Test 43 [I].
- **In-place reallocation `8280B0C0`** [V]: grows the block in front of the old one (`8280DD58`) and builds a new header over the old
  bytes with no copy. The old and new textures alias the same memory (linked through +36). **Action:** the memory-range index
  shares the copy and re-versions it.
- Other texture writers (`8280BA18` / `8280C248` copy and tile paths, `82357688`, `823B37F8`, cube textures, fluid-sim `827BECD0`)
  all Lock, fill and Unlock [V].

### 2d. GPU writes to memory
- **Resolve `8298A128`** writes EDRAM results into texture memory 14 / 25 / 27 times per frame [T]. **Action:** mark the destination
  "GPU-owned". Our resolved copy is the content (today's `RememberResolvePages` / `g_tc.resolve_pages`, `native_texture_cache.h`) [V].
- Occlusion queries and fences: the GPU writes results into query objects and headers (`82978120`); the CPU only reads them. Not
  content.
- Memexport: not determined. The translator records a memexport flag, so checking the shader pack will settle it.

### 2e. Minimum extra hooks beyond create + unlock
1. Begin*Vertices ring: copy at the next D3D entry (existing behaviour).
2. The XG header builders plus `829A0E58` / `829A0DF0` act as "create".
3. A post-call hook on `8280D150`: move owned copies after the memmove.
4. **A lock-count check at draw time.** If a bound resource has header +0 bits 8-11 non-zero, or was never captured, hash/copy it at
   the draw. This one check covers `82334010`, `82792FE0` and both inline Unlocks. Optional: patch the inline Unlocks at
   `8259D134` and `827BF904`.
5. Key owned copies by **header + version**, with a memory-range index for aliasing (`8280B0C0`, `829A0CB8`) and same-frame reuse
   (`828093B8`).
6. Resolve destinations are GPU-owned.
7. **Keep the low-rate full-hash verify (job 240) as a safety net** throughout Stage 1.

---

## 3. What happens to the existing mechanisms
| Mechanism today [V] | Where | After Stage 1 |
|---|---|---|
| Stream snapshots at draw time, epochs per rewrite (job 211), static-buffer skipping (jobs 152/153/216) | `SnapshotStreams` (`gow2_native_scene.cpp`); switches `gow2_native_snapshot_*` | **Deleted** for header-backed VB/IB: the owned copy at Unlock/create is the snapshot. **Kept** in a smaller form for Begin*Vertices ring data (copy at the next D3D entry) and for the lock-count fallback (2e.4). |
| Buffer hashing at replay: full hash for small buffers, sampled + full every 120 frames for large; game-thread known hashes | `GetGuestBuffer`; `gow2_native_known_hashes`, `gow2_native_buffer_full_hash` | **Deleted:** the version number replaces the hash. Host buffer upload happens when the version changes. The dynamic ring for per-frame data (`gow2_native_dynamic_ring`) **stays**. |
| Texture sample hash (64 samples per level/face), settle logic (job 179), change-sync / first-sync (jobs 175/180) | `SampleHash`, `GetOwnTexture` (`native_texture_cache.h`); `gow2_native_own_textures_*` | **Deleted:** decode when the texture's version changes. Settle is no longer needed, because we know when the streamer finished (last Unlock). |
| Round-robin full verify (job 240) | `OwnTexturesFrameEnd`, `gow2_native_own_textures_verify` | **Kept** as the safety net, with mismatches logged as "missed write path". It goes only when the comparison mode has been clean for a full chapter sweep. |
| Resolve-written page set | `RememberResolvePages`, `g_tc.resolve_pages` | **Kept**, folded into the "GPU-owned" flag of the resource table. |
| Emulator stand-in textures (`ctx.guest_textures->GetTexture` when our decode can't: unsupported format, 3D/1D/stacked, BC size, layout, resolve pages) | `gow2_native_scene.cpp` ~4000 / ~4119; NATIVEMISS / NATIVETEX counters | **Kept in Stage 1.** Ownership doesn't fix unsupported formats. They become **logged and counted per format** (principle 3) and are removed format by format later (Stage 6 needs zero). |
| Canvas ring capture: mid-asm hook at `0x82803B48`, `FlushPendingVertices` | `canvas_regs.h`, `gow2_native_scene.cpp` | **Kept** until Stage 3 owns the ring. |

Expected gain: no per-draw hashing on the game thread (recording was the game-thread bottleneck in busy scenes, job 150) [T], and no
"stale texture for a frame" class of bugs (jobs 178-180) [T].

---

## 4. Comparison mode (prove it before switching)
- **Switch:** `gow2_native_own_resources_compare` (Testing). Both paths run: the current replayer (snapshots, hashes) as the
  reference, the owned copies as the candidate.
- **At every draw**, for every bound vertex stream, index buffer and texture (base level, mips, cube faces), hash the owned copy
  and what the current replayer would read for that draw.
- **Log a mismatch once per (resource, version):** `NATIVEOWNCMP`, with resource header, type, address, size, version, the function
  that created/last committed it, the caller (lr at that call), the draw's shaders, and which side looks newer.
- **Classify mismatches automatically:**
  - "replayer newer" = a missed write path
  - "owned newer" = the replayer read too early or too late, an old bug
  - "header moved" = defrag or rename
  - "never committed" = a lock-without-unlock buffer
- **Counters every 600 frames:** resources tracked per type, versions per second, captures by path (create, unlock, defrag hook,
  lock-count check, ring), mismatches per class.
- **Pass criterion:** over the menu, the ch2 hospital and the ch10 firefight (Stage 0 harness runs, or the user's runs), **zero
  unexplained mismatches**. Every remaining mismatch class has a named write path and a hook. Plus one chapter sweep with the
  switch on and the job-240 verify finding nothing stale.

---

## 5. Risks, unknowns and estimate
**Risks / unknowns.**
- **The writer behind `82792FE0`'s locked-once VB pointers** isn't found. The lock-count check covers it at a hashing cost; how
  often it is drawn decides the cost.
- **Defrag frequency** is unknown. If it runs often, the post-hook on `8280D150` must be cheap, moving copies instead of
  re-decoding.
- **Same-frame address reuse** through `828093B8`, which frees at once when the GPU is idle [V]. Our replay reads a frame later,
  so owned copies must be immutable per version. That is the point of versioning.
- **Async IO completion timing:** the last Unlock in `823586D8` comes after IO completion [V]. If any streaming path unlocks before
  its IO finishes, it would show up as "replayer newer" mismatches [I].
- **The movie library's texture path** (Bink planes, `gow2_native_emu_tex_ps`) is not analysed.
- **Memexport use** is not determined.
- **Lock flag meanings** (0x10, 0x1010, 0x12, 0x1) and Common bit 0x00200000 are inferred from behaviour only.
- **The trace's library ranges** count calls from 0x829A3000-0x829D6000 as "inside"; loading windows (frames 1-600) may not cover
  each chapter's full streaming.

**Estimate (working days, mine).**
- Resource table + create/bind hooks (XG builders, `829A0E58` / `829A0DF0`, CreateTexture / CreateSurface, `828093B8`): 1.5-2
- Lock / unlock hooks, inline unlock handling, lock-count check at draw: 1-1.5
- Defrag / realloc hooks (`8280D150`, `8280B0C0`, aliasing index): 1-1.5
- Comparison mode, counters, classification: 1.5-2
- Switching VB/IB and textures over to versions (`GetGuestBuffer`, `GetOwnTexture`): 1-2
- Runs, mismatch hunting, write-up: 1-2

**Total: about 7-11 working days**, plus 3-4 user runs (or Stage 0 unattended runs). This is above the 6-9 days in `28`, because the
XG / defrag / ring findings add hooks. `28` is updated to match.

## 6. Checkpoint (from 28, made concrete)
- **Continue** if the comparison mode is clean in the three scenes, the mismatch classes all map to the hooks in 2e, and the job-240
  verify finds nothing stale in a sweep.
- **Stop and rethink** if:
  - "replayer newer" mismatches keep appearing from write paths not in this list after one investigation round, or
  - defrag or the lock-count fallback turns out to run so often that hashing is not actually removed.
  In that case the hybrid stays: owned copies plus hashing as the safety net.
