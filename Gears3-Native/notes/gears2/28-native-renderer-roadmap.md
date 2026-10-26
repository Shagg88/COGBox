# 28 - Native renderer roadmap: from replayer to our own D3D device (plan for review, 2026-10-20)

> **Carried into Gears2-Native on 2026-10-20 as a plan only.** File names such as `src/...`, `tools/...`, `patches/...` and job numbers refer to the deleted Gears2-Recomp project. They record what we learned there; none of that code exists in this project.

Planning only. No code or behaviour changes, and no stage is started until the user decides.
Base: `27-d3d-inventory.md` (what the game calls per frame in menu / ch2 / ch10, and the inline device writes) and `09-d3d-map.md`
(library bounds, device layout, names, draw path).
Labels: **[V]** verified in code (our source or the game's PPC listing), **[T]** seen in a trace or play-test log, **[I]** inferred.

---

## 1. End state and principles

**End state.**
- The game's D3D calls go into **our own device implementation**.
- The shipping build contains **no Xenia GPU emulation**: no Xenia shader translator, EDRAM/render-target cache or texture cache
  running. The emulator stays only in developer builds, as a reference to compare against.
- Formats, resolution, anti-aliasing and passes are ours to choose.

**Where we are today [V].** We are a **replayer**:
- All 1,811 hooks in `src/gow2_d3d_trace.cpp` call the original D3D. Only DrawIndexed `82980B20` and DrawTessellated `82980F70` are
  skipped (`NativeSceneSkipEmulatedDraw`, `gow2_native_scene.cpp`), plus Resolve `8298A128` on 3D frames
  (`NativeSceneSkipEmulatedResolve`).
- The game's D3D still builds command buffers, and the emulator still executes them.
- We read state from the device shadow at each draw (`RecordDraw`), constants from dev+1920 / dev+6016, and buffer and texture
  contents from guest memory at replay time (`SnapshotStreams`, `GetGuestBuffer`, `native_texture_cache.h`).

**Principles.**
1. **One source of truth: our device.** Each stage moves one group of functions from "original D3D runs, we watch" to "our
   implementation runs, the original does not."
2. **Capture when the game issues it.** State and resource contents are taken when the game creates, unlocks or sets them, not
   rediscovered later by hashing guest memory.
3. **Nothing unhandled falls back silently.** Every call, field or format we don't handle is logged and counted, and shown in the
   NATIVE log summary. Today the device fields we don't read are ignored without a word (`27`, architecture check, Q6).
4. **Prove each stage against the emulator before switching.** Every stage first ships a comparison mode: ours runs next to the
   original and logs differences. The stage's switch stays off until the user's runs pass.
5. **The emulated renderer stays the default** for normal play until the replacement passes a full chapter sweep. Each stage has its
   own switch (`gow2_native_own_<group>`), so any stage can be turned back off.
6. **Keep the game's device memory layout.** 81 game functions write 64 device fields directly (`27` section 3) [V]. Our device keeps
   the same struct (0x5E00 bytes, global 0x83105D40, `09` section C) and the same dirty-bit meanings, as UnleashedRecomp's
   `GuestDevice` does.

**An important caveat about what the stages fix.** The stages replace *plumbing*: who creates resources, who issues the GPU work,
who answers queries. The current picture bugs (white ghost, dark rooms and black guns without the 0.25-px nudge: Tests 46-55) come
from how **our draws execute**, not from the plumbing. Job 229 showed the light draws get exactly the emulator's inputs [T]. So
the stages will not fix those bugs by themselves. Stage 3 is where our draws become the only draws, and a **pixel-parity
checkpoint** sits there (section 4). **[Superseded by D19, 2026-10-20: the ghost and dark-room bugs were the old replayer's, not the emulated renderer's; they are no longer open work. Stage 0 comparisons catch them as specific mismatches if they reappear.]**

---

## 1b. Layers for reuse (Gears 3, Gears 1)

**Goal.** A later Gears 3 or Gears 1 port should only need new *upper* layers. So the replacement device is built as four layers,
each its own CMake static library. A lower layer never includes an upper layer's headers, so a mistake fails to build instead of
quietly creating a dependency.

**Our XEX's linked libraries [V].** Read from `game/default.xex`, XEX optional header `0x000200FF` (static libraries).
The script is `/tmp/claude-0/inv/xexlibs.py`; it's worth adding to `tools/` once code changes are allowed.

| Library | Version | Approval |
|---|---|---|
| **D3D9LTCG** (Direct3D) | **2.0.7645.1** | approved |
| **XGRAPHC** (XGraphics) | 2.0.7645.0 | approved |
| D3DX9, XAPILIB, XBOXKRNL, LIBCMT, XNET, XONLINE, XMP, XHV, XAUDLTCG, X3DAUDL | 2.0.7645.0 | approved |

**What this tells us.**
- Gears 2 links XDK build **7645**: D3D at QFE 1, everything else at QFE 0.
- The D3D library is the **LTCG** (link-time code generation) variant. The compiler could inline D3D code into game functions and
  specialise it per title.
- That is very likely why 81 game functions write device fields directly (`27` section 3) [I].
- It also means another game linked against the same XDK version can still have different machine code [I]. So a version profile
  can't be a fixed address list: it needs byte signatures plus checks.

### Layer 0 - Xbox 360 GPU core (game-free, XDK-free)
What the Xenos hardware does, whoever drives it.

**Covers:**
- Texture and surface formats and their conversions (7e3, 20e4/DepthF24, 2_10_10_10, BCn, endian swaps).
- Untiling and tiling, and mip / packed-mip layout.
- Resolve and copy semantics, MSAA sample positions, EDRAM pitch and base rules.
- The ucode→HLSL translator and the shader-pack format.
- The nrhi device (our D3D12/Vulkan backend).
- The screenshot / FLIP comparison core.

**Today's code:**
- `src/native/xenos_texture.h`, `guest_texture.h` and `native_texture_cache.h`.
- The format and resolve helpers in `native_scene.hlsl` / `kSceneHlsl` (`XeF32To20e4`, `DepthF24`, `ResolvePs`).
- `tools/ucode2hlsl/` and `tools/build_shader_pack.py`.
- nrhi in `patches/rexglue-sdk.diff` (including `SetSamplePositions`).
- `tests/xenos_texture_test.cpp`.

**Reuse:** as-is for every 360 game.

### Layer 1 - Graphics-library version profile (XDK D3D 7645.1 + XGraphics 7645.0)
What *this build of Microsoft's D3D library* does on top of the hardware.

**Covers:**
- Device struct layout: 0x5E00 bytes, field offsets, dirty-bit meanings, and the shader-constant areas at dev+1920 / dev+6016.
- Texture, vertex-buffer and index-buffer header layouts as XGraphics builds them (`829A0C40`, `829A0D78`, `829A0CB8`, …).
- The lock core `82976600` and its semantics.
- The command-buffer format and the draw-time flush rules.
- The table of the 136 D3D/XG entry points, each with address, byte signature and arguments.

The profile is **data plus a few adapters**. A table maps each logical D3D function (SetTexture, Lock, DrawIndexed, …) to its
address and signature, and our device is written against the logical names.

**Today's code:**
- The 1,811 hook addresses in `src/gow2_d3d_trace.cpp`.
- The `kDrawIndexed` / `kDrawTessellated` / `kDrawVertices` constants and the device-shadow reads in `RecordDraw` (`gow2_native_scene.cpp`).
- `09-d3d-map.md` and `27-d3d-inventory.md`.

**Reuse:**
- **A game on the same 7645 libraries** reuses the struct and header layouts, and its addresses are re-found by signature.
- **A different XDK version** needs a new profile: diff the struct and headers, then re-sign the functions.
- **Later tool:** `tools/d3d_profile_scan.py` would read the static-libraries header, find each signature in the new XEX, and report
  which entries match, moved or are missing.

### Layer 2 - Unreal Engine 3 layer (UE3 Xenon RHI usage)
What *UE3's renderer* does with D3D.

**Covers:**
- Resource lifetime: the deferred free `828093B8`, texture-pool defrag `8280D150` and realloc `8280B0C0`. These are UE3's Xenon
  texture-pool code, not D3D [I, from their callers in `29`].
- Texture streaming: pool size, LOD bias, and the system-settings / Coalesced-ini knowledge in `gow2_engine_settings.cpp`.
- The scene pass structure: depth prepass, light passes with the GEQUAL re-test, the bloom/DOF/motion-blur chain, the tone map.
- UE3's shader-parameter conventions.

This layer names passes by *role* ("light pass", "tone map"), not by Gears 2's shader hashes.

**Reuse:**
- Gears 1 (2006), Gears 2 (2008) and Gears 3 (2011) are three different UE3 versions [I]. So this layer is "UE3 core plus a small
  per-engine-version table".
- The role-based pass logic and the resource-lifetime model carry over.
- The pool and streaming addresses are re-found for each game.

### Layer 3 - Gears 2 game layer
Everything specific to this XEX and this game's content.

**Covers:**
- Game-code addresses: the 12 EndVertices sites in 8 functions, and the untiled patch `82806A44`.
- The shader-hash → pass-role table (`B6B6DD4E` = tone map, `00700263` = light pass, the hashes in `gow2_native_pass.cpp`).
- Per-game fixes: the black-shading nudge, bloom-off, AO alternate.
- HUD, UI and canvas passes (`gow2_native_hud.cpp`, `gow2_native_ui.cpp`, `canvas_regs.h`).
- Game patches (`gow2_game_patches.cpp`), frame-pacing hooks, crash watch.
- Chapter boot, scripted test routes and the Play-Test options.

**Reuse:** none. Gears 3 and Gears 1 each get their own Layer 3.

### Where today's code stands, and when to split
**Already in one layer:**
- `xenos_texture.h`, `guest_texture.h`, `native_texture_cache.h` and the translator are Layer 0.
- `gow2_d3d_trace.cpp` is almost pure Layer 1: 1,817 guest addresses, all D3D/XG hooks [V count].

**Mixed:** `gow2_native_scene.cpp` (5,901 lines) has all four layers:
- Layer 0: format and resolve code, the replay executor.
- Layer 1: draw entry-point constants, device-shadow reads.
- Layer 2: pass-role logic.
- Layer 3: per-hash fixes, research switches (jobs 193-235) and comparison tools.

**Proposal:** split it in **Stage 0** as a no-behaviour-change refactor, proven by the Stage 0 harness (same frames, FLIP score
zero, same NATIVE log counters). Estimate +2-4 days [I].

After the split, each stage puts its new code straight into its layer:

| Stage | Layers its new code goes into |
|---|---|
| 1 Resources | Layer 1 (headers, lock), Layer 0 (formats), Layer 2 (texture pool) |
| 2 Resolves and tiling | Layers 0 and 1 |
| 3 Draws | Layer 0 (executor), Layer 1 (flush) |
| 4 Queries, sync, swap | Layer 1 |
| 5 State | Layer 1 |
| 6 No emulator in shipping build | No layer change |
| 7 Freedom features | Layer 0 (mechanics); Layer 3 decides where they apply |

**What a Gears 3 or Gears 1 port then costs:**
1. Read its static-libraries header.
2. If its D3D version is 7645, re-sign the Layer 1 addresses; if not, build a new Layer 1 profile.
3. Fill in the Layer 2 engine-version table.
4. Write a new Layer 3.

I haven't read either game's XEX, so their XDK versions are unknown. A later game is very likely on a later XDK [I].

---

## 2. Stages

Counts and addresses come from `27` (trace windows: menu frames 1201-1500, ch2 1801-2100, ch10 901-1200) unless noted.
Estimates are working days for me, including building comparison tooling, plus 1-3 short user runs per stage. They are rough [I].

### Stage 0 - Automated comparison and test harness (before Stage 1)
**Why first.** Every later stage is "prove it against the emulator, then switch". That needs a harness that runs the same route on
both renderers, collects logs, screenshots and per-draw comparisons, and reports differences without anyone watching.

**What exists [V].**
- Scripted input: `src/gow2_test_input.cpp`, `gow2_test_input_hook.cpp`.
- Chapter boot: `--gow2_chapter=N`; `scripts/chapter-sweep.ps1` boots each chapter, plays generic input and writes `out/sweeps/<stamp>/report.md`.
- Routes: `notes/21-test-routes.md`.
- Per-draw comparison: SDK `NativeDrawObserver` plus `tools/compare_draws.py` (job 229).
- Image tools: `split` / `splitdiff` debug views (job 226), `tools/shot_score.py`, `tools/stage_score.py`.

**What it adds (revised 2026-10-20 with the user's changes, D12).**
- **Same-frame comparison, not two runs.** Each checked frame is rendered by **both** renderers inside one run: the emulator
  executes the game's command buffer and our device renders the same draws from the same captured state. Run-to-run
  differences (timing, streaming, AI) therefore can't show up as false differences. The old project worked this way per draw
  (job 229, our replay next to the emulator in one frame) [V in the old notes]. The new project builds it in from the start.
- **Noise floor first.** Before anything is compared, the emulator is compared with itself:
  - the same captured frame executed twice, which should give exactly 0;
  - the same checkpoint across two runs, which shows what timing noise looks like.

  FLIP thresholds per route and per buffer are set from these numbers, written into `00-decisions.md`, and only changed there.
- **Intermediate HDR buffers, not only the final frame.** At each checkpoint the harness saves these from both renderers:
  - the lit scene (7e3/FP16), AO, bloom, and the DOF / motion-blur inputs, as **EXR** via DirectXTex, scored with **HDR-FLIP**;
  - the final LDR frame, as PNG, scored with LDR-FLIP.

  A difference is reported at the first buffer where it appears, not only at the end.
- **Reference library before Stage 5.** Once our device owns the state (Stage 5), the emulator can no longer render the same frame
  next to us, because it needs the original D3D to have built the command buffer [I]. So before Stage 5 starts we record emulator
  reference captures for every harness route and checkpoint: EXR buffers, final frames, draw logs, and RenderDoc captures of the key
  frames. These become the comparison target for Stages 5 and 6.
- **Baseline first.** Screenshots, buffers and timings (build, pack, boot, per-route frame times) are recorded **before** the SDK
  library pins are bumped (`30` section 3), so the bump itself gets compared against the baseline.
- **Separate diagnostic runs.** RenderDoc capture, Nsight Aftermath and D3D12 GPU-based validation each run in their own pass, because
  they interfere with each other and with timing. GPU-based validation only runs on a short route, since it is very slow.
- **The report** names the first draw, resolve or buffer that differs beyond threshold at each checkpoint, with FLIP error maps.
- **A queue for unattended runs** (see "Hardware and throughput"), with the disk-retention rule below.

**Disk retention for harness output (proposal P12).** All harness output lives in `out/harness/` and is never committed.
- **Kept forever:** run summaries and reports (small text).
- **Kept for the last 5 runs of each route:** full artefacts (EXR, PNG, logs).
- **Kept until the issue is closed:** artefacts of failing checkpoints.
- **Kept until you remove the tag:** anything tagged `keep`, plus the Stage 5 reference library.
- **Captures:** RenderDoc and Aftermath dumps are pruned after 14 days unless tagged.
- **Hard cap:** 50 GB for `out/harness/`. The oldest untagged artefacts are pruned first, and the report lists what was pruned.
- If you approve this rule, that approval is the say-so for this one folder only. Nothing outside `out/harness/` is ever pruned.

**Pass criterion.**
- Emulator-vs-emulator on the same frame scores exactly 0, and the two-run noise floor is measured and recorded.
- Ours-vs-emulator produces a readable report down to the first differing buffer.
- The baseline before the pin bump is stored.

**Estimate:** 6-9 days (was 4-6; same-frame dual rendering and the HDR buffer dumps add work) [I].

### Stage 1 - Resources (create / lock / unlock)
**Functions.**
- 19 'creation' + 4 'lock/fill' active at runtime (`27` section 1-2).
- **Plus the load-time creators the steady-state windows missed**: CreateTexture `8297C298`, Create* surface/buffer `8297C408`,
  vertex/pixel shader creation `82982F20`, CreateVertexDeclaration `829835A8` (`09` B2) [V names med/high].
- **Plus the XGraphics header setters**, which are the game's real create path: `829A0C40`, `829A0D78`, `829A00D8`, `829A0170`,
  `829A0E58`, `829A0DF0` [V].
- **Plus the lock core `82976600`** and the inline unlocks at `8259CE00` / `827BF6F0` [V].
- Several of the 19 are read-only queries (GetDesc, GetLevelDesc, XG layout helpers) and need no takeover, only reuse
  (section 3, item 2).
- **Regrouping:** SetShaderGPRAllocation `82983698` is not a resource function; it moves to Stage 5. The shader microcode helpers
  `8298FE90` / `829831F0` stay here as "shader resources".

**What our version does.**
- Calls the original (allocations and layout unchanged in Stage 1).
- Records each resource in our table: object, type, base/mip addresses, size, format, fetch constant at +28..+52 for textures.
- Marks contents dirty at Unlock and at the extra write paths found in section 3. Our owned copy is refreshed only when dirty.

**What it replaces.**
- Per-draw stream snapshots and epochs (`SnapshotStreams`, `gow2_native_snapshot_*` switches, job 211).
- Per-frame buffer hashing (`GetGuestBuffer`: full hash for small buffers, sampled hash plus a full check every 120 frames for large
  ones; `gow2_native_known_hashes`).
- The texture cache's sample hashes, round-robin full verify and settle logic (`native_texture_cache.h`: `SampleHash`,
  `FullTextureHash`, `OwnTexturesFrameEnd`; jobs 179/180/240).
- Some of the emulator stand-in textures (`ctx.guest_textures->GetTexture` fallbacks at `gow2_native_scene.cpp` ~4000 and ~4119).

All of these stay in code but become dead behind the switch. They are deleted only after the stage passes.

**Comparison.** At every draw, hash what our owned copy holds against what the current replayer would read. Log mismatches by
function, caller and address.

**Pass criterion.** No unexplained mismatches over the menu, the ch2 hospital and the ch10 firefight.

**Risks.**
- Game writes to resource memory with no Unlock (section 3, the deciding unknown).
- Streamed mips written by IO.
- Pool reuse (job 180: "the streamer reuses the pool") [T].

**Depends on:** Stage 0. **Estimate:** 7-11 days (detailed in `29`).

### Stage 2 - Clears, resolves, tiling
**Functions.** Clear `829825F0`, Resolve `8298A128`, BeginTiling `829890C0`, EndTiling `82989438`, predication `82988F50`,
TILE_FLUSH / HiZ event `82977A20`, XG tile helper `8299F7A8` (`27`; tiling names [I] this pass).

**What our version does.**
- **Clear:** clear our target (already done in the replay: `RecordClear`).
- **Resolve:** copy our target to our texture (already done: `RecordResolve` and the resolve replay, including the 7e3 clamp and
  16-bit encodings, jobs 172/188/206).
- **Tiling:** draw the frame once with no strips. We already merge the strips (`AliasKeyOf`, job 89). The originals stop running.

**Challenge to the grouping.**
- Tiling can be removed at the game level instead of emulated. The community Black Shading patch sets `GUseTilingMode=FALSE`
  (`82806A44`, `notes/19`) [V]. Then BeginTiling / EndTiling / predication are never called.
- **Recommendation: make "tiling off" a Stage-2 decision** rather than emulating predicated tiling. Test52 showed our renderer runs
  untiled (dark rooms, but no new breakage) [T].

**Replaces.** `NativeSceneSkipEmulatedResolve`, `gow2_native_skip_emu_resolves`, the EDRAM-ownership transfer passes (job 206),
if tiling is off.

**Comparison.** Our resolve outputs against the emulator's resolve copies, per resolve (the `split` / `splitdiff` tooling, job 226).

**Pass criterion.** Every resolve in the 3 scenes either matches the emulator within format precision or is explained.

**Risks.** The game reading back tiling fields 12708-12744 / 10932-10943 that game code writes (`27` section 3) [V].

**Depends on:** Stage 1 (resolve destinations become owned textures). **Estimate:** 3-5 days, or 5-8 if tiling must be emulated.

### Stage 3 - Draws and the draw-time flush
**Functions.**
- DrawIndexedVertices `82980B20`, DrawVertices `82980730`, BeginIndexedVertices `82980210`, BeginVertices `8297FD30`,
  DrawTessellated `82980F70` (water).
- The flush they share: `8298F600`, which calls the LOAD_ALU loader `8298F378`, constant upload `8298E5B8`, fetch flush `8298E460`
  and group emit `8298E220` (`09` draw path; callers of `8298F600` checked this pass) [V].

**Inline sites.**
- The **12 inlined EndVertices sites in 8 game functions** that fill ring space (memcpy, direct stores or a virtual callback) and
  set dev+48 = dev+13444: `82803830`, `82803A98`, `826FB630` (4 sites), `826FD7A8` (2), `822D0900`, `828044F0`, `8272B1F8`,
  `82336C00` [V, research pass B; `09` D listed 9 functions writing dev+48, including `82803F38`, which writes no vertices].
- `82803F38`, the one game function that writes a GPU packet itself (SET_BIN_MASK_LO) [V].

**What our version does.**
- No packets.
- Begin*Vertices hands out memory we own. dev+48 / dev+56 point into a scratch ring we own, so the inlined EndVertices memcpy and
  `82803F38`'s packet land harmlessly there.
- We read the inline data from there.

**Replaces.** `NativeSceneSkipEmulatedDraw`, the canvas register hook (`canvas_regs.h`, mid-asm hook at `0x82803B48`),
`FlushPendingVertices` guessing, and the emulator still drawing inline draws on 3D frames (architecture check Q3).

**Comparison.** The job-229 draw-by-draw comparison (`NativeDrawObserver` SDK hook, `tools/compare_draws.py`) [V]. It already
compares state, constants, fetch constants, index/vertex bytes per draw.

**Pass criteria.**
1. `compare_draws.py` reports identical inputs for every draw in the 3 scenes (job 229 already showed this for the light draws, ch2) [T].
2. **Pixel-parity checkpoint:** our frame against the emulator's frame, within the Stage 0 thresholds (D19: no named old bugs carried over).

**Risks.**
- Ring-pointer assumptions in game code.
- The LOAD_ALU literal path: c240-c255 are the only constants that differ from the emulator, by design (job 229) [T].
- Tessellation (water) is not handled by us today (`NativeSceneUnhandledCall` `82980F70`) [V].

**Depends on:** Stages 1-2. **Estimate:** 6-10 days, excluding the visual bugs.

### Stage 4 - Queries, sync, swap
**Functions.**
- Queries: query-result poll `829783A8` (up to 16,379 calls/frame in the menu; this pass [I name, T count]), sync/query object
  release `82977ED0`, query/fence write `82978120`.
- Sync/swap: IsFencePending `8298C6D0`, interrupt callback `8298C5B8`, KickOff/MakeSpace `8298C1A0`, BlockUntilIdle `8298C650`, ring-space query `82989A10`, swap/fence helper `8298CEC0`, Swap `8298D430`, BlockOnRingPosition `8298C2A0` (`27`).

**What our version does.**
- Answers the query-result poll immediately from our occlusion results. Real counts already exist: job 195,
  `gow2_native_occlusion`, `NATIVEOCC` [V].
- Fences complete at once (no ring is consumed).
- Swap presents our frame.

**Watch.** Our 60 fps fix hooks `8298CEC0` and reads PresentationInterval at dev+13596 (`src/gow2_frame_pace.cpp`) [V]. Frame
pacing must move into our Swap.

**Replaces.** The job-195 path that writes our counts into the emulator's report memory, and `SetNativeOcclusionAnswer` (SDK).

**Expected side effect.** The GPU-wait spin `82977688` (0.6-1.0 M calls/frame, `09`) disappears, freeing a CPU core [I].

**Comparison.** Query answers ours against the emulator's for the same query object; frame time and pacing logs (`FRAMETIME`,
`TICKRATE`).

**Pass criterion.** Same visibility decisions (no popping) over the 3 scenes, and stable 30/60 fps pacing.

**Depends on:** Stage 3. **Estimate:** 3-5 days.

### Stage 5 - State setters and the render-state table
**Functions.**
- 24 state setters and 3 constant setters active at runtime (`27`).
- SetShaderGPRAllocation `82983698` (moved here).
- The ~240 setters reached through the device's function-pointer tables at dev+64..547 (`09` C) [I].

**What our version does.**
- Keeps reading the device at each draw, as today.
- Stops calling the originals.
- **Logs every device field written that we don't use**: a field-coverage table over the 64 inline-written fields plus the setter
  targets.

**Replaces.** The last original D3D calls. After this, the emulator's command processor receives nothing.

**Comparison.** Field-coverage log plus `compare_draws.py`.

**Pass criterion.** Zero unhandled fields that affect output in a full chapter sweep.

**Depends on:** Stage 3. **Estimate:** 4-7 days.

### Stage 6 - Remove GPU emulation from the shipping build
**What.**
- A build option that leaves out Xenia's GPU pipeline (shader translator, render-target and texture caches, command processor
  execution).
- Our nrhi device stays.

**Important dependency [V].** Our device is created by the SDK's command processor today: `CreateNativeRhiDevice(D3D12CommandProcessor*)`
(`native_rhi_d3d12.cpp`). The presenter / swapchain and window also live in the SDK. So this stage needs a deeper SDK fork: a
"native-only" graphics system that owns the device, swapchain and presenter without Xenia's pipeline.

**Pass criterion.** Full chapter sweep (all 29 chapters, the user's runs) with the emulator absent.

**Estimate:** 5-10 days.

### Stage 7 - Freedom features (after Stage 6, each optional)
| Feature | What | Estimate | Notes |
|---|---|---|---|
| Any resolution | Arbitrary internal size, not only 2x/3x | 2-4 d | 2x exists (`gow2_native_res_scale`); scissor/viewport/sub-pixel rules worked out in jobs 168-174 [V] |
| FP16 instead of 7e3 | Lit scene in RGBA16F without the 31.875 clamp, where safe | 2-3 d | Needs a pass-by-pass check of what relied on 7e3 clamping (job 188-192 history) [T] |
| Real AA | MSAA at higher counts, or SMAA/TAA on our final image | 3-6 d | MSAA interacts with the game's sample-0 depth tricks (`19`) |
| HLSL pass overrides | Replace selected game shaders by hash with hand-written HLSL | 2-3 d infrastructure | Pass module already keys passes by hash (`gow2_native_pass.cpp`) [V] |
| HDR output | scRGB / HDR10 swapchain and our own tone map | 3-5 d | We already own the tone-map pass B6B6DD4E [V] |
| Ultrawide | Wider FOV and HUD layout | 5-10 d | Needs game-code changes (projection, HUD anchoring), not only rendering [I] |

**Temporal upscalers (DLSS / FSR / XeSS) - design (D12).**
- **Jitter:** a per-frame sub-pixel viewport offset, following the upscaler's sequence, applied in our device when it sets the viewport.
  No game shader changes. The old project's 0.25-px nudge showed a viewport offset reaches every draw [I].
- **Motion vectors:** UE3's velocity buffer (written for motion blur) for moving objects, plus camera reprojection from depth (current
  and previous view-projection) for everything else.
- **Placement:** the upscaler runs on the lit HDR scene **before** post-processing (bloom, DOF, motion blur, tone map) and before the
  HUD. Post and HUD then run at output resolution.

### Order: challenges and recommendations
- **Move "tiling off" first.** Deciding to run untiled (game patch `82806A44`) before Stage 2 removes a whole Xbox-only subsystem.
- **Stage 4 could go before Stage 3 for queries only.** Answering the query poll from our own results already works (job 195). The
  fence/swap parts must wait for Stage 3.
- **Pixel parity gates Stage 3.** Our visual bugs must be fixed or understood before the emulator stops drawing. Today that is the
  single biggest risk to the whole plan.
- **State (Stage 5) last is right.** Reading the device shadow already works, and the inline writers (81 functions) make it the
  stage with the widest surface.

**Total rough estimate:** Stages 0-6 ≈ 36-61 working days (Stage 0 now 6-9 d, plus the +2-4 d layer split, section 1b; tools in `30-tooling.md`) plus user test runs, not counting the visual bug work.

---

## 3. Unknowns to investigate before Stage 1
Being investigated now (two research passes over the game's code). Results go into `29-stage1-resources.md`.
1. **Resource memory written without a matching Unlock** [open]:
   - UE3 dynamic vertex/index rings.
   - BeginVertices pointers (the 9 inline EndVertices) [V that they exist].
   - CPU-skinned meshes and particles rewriting buffers between draws of one frame (job 211, epochs) [T].
   - Texture streaming writing mips into reused pool memory (job 180) [T].
   - XGraphics tile/copy calls `829A0C40` / `8299F7A8` / `829A0D78`.
   - Resolves writing texture memory [V].
2. **How the game creates resources: answered (research pass A, 2026-10-20; details in `29`).**
   - **Almost nothing goes through D3D Create\*** [V, T]. UE3 builds resources with XG header calls over memory it allocated itself:
     - XGSetTextureHeaderEx `829A0C40` (~16k calls over the 3 trace runs)
     - XGSetCubeTextureHeaderEx `829A0D78`
     - XGSetVertexBufferHeader `829A00D8` (~9.7k)
     - XGSetIndexBufferHeader `829A0170` (~1k)
     - binding to memory with XGOffsetResourceAddress `829A0E58` (~26.5k) and XGOffsetBaseTextureAddress `829A0DF0`
   - CreateTexture `8297C298` is used only for render/resolve targets (about 9 per run), and CreateSurface `8297C408` for EDRAM surfaces.
   - **Every CPU lock goes through one function, `82976600`** (VB lock `8280DFF0`, IB lock `828074A0`, texture locks via `8297B730`) [V, T].
   - **Unlock `829768C0` is also inlined in game code at `8259CE00` and `827BF6F0`** [V], so hooking the function alone misses two sites.
   - **Contents also change without any lock** [V]:
     - texture pool defrag `8280D150` (memmove plus re-header)
     - mip-streaming reallocation `8280B0C0`
     - dynamic VB/IB "discard" locks that put the same header onto new memory
     - Resolve writing its destination texture
     - two textures sharing one allocation (`8280B320` via `829A0CB8`)
   - **D3D never frees game resources:** the destructor `829763D0` ran 0 times [T]. UE3 frees memory itself in `828093B8` after a
     fence check.
   - Consequence: Stage 1's resource table is **keyed by memory range and re-keyed at every `829A0E58` / `829A0DF0`**, not keyed by
     header object. The "create" hooks are the XG header setters, not D3D Create\*.
3. **Which Xbox-only features the game depends on for correct output:**
   - **Predicated tiling:** can be turned off by the community patch (`82806A44`) [V]; whether output is fully correct untiled in
     our renderer is unproven (Test52: dark rooms) [T].
   - **EDRAM resolve/restore:** required. The game copies colour and depth out and draws them back (restore shaders E9E3FEBA and
     others, notes/14) [V].
   - **Hi-Z / Hi-Stencil:** `82977A20` and RB_HIZCONTROL writes in 9 functions [V]. Likely an optimisation only (culling), so it
     can be a no-op [I].
   - **Ring ownership:** owner thread dev+10888 written by 8 game functions [V]. Must stay plausible; a no-op otherwise [I].
   - **Shader microcode patching** `8298FE90` (called by UE3 `82809D08`) [V call]. If UE3 patches shader microcode at runtime, our
     hash-keyed shader pack can miss variants [I]. To check.
   - **Float 7e3 render targets and 24-bit float depth:** semantics we emulate today. The 20e4 depth-copy test (Test53) made things
     worse, so the exact console behaviour there is still unknown [T].

---

## 4. Checkpoints after Stage 1
**Continue if all of these hold:**
- The comparison mode shows **no unexplained mismatches** in the 3 scenes, and every explained one has a hook.
- The unlock-less write paths number **a handful** (on the order of 10 or fewer sites), each hookable at a specific function.
- Turning hashing off does not regress the picture (no stale textures or vertices in the user's runs).

**Stop and rethink if any of these happen:**
- Resource memory is written by **paths we can't hook** (for example IO/DMA straight into texture memory, or many scattered memcpy
  sites). Then "capture at unlock" can't be complete, and a hybrid stays necessary: our owned copies plus hashing as a safety net.
- Mismatches remain that we can't explain after one investigation round.
- The cost (days) of Stage 1 runs past about double the estimate. That would mean Stages 2-5 are likely underestimated the same way.

A second checkpoint sits in Stage 3: **pixel parity.** If our draws can't match the emulator's picture (within the Stage 0 thresholds), the plan
pauses there regardless of plumbing progress.

---

## 5. Current work in the meantime (recommendation only)
- **Default for playing:** the emulator renderer with the community patches at 2x (`Play-Native.bat emu2x`), clean in Test51 [T].
  It is what a player would use until the native renderer passes its sweeps.
- **Freeze replayer feature work.** No new switches or experiments on the replayer, except fixes needed to make comparisons possible.
- **Park the white ghost and the dark-room / black-gun bugs**, written up in `26-play-tests.md`. They are draw-execution bugs and
  return at the Stage 3 pixel-parity checkpoint. One exception worth considering before Stage 3: a focused, comparison-driven hunt
  (per-draw pixel diff against the emulator in one frame), because pixel parity gates the plan anyway. **[Superseded by D19, 2026-10-20: the ghost and dark-room bugs were the old replayer's, not the emulated renderer's; they are no longer open work. Stage 0 comparisons catch them as specific mismatches if they reappear.]**
- **Keep the test switches** (`showours`, `lightemu`, `aoown`, `nobsfix`, `bspatch`, `depthf24`, `xpos`, `d3dtrace`). They are
  comparison tools for later stages, all off by default.

---

## Hardware and throughput (standing practice from 2026-10-20)

**Measured [V, job 250, 2026-10-20 13:23].**
- AMD Ryzen 7 3700X, 8 cores / 16 threads.
- **16 GB RAM** (2 x 8 GB, 2400 MHz), about 8 GB free at idle, 5 GB pagefile.
- RTX 3080 Ti 12 GB, driver 32.0.16.1742.
- C: 718 GB free, D: 9.7 TB free.
- Not installed: sccache/ccache, RenderDoc, PIX, Nsight Graphics. cmake/ninja/clang come from the Visual Studio environment that
  the jobs load.

**RAM is the limit, not cores.** 16 GB has to cover parallel compiles, game instances and Windows. So:
- **Compiles:** renderer and tool sources on 14 threads (all but 2). The 221 recompiled game files (~1 MB each, the heaviest clang
  jobs) on 10-12 threads until peak memory per compile is measured. Then raise it as far as "no paging" allows.
- **Game instances:** start with **one** instance per unattended run. Measure the working set of an emulator run and of a native
  run. Allow two at once only if both fit with at least 3 GB to spare. Two instances also share one GPU (12 GB VRAM), so measure
  frame times with two before trusting their timing logs.
- **Every background job runs at below-normal priority** (`Start-Process -Priority BelowNormal`, or `[Diagnostics.Process]::PriorityClass`).
  **No heavy job starts while the user is play-testing** (a `gow2.exe` started by a Play-*.bat is running) unless the user says so.
  The job runner already refuses to start while `gow2.exe` runs [V].

**Builds.**
- **Already true [V]:**
  - Ninja builds use all 16 threads (`scripts/build.ps1`, "Building with 16 jobs").
  - The recompiled game code is its own target `gow2_recomp` with a precompiled header (`out/build/*/CMakeFiles/gow2_recomp.dir/cmake_pch.hxx.pch`).
  - A renderer-only change rebuilds only our sources: **about 23 s** (jobs 245/247).
- **Add:**
  - A compiler cache (sccache works with clang-cl and MSVC; installing it needs the user's OK, as it is a download).
  - Cap recompiled-code jobs as above.
  - A precompiled header for `src/native/*` (the scene file is about 6,000 lines with heavy SDK headers).
  - Split `gow2_native_scene.cpp` (planned for Gears 3 reuse anyway), so one edit doesn't recompile 6,000 lines.

**Shader pack.**
- **Already true [V]:** translation and compilation run in parallel across all cores (`tools/ucode2hlsl/run_all.py --jobs`). The
  last full rebuild translated 3,141 shaders in 49 s, and compiled DXIL + SPIR-V + FXC for all of them in 270 s total (job 249).
- **Add:** a cache keyed by (shader hash, translator version hash, compiler version, target). Only shaders whose translation
  changed get recompiled; an unchanged translator gives a near-instant pack rebuild.

**Testing.**
- Stage 0's harness queues route runs and chapter sweeps to run **unattended** when the user is away or says so. It collects logs,
  screenshots, per-draw comparisons and a summary report per run.
- Independent tests run in parallel (two instances at most), only after the memory measurement above allows it.
- Emulator and native runs of the same route run **one after the other**, not side by side, so their timing logs stay comparable.

**Diagnosis.**
- Use GPU capture tools instead of theorising:
  - **PIX on Windows:** pixel history ("which draw last wrote this pixel and why") against D3D12.
  - **RenderDoc:** D3D12 and Vulkan frame capture.
  - **Nsight Graphics:** GPU frame debugging and profiling.
  - Installing them needs the user's OK.
- Combined with the per-draw emulator comparison, a ghost pixel can be traced to the exact draw that adds it, in our frame and in
  the emulator's frame.

**Working style.** While a build or run is in progress, keep doing analysis or planning. Don't wait idle on timers.

**Baseline times (before any of the changes above) [V, job logs 2026-10-20].**
| Task | Time | Source |
|---|---|---|
| Renderer-only rebuild (one file in `src/native`) | ~23 s compile, ~55 s whole job | jobs 245, 247 |
| SDK rebuild + install, then game build | ~70 s SDK + ~23 s game, 2 min 05 s whole job | job 248 |
| Full rebuild of the recompiled game code (221 files) | ~110 s compile on 16 threads | `out/build/release-vk-fast1` object timestamps (Oct 18) [I] |
| Shader pack, full | 49 s translate + 270 s compile, 5 min 26 s whole job | job 249 |
| Play test (user-driven) | 2-5 min per run, plus reading the logs | Tests 46-58 |
These get measured again after each change above, and the new numbers go into this table.

---

## 6. Decisions for the user (with my recommendation)
1. **Graphics API:** D3D12 first, with Vulkan kept compiling through nrhi but not tested per stage. **Recommendation: D3D12 only
   until Stage 6**; Vulkan parity after.
2. **Platforms:** Windows only for now. **Recommendation: yes**; Linux/Proton later if Vulkan parity lands.
3. **Tiling:** emulate predicated tiling or run the game untiled via the `82806A44` patch. **Recommendation: untiled.**
4. ~~**Pixel-parity work before or during Stage 1-2:** run a comparison-driven ghost/dark-room hunt in parallel with Stage 1 planning.~~
   **Superseded by D19:** no hunt; those were old-replayer bugs.
5. **Default renderer for players meanwhile:** **Recommendation: emulator at 2x with patches.**
6. **First freedom features:** **Recommendation: any resolution, then FP16 lighting, then AA.** HDR and ultrawide later (ultrawide
   needs game-code work).
7. **Shipping without the emulator needs an SDK fork** (Stage 6). **Recommendation: accept**, and keep the fork's changes inside
   `patches/rexglue-sdk.diff` as today.
8. **Install sccache and a GPU capture tool (PIX, plus RenderDoc).** Both are downloads, so they need your OK. **Recommendation: yes. DECIDED 2026-10-20: yes** (installed when work starts, not during planning).
9. **Unattended runs:** allowed when you are away or say so, never while you play-test, always at low priority. **Recommendation: yes, one instance at a time until memory is measured. DECIDED 2026-10-20: yes.**

10. **Layer split (section 1b):** do it in Stage 0, before Stage 1 adds code, or only when Gears 3 starts. **Recommendation: Stage 0**;
    splitting later means untangling five stages of new code instead of one file.
11. **Tooling (`30-tooling.md`):** adopt the Stage 0 set (FLIP, RenderDoc API, markers/labels, DRED, Aftermath, nightly validation,
    Catch2, DirectXTex) and bump the SDK's outdated pins. **Recommendation: yes**, with the downloads listed there.

---

## 7. Decisions log
All decisions now live in **`notes/00-decisions.md`**, the single source of truth.
- Section 6 above keeps the reasoning behind each recommendation.
- Once something is decided, the entry in `00` is what counts. If any note disagrees with `00`, the note gets corrected.
- Every new decision is added to `00` the same day: number, date, status, and a link to its details.

## 8. Backup, remote and branch strategy (proposal P10)
**State:** read-only job 251 (git branches, remotes, tags, size) is queued; this section gets its facts once it runs. **Rule R1
stands: nothing is pushed without your explicit say-so.**

**Branches.**
- **`main` is always playable.** Every commit on main must boot, reach the menu and pass the quick harness route with the default
  renderer (today the emulator at 2x with patches).
- **One branch per stage:** `stage/0-harness`, `stage/1-resources`, and so on. Research jobs live there, not on main.
- **A stage merges to main only when** its switch is still off by default, the Stage 0 harness shows no regressions, and you have
  done your play-test run.
- **Small fixes** use short `fix/<name>` branches.

**Tags.**
- `stage-N-start` when a stage branch is cut.
- `stage-N-pass` when its pass criterion is met.
- `stage-N-default` when its switch becomes the default.
- `playable-YYYYMMDD` for any build you want to be able to go back to.

Tags are local, so they cost nothing.

**Backup, without GitHub.** A `git bundle` of all branches and tags into a folder you choose, ideally another drive or a synced
folder. It is one file and can be restored with `git clone`. Do it after each merge to main, and weekly.

**Backup, with a remote (only if you decide).** A *private* GitHub repo. Only our code goes there: game files and game-derived files
stay out by rule R3.

**What isn't in git and how to rebuild it:**
- **Rebuilt from the game plus our tools:** the shader pack, `generated/` and the build directories.
- **Must be backed up separately:** the SDK checkout. Its changes are `patches/rexglue-sdk.diff`, which *is* in git, but the pinned
  base commit must be recorded in the repo too. Job 251 reports the SDK checkout's commit and remote so we can check this.

## 9. Test coverage beyond the campaign
**What exists [V]:** `scripts/chapter-sweep.ps1` boots each campaign chapter with scripted input and writes a report.

**Goal:** every mode below gets a scripted harness route, so Stage 0's comparison and every stage's pass criterion cover it. Each
needs a way to boot into it directly, the way `--gow2_chapter` does for the campaign. That's research for each mode [I].

| Area | Why it is a separate risk for the renderer | Route |
|---|---|---|
| **Split-screen co-op** | Two scene renders per frame, each with its own viewport and scissor and half-height targets. It stresses our resolve, scissor and scaled-resolution rules (jobs 168-174) twice per frame. | Start co-op on ch1 and ch10 with a second scripted pad. Test input may need a second controller slot [I]. |
| **Multiplayer maps** | Different lighting, many more dynamic lights, other weather and post settings, and 10 players. | Private/local match on each MP map with bots, fly-through camera. Map list from job 251 (`MP_*`). |
| **Horde** | High enemy counts, many lights and decals, long sessions that exercise texture-pool churn (Stage 1 defrag path). | Waves 1, 10, 50 on 2-3 maps, 20 min each, unattended. |
| **In-engine cutscenes** | Camera cuts, depth of field, letterbox bars, streaming spikes. | Campaign routes that hold still through each cinematic. |
| **Movies (FMV)** | A separate path: video frames uploaded as textures and drawn full-screen. It must keep working when the emulator is gone (Stage 6). | Intro and chapter movies, plus skip. File types from job 251. |
| **Menus and loading** | Front-end, pause, scoreboard, options, the loading screen while levels stream, the save icon. Most of the HUD/UI pass (`gow2_native_ui.cpp`) lives here. | Scripted walk through every menu page, and load screens between chapters. |
| **Device events** | Controller disconnect, sign-in prompts, the guide overlay: the game draws system UI over everything. | Simulated through test input [I]. |

**Pass criterion for each stage, from now on:** campaign sweep plus every row above. FLIP under the agreed threshold against the
emulator, no new validation errors, no crashes.

## 10. Pipeline-cache warm-up (no shader stutter)
**Today [V]:**
- Compiled shader bytecode is cached on disk by content hash (`native_rhi_d3d12.cpp`, "Shader bytecode cache").
- Pipeline state objects are created **on first use, on the render thread** (`CreateGraphicsPipelineState` inside nrhi). There is no
  `ID3D12PipelineLibrary` and no Vulkan `VkPipelineCache` on disk. So each new combination costs a driver compile mid-frame. The pack
  holds 3,141 shaders.

**Plan:**
1. **Measure first (Stage 0).** The harness logs every PSO compile with its duration and the frame it landed in (`NATIVEPSO`), and
   counts frames over 2x the median frame time.
2. **Pipeline manifest.** Each harness route writes the set of pipeline keys it used (shader pair + state). The merged manifest from
   all routes ships with the build.
3. **Warm-up.** At boot and during loading screens, compile the manifest on background threads: all cores but 2, below-normal
   priority.
4. **Disk cache.** Use `ID3D12PipelineLibrary1` on D3D12 and `VkPipelineCache` on Vulkan, keyed by GPU and driver version so a driver
   update rebuilds it.
5. **A miss still never blocks.** On a cache miss, draw with a generic fallback pipeline for that frame, or skip the draw and log it,
   while the real PSO compiles. Fallback versus skip is decided per pass in Stage 3.

**Stage:** measurement in 0; the manifest and disk cache in 3, when our draws become the only draws. **Estimate:** 3-5 d [I].

## 11. PC display behaviour
**Today [V]:**
- The SDK has `fullscreen`, `vsync`, `window_width/height`, `present_letterbox`, a safe-area option and `video_mode_refresh_rate`.
- The D3D12 presenter uses the flip model with `ALLOW_TEARING` when the system supports it.
- There is no HDR colour space anywhere (no scRGB/HDR10 code).

| Item | Plan | Stage |
|---|---|---|
| **Window modes** | Windowed and borderless full-screen. With the flip model plus tearing, borderless performs like exclusive full-screen on Windows 10/11 [I], so no exclusive mode. | 6 |
| **Resize** | Resizing the window rebuilds the swapchain only; the internal render size is its own setting. Covered by the "any resolution" feature. | 6 / 7 |
| **Alt-tab and minimise** | Keep rendering paused while minimised and keep the device alive; D3D12 has no device-lost on alt-tab. Audio follows the game's own pause rules [I]. Test: alt-tab 50 times in one route. | 6 |
| **VRR (G-Sync / FreeSync)** | Needs tearing allowed, vsync off and our frame limiter (`gow2_fps`) instead of vsync. Depends on game logic at high frame rates (section 14). | 6 |
| **HDR** | scRGB or HDR10 swapchain and our own tone map (we already own pass `B6B6DD4E`), plus a calibration screen. | 7 |
| **Ultrawide / other aspect ratios** | Projection and HUD anchoring are game code (Layer 3), not only rendering. Letterbox stays the safe default. | 7 |
| **Multi-monitor, DPI, refresh rate** | Choose the monitor, handle per-monitor DPI, follow the monitor's refresh rate. | 6 |

## 12. GPU vendor test plan
**Hardware:** only NVIDIA (your RTX 3080 Ti) is on hand [V, job 250].

| Vendor | How we test | What tends to differ [I] |
|---|---|---|
| **NVIDIA** | Your PC: every stage, every route. | Primary target; most forgiving about barriers and undefined behaviour, so it can hide bugs. |
| **AMD** | Testers you trust, running a harness build that writes a report bundle. Or a cloud Windows VM with an AMD GPU, if you want to pay for it. | Stricter about resource barriers, descriptor ranges and uninitialised data; programmable sample positions and depth-format support may differ. |
| **Intel (Arc / iGPU)** | Testers, or an Intel laptop. | Precision and wave behaviour in shaders, typed UAV formats, driver PSO compile times. |
| **WARP (Microsoft's software rasterizer)** | Nightly, on your PC, in the background. | Not a vendor but a strict, deterministic reference. A frame that matches WARP but not the GPU points to a driver or undefined-behaviour issue, not our logic. |

**Every build logs a capability line at boot (`NATIVECAPS`):** vendor, driver, shader model, sample-position tier, typed UAV formats,
and the pipeline-cache identifier. Features with a vendor-specific path (e.g. `SetSamplePositions`) must have a fallback that is tested
on NVIDIA by forcing it off.

**Minimum spec** is set at Stage 6 from what passes. FLIP thresholds may need a per-vendor tolerance; any per-vendor tolerance goes
into `00-decisions.md` with its reason.

## 13. Crash and GPU-dump reporting
**Today [V]:**
- `src/gow2_crash_watch.cpp` writes a text report and a small minidump (stacks, registers, modules; not the 4 GB guest heap).
- DRED is on only when the debug layer is on, and its readback is only in Xenia's command processor (`30-tooling.md`).

**Plan (Stage 0, kept in every build):**
- **One folder per crash:** `out/crashes/<timestamp>/` with:
  - the minidump
  - the log tail
  - the options file
  - build id (git hash and stage switches)
  - the GPU and driver line
  - the last 64 pass markers our device issued
- **On a device-removed:** also DRED breadcrumbs and page-fault address (all vendors) and, on NVIDIA, an Nsight Aftermath dump.
- **A single zip** you or a tester can send. Nothing is uploaded automatically; any automatic upload would be a later decision of
  yours.
- **The harness** treats a new crash folder as a failed run and attaches it to the report.

## 14. Non-renderer items for "100% native" (listed only, not planned)
These are outside the renderer roadmap. Each needs its own investigation and plan later.
1. **Native audio (XMA).**
   - Today the SDK emulates the 360 audio path: the kernel XMA calls and `src/audio/xma_decoder.cpp` via FFmpeg [V files].
   - Native would mean hooking UE3's audio device to XAudio2 or SDL directly, with XMA decoded natively or converted once on the
     user's machine.
2. **Game logic at high frame rates.**
   - `gow2_fps` exists [V], but UE3 game code on the 360 assumed about 30 fps in places [I]: physics, animation, cutscene timing,
     input, AI.
   - Needed before VRR or high-refresh output means anything.
3. **Online services replacement.** Xbox Live, XNet and XOnline are linked [V, static-libraries header]. Covers:
   - matchmaking and lobbies
   - system link
   - achievements and leaderboards
   - presence

   Options range from LAN/system link over IP to a community relay.
4. **Distribution and legal model.**
   - What we may ship (only our code and patches, never game files: R3), how users supply their own copy, naming and trademarks, and
     the licenses of everything in the binary (rexglue's license with its Xenia portions, FFmpeg, and the SDKs in `30`).
   - Needs a real lawyer's view before any public release. I'm not one.

## Status
| Stage | Plan | Comparison mode | Passed | Switched on |
|---|---|---|---|---|
| 0 Harness | here | - | - | - |
| 1 Resources | `29-stage1-resources.md` (in progress) | - | - | - |
| 2 Clears / resolves / tiling | here | - | - | - |
| 3 Draws | here | job 229 tool exists | - | - |
| 4 Queries / sync / swap | here | - | - | - |
| 5 State | here | - | - | - |
| 6 No emulator in shipping build | here | - | - | - |
