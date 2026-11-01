# 79. Modern ambient occlusion (XeGTAO), and what it led to: motion vectors, SMAA T2x, Gears 3 (2026-10-31 - 11-01)

## Summary (read this first)

The note began as a plan (sections 1-6) and then records the work in order (7-31). Facts are marked [V] verified,
[I] inferred, [G] guessed, as in the other notes. All of it is shared layer 0 (`Gears2-Native/src/layer0_xenos`), so
Gears 3 gets it too; per-game settings are in `gow2_app.h` / `gow3_app.h`, and the launcher in `cogbox/`.

**What was added** (all user-tested at 4K HDR on a high-end NVIDIA GPU, D3D12; Vulkan where noted):

| Feature | Switches (launcher option) | Sections |
|---|---|---|
| **XeGTAO** (Intel, MIT) at full resolution. Gears 2: replaces the game's own half-res AO in its own composite, and is added (multiplied before the fog pass) where the game turns AO off. Gears 3: the same where a level has real-time AO (ch1, ch12), added gently where AO is baked (ch8). The game's own AO chain is skipped while ours is shown (cost about equal to the original). | `--gow2_ao=gtao`, `--gow2_ao_quality`, `_radius`, `_power`, `_add_radius`, `_add_power`, `_skip_game`, `_everywhere`, `_fog_ps` ("Ambient occlusion": Off / Game's own / XeGTAO high / ultra; default XeGTAO high in Gears 2, Game's own in Gears 3) | 7-19, 25 |
| **Developer A/B keys** F9 (AO: XeGTAO / none / game), F10 (views), F11 (quality) | `--gow2_ao_debug_keys`, `--gow2_ao_show`, `--gow2_ao_debug` | 12 |
| **DLSS / FSR / XeSS fixes:** depth copied before the game clears it (vectors had lost all translation), reversed-Z in the reprojection, the SDKs' inverted-depth flags | `--gow2_upscale_depth_at_resolve` | 20-21 |
| **Characters' own motion** in the motion vectors (Gears 2), from the game's velocity-pass transforms | `--gow2_upscale_character_motion` | 21 |
| **SMAA T2x:** SMAA with two-position jitter and a temporal resolve, as an "upscaler" kind (D3D12 and Vulkan; Gears 2 and 3) | `--gow2_upscaler=smaa_t2x` ("Anti-aliasing: SMAA T2x (temporal)") | 22, 26 |
| **Ultrawide** checked; AO detection and the character mask fixed for its 1280x550 scene | - | 23 |
| **Gears 3 scene camera:** a planar-reflection camera was taken for the view, which had left DLSS / FSR / XeSS / T2x without jitter in whole levels | - | 28 |

**Bugs found and fixed on the way** (besides the above):
- The AO chain stayed "known" after the game stopped drawing it, so other effects reusing that EDRAM were skipped, causing a device loss in Gears 3 cutscenes (s.30).
- One-off false AO matches are now ignored: a 3-frame streak is required (s.27).
- Nothing of ours touches multisampled targets / depth (s.29).

**Tests:** `tests/layer0_ao_test.cpp` (camera maths, the Gears 2 measured camera, the Gears 3 reflection camera), and in `tests/layer0_upscaler_test.cpp` reversed-Z motion and per-character motion. 116 test cases pass.

**Open / later:** Judgment's AO (s.16 item 4); PCSS for dynamic shadows (s.16 item 5); XeGTAO denoise tied to the AA choice (s.16 item 3); split-screen untested.

**Developer switches kept** (all off by default):
- `--gow2_ao_log`, `--gow2_ao_trace_frames` / `_class` / `_vs`, `--gow2_renderer_hlsl_dump_hashes`;
- `--gow2_upscale_capture_frames` with `--gow2_upscale_capture_raw_class`;
- the one-time "multisampled" warnings.
- The one-off camera-constant scan of s.21 was removed (s.31).


## 1. How a Gears 2 frame reaches the native renderer
1. **The ring buffer.** The guest writes `CP_RB_WPTR`, which wakes `OurGpuSystem::WorkerMain`
   (`src/layer0_xenos/gpu_system.cpp:304,335`). It calls `OnPrimaryRange`.
2. **PM4 parsing.** `Pm4Execute::OnPrimaryRange` (`pm4_shadow.cpp:636`) runs the PM4 processor (`pm4.cpp`; `DRAW_INDX` at :404, swap at :277).
   - With `--gow2_pm4_split`, draws are queued into `RenderPipe` and replayed on the renderer thread.
3. **Draw dispatch.** `observer_hub.cpp:75` sends each draw to `ShadowRenderer::OnDraw` (`renderer/shadow_renderer.cpp:1953`). From there:
   - an EDRAM copy (`RB_MODECONTROL==6`) goes to `Resolve()` (:8615), then to `ResolveDepth` (:8889) or `ResolveColour` (:9031);
   - everything else goes to `Draw()`. Shaders are keyed by the XXH3 of the ucode (`vs_hash`/`ps_hash`, :5468).
4. **Swap.** `OnSwap` (:2212) runs `UpscaleEndFrame`, then `PresentOurs` (gamma, SMAA, RCAS, HDR encode), then `Submit`.
5. **Render targets.** One host texture per `RenderTargetKey{base, storage class, pitch, msaa}` (`render_target_cache.h:44`), scaled by the resolution scale.
   - Scene depth is D24FS8 at EDRAM base 0, pitch 1280. On the host it is `D32_FLOAT_S8X24`, holding guest depth × 0.5 (`float24_remap`).
   - It is sampleable as an SRV, plane 0.
6. **Backends.** Both implement `gpu::Backend` (`include/xenos/renderer/gpu.h:407`).
   - Built-in compute passes work on both APIs: HLSL in `renderer/builtin_shaders.h` → `ShaderCache::GetBuiltin` → DXC → `CreateComputePipeline` → `Dispatch`.
   - The shared compute layout allows 16 root constants, SRVs t0-t3, and UAVs u0 and u1 (`gpu.h:195`, `d3d12_backend.cpp:4166`).

**Gears 2 ch02 frame, from `notes/35`:**

| Draws | Pass |
|---|---|
| 1-163 | Depth prepass |
| 165-537 | Base pass into rt0 (7e3) |
| — | Copy 0: scene-depth resolve |
| 538-934 | Occlusion queries, lights and shadow maps |
| 935-1127 | Half-res effects: the **AO chain** (copies 8-13; copy 13 = k_16_16_FLOAT, x = AO, y = depth, with reprojected history), then velocity (copy 14). **Title Update 6 numbering differs: see section 7 (copies 5-10, final AO = copy 10)** |
| 1128 | Copy 15: HDR scene resolve. This is where the upscaler hooks in (`ResolveColour` :9092-9109) |
| — | Bloom, then tone map + UI |

**The model for a mid-frame host pass** is the upscaler (`RunUpscaler`, :4417):
- detect a resolve (`IsSceneColourResolve`, :4407);
- run compute passes on the scaled render targets;
- swap `resolve_source`, so the game's later passes read our output.

## 2. Algorithm choice: XeGTAO (recommended)

| | XeGTAO (Intel, MIT) | HBAO+ (NVIDIA GameWorks) |
|---|---|---|
| Form | Plain HLSL compute, 3 passes | Closed binary SDK |
| APIs | Compiles through the existing DXC path for D3D12 and SPIR-V | D3D11/D3D12 only, no Vulkan |
| Normals | Works from depth alone | Prefers normals |
| Fit | Fits `builtin_shaders.h` | Extra redistribution terms; does not fit the shared compute layout |

Decision: implement XeGTAO. Make the code an "AO provider" seam, so that an HBAO-style horizon pass could be added later as another preset if wanted.

Gears 2 is forward-lit and has no normal buffer (rt0 only). Normals are rebuilt from depth (XeGTAO's built-in path).

## 3. Integration strategy for Gears 2: replace the game's AO at its final resolve
Gears 2 already computes, stores and **applies** AO itself. The safest replacement keeps the game's own composite (its intensity curve, its bilateral upsample using the y-depth channel, and where in the lighting it is applied) and swaps only the AO values:

- At the `ResolveColour` of the game's **final AO target** (k_16_16_FLOAT, half scene width), run XeGTAO on the scaled scene depth. Then pack the result into the same format:
  - x = our AO, sampled down to the AO target size with a depth-aware filter;
  - y = the game's own depth value, copied from the source target.
- Hand that to the resolve through `resolve_source`, the same way the upscaler does.
- Later, optionally, `Skip()` the game's own AO draws to recover their GPU time, once the replacement is proven.

Why not composite our own AO onto scene colour at copy 15? By then the light passes and translucency are already in that colour. Multiplying AO in would darken direct light and effects. That is wrong, and it is the classic bad-SSAO look.

## 4. Phases

### Phase 0: Investigation (diagnostics only, no feature code) - done 2026-10-31, results in section 7
- Run `--gow2_draw_log_frames` and `--gow2_upscale_probe` on ch02 frame 1500 and ch23. Find out:
  - the PS hashes of the AO chain draws (935-1127);
  - **which copy is the final AO** that the composite reads (texture fetches of copy 13's address), and which copies are history ping-pong;
  - which draw applies the AO, and how (a multiply in an ambient pass? inside the lighting?);
  - the exact packing of the y channel (linear or hardware depth? scaled how?).
- Test `AmbientOcclusion=False` through `--ue3_ini_edits` (the job-258 method: copies 8-13 should disappear). This becomes the "Off" mode.
- Settle the unverified "G-buffer MRT at draw 164" (`notes/35:28`) with a resolve log. A real normal target would improve quality.
- Write the findings into this note, with hashes and addresses, as [V]/[I] facts in the project's style.

### Phase 1: Camera and depth inputs available without an upscaler - done 2026-10-31, see section 9
- Today `up_vp_` (c0-c3 capture, `FindCameraMatrix` :4254) is only filled when `up_active_`. Split the camera capture from the upscaler: a new flag that captures it when AO or the upscaler needs it.
- Derive the XeGTAO constants from the captured matrix with the existing helpers in `include/xenos/renderer/upscaler.h`: `FromConstantsTransposed`, `PerspectiveParams`, `DepthAtInfinity`. The XeGTAO constants are `NDCToViewMul/Add`, `DepthUnpackConsts` and `CameraTanHalfFOV`, with an infinite far plane.
  - Remember the ×2 for `float24_remap`, as `RunUpscaler` :4478 does.
- Add a unit test in `tests/layer0_upscaler_test.cpp` style for the constant derivation.

### Phase 2: XeGTAO as built-in compute passes - done 2026-10-31, see section 10 (in `xegtao_shaders.h`, not `builtin_shaders.h`)
**Passes:**
1. **Depth prefilter.** Hardware depth to a viewspace-depth MIP chain (5 mips, R16F/R32F). Run one dispatch per mip using `uav_mip`, because the layout has only 2 UAVs. That costs little. Read the scene depth with `PrepareDepthTarget(...)` first, the way `ResolveDepth` does.
2. **Main pass.** Inputs are the depth MIPs (t0). Outputs are AO (u0) and packed edges (u1). Noise is the in-shader Hilbert/R2 index, seeded by frame.
3. **Denoise.** 1-2 dispatches, ping-ponging the edge-aware blur.

**Constants.** Keep the per-frame values in the 16 root constants: viewport size, pixel size, NDC→view, depth unpack, radius, falloff, noise index. Put quality-preset values in as `#define` variants (Low/Medium/High/Ultra = slice and step counts), each with its own built-in name. This needs no root-signature change in either backend.

**Upscaler interaction.** With DLSS/FSR/XeSS, the depth is jittered. Use the de-jittered depth path (`--gow2_upscale_dejitter_depth`, `ResolveDepth` :8943), or pass `up_jitter_` as an offset.

**Resolution.** Run at the scene-depth scale (full scaled res), or at half res as the "Performance" preset. Notes/27 says the AO history must stay at a fixed fraction of the main target.

### Phase 3: Hook into the game's AO resolve - done 2026-10-31, see section 11
- In `ShadowRenderer::ResolveColour` (next to the upscaler block at :9095), add an `IsGameAoResolve(info, *rt)` predicate. It matches on format, half-scene size, ordering (after the scene-depth resolve in the same frame), and the Phase-0 shader hash of the last draw into it.
  - Use one replacement per frame, but allow one per viewport for split-screen.
- Run `RunGtao()`, then a pack pass (x = AO, y = the game's channel copied from `rt->texture`). Set `resolve_source` to the result.
- Clear `tex_memo_valid_` after any mid-frame binding change, as SMAA does (:3309).
- Then decide the game's temporal history (copies 9-12). Two options:
  - feed our AO into the history too, so the game's reprojection keeps stabilising it;
  - skip the history draws, because XeGTAO is already denoised.

  Choose by looking at captured frames.

### Phase 4: Options, end to end
- **Cvars** (in the `--gow2_*` style): `--gow2_ao=game|gtao|off`, `--gow2_ao_quality=low..ultra`, `--gow2_ao_radius`, `--gow2_ao_strength`.
- **Launcher.** Add an "Ambient occlusion: Off / Game / XeGTAO (quality)" setting in `cogbox/src/gears_settings.cpp`:
  - group `"inifx"`, next to MotionBlur/DoF at :245-250;
  - "Off" adds `|SystemSettings|AmbientOcclusion=False` to the `--ue3_ini_edits` list (:881-883);
  - the XeGTAO modes pass the cvars;
  - add strings to `cogbox/lang/*.txt`.
- **In-game menu.** Add the entry in `ingame_menu.cpp`. It applies on restart, like the other renderer options.

### Phase 3b: Live A/B debug toggle (in game, no restart) - done 2026-10-31, see section 12
The live toggle is cheap. In the "game" and "gtao" modes the game's own AO chain still runs. The only thing that
changes is whether we swap `resolve_source` at the AO resolve. That is a per-frame branch on the renderer thread.

- **Hotkeys.** Register them with `rex::ui::RegisterBind` (the same API as the F1 menu bind, `ingame_menu.cpp:542`). They set an atomic that the renderer reads once per frame:
  - **F9** cycles the AO mode: Game → XeGTAO → None. "None" packs x = 1.0, so it runs live, unlike the ini switch, which needs a restart.
  - **F10** cycles a view: normal → **AO buffer only** (greyscale, full screen, blitted in `PresentOurs` before the HUD) → split screen (left half game AO, right half XeGTAO).
  - **F11** cycles the XeGTAO quality preset. The PSOs are prebuilt for every preset, so switching does not hitch.
- **Label.** A small ImGui line shows the current mode, preset and the AO pass's GPU time. It reuses the `fps_bar.cpp` window code.
- **Default off.** All of this sits behind `--gow2_ao_debug=true`, so players never see it.
- **Menu.** An optional "AO (debug)" entry in the in-game menu (F1 / Back+Start) that applies live. The menu today only writes changes and applies them on restart (`ingame_menu.cpp:473`), so this needs a small "live item" path.

### Phase 5: Parity and the other games
- **Vulkan:** the passes are built-in compute, so they should work unchanged. Verify.
- **Gears 3 and Judgment:** reuse layer 0. Only the AO-resolve detection differs. Gears 3 notes/27 lists the AO artefacts at 2x/3x scale; the replacement fixes those as a side effect.
- **Gears 1:** it has no SSAO. It needs the "add" strategy: composite before the light passes. That is a separate design, out of scope here.

## 4b. Building and testing (dev builds alongside an existing COGBox install)

**Building**
- The port is built with `Gears2-Native\tools\build.ps1 -Tu6`. It configures, runs codegen, then compiles, and the output is `build\RelWithDebInfo-tu6\gow2.exe`.
- After the first full build, only layer-0 files change (`shadow_renderer.cpp`, `builtin_shaders.h`). Ninja and sccache therefore rebuild just those objects and relink.
- The HLSL is compiled at run time by DXC and cached. Shader edits need no new SDK and no regeneration.
- The launcher (`cogbox/build.sh`) only needs rebuilding in Phase 4, for its new setting.

**Testing, step 1: kept separate (Phases 0-3)**
- Run the dev build with `Play.bat --gow2_ao=gtao --gow2_ao_debug=true`.
- This uses the repo's own `game\` and `out\userdata`. It never touches the COGBox install or its saves.
- Logs go to `out\play-logs\<time>\`.

**Testing, step 2: in the existing COGBox install, with its saved settings (Phase 3b and later).** The launcher already supports this without code changes:
1. In COGBox's `settings.ini`, under `[gears2]`, set `PortTu = <repo>\build\RelWithDebInfo-tu6\gow2.exe`. This is the developer override in `games.cpp:137-140`. COGBox then starts the dev exe with the player's normal settings, saves, DLC and mods.
2. Put the experimental switches in `extra-args.txt` next to COGBox.exe, one per line (`page.cpp:95`, appended in `gears_settings.cpp:1097`). For example:
   - `--gow2_ao=gtao`
   - `--gow2_ao_debug=true`
3. To go back, delete the `PortTu` line and empty `extra-args.txt`. Back up `settings.ini` before the first try.

**Compatibility with old builds**
- The launcher shows options only when the exe contains the cvar name (`exe_has`, `gears_settings.cpp:348`).
- So the new AO setting stays hidden for the release exe and appears for the dev exe.
- Existing saved settings are not changed.

**Recommendation:** keep them separate (step 1) while the AO resolve detection is being developed. Switch to the `PortTu` route once the debug toggle works, to test real play sessions.

## 5. Critical files
- `notes/79-modern-ao-plan.md` (this note)
- Later phases:
  - `src/layer0_xenos/renderer/shadow_renderer.cpp`: camera capture split, `IsGameAoResolve`, `RunGtao`, the hook in `ResolveColour`
  - `src/layer0_xenos/renderer/builtin_shaders.h`: XeGTAO HLSL
  - `include/xenos/renderer/upscaler.h`: constant helpers
  - `cogbox/src/gears_settings.cpp`
  - `ingame_menu.cpp`
  - `THIRD_PARTY_NOTICES.md`: the XeGTAO MIT notice

## 6. Verification
- **Phase 0 (done, section 7):** draw and resolve logs show the AO chain; the ini-off test removes copies 5-10 (Title Update 6 numbering).
- **Unit tests:** constant-derivation test in `tests/`, run through `tools/build.ps1`.
- **Visual:** A/B captures with `frame_capture`/`image_dump` (game AO vs XeGTAO vs off) at 1x, 2x and 4x scale, and with DLSS/FSR on (jitter). Check ultrawide 1280x550 and split-screen.
- **Stability:** a `tools/chapter_sweep.ps1` run over all chapters on D3D12 and Vulkan, with no new failures.
- **Performance:** cost per preset from `profiler.cpp` / the fps bar, measured against the game-AO baseline.

## 7. Phase 0 findings (2026-10-31)
Setup:
- Build: the COGBox 0.3.0 release, Title Update 6 (`port-tu6\gow2.exe`).
- Run: chapter 2 started straight in with `--gow2_launch="GearGame_P?loadcheckpoint=0?chapter=1"`, a scripted pad pressing A, windowed, 1x scale, default 30 fps.
- Captured frame 1500 with:
  - `--gow2_resolve_log_frames`;
  - `--gow2_draw_log_frames` + `--gow2_draw_log_textures`;
  - `--gow2_buffer_dump_frame` with `--gow2_renderer_resolve_readback=full` (works with our renderer).
- A second run used `--ue3_ini_edits="Xe-GearEngine.ini|SystemSettings|AmbientOcclusion=False"`.
- Draw numbers move by about 10 between runs; the structure does not.

### Frame 1500 under Title Update 6: 1,115 draws, 17 copies (notes/35 was the disc build: 20 copies)

| Copy | After draw | Source → destination | What it is |
|---|---|---|---|
| 0 | 513 | depth D24FS8, base 0, 1280 pitch → 0FA58000 k_8_8_8_8 | Scene depth, sampled later as fmt 23 [V] |
| 1-4 | 788-843 | 800x800 D24S8 → 0EF90000 | Shadow maps |
| 5 | 884 | rt1 (k_8_8_8_8, base 1808, 640 pitch) of an MRT quad → 0EDB0000 | AO setup, all white this frame [V]. Role [I] |
| 6 | 889 | rt0 k_16_16_FLOAT, base 1624, 640 pitch → 0EF90000 | **x = dynamic-object mask** (the player character, from a 56,490-index skinned mesh) [V]; **y = half-res linear view depth** (134-1022 world units, median 446) [V] |
| 7 | 890 | same target → 0EF90000 | **x = raw AO**, noisy (0.10-1.0, mean 0.86) [V]; y = depth |
| 8, 9 | 891, 892 | same → 0EF90000 | AO blur passes (ping-pong; one quad each) [V] |
| 10 | ~946 | same target → **0EEA0000** | **Final AO.** x = AO after the temporal blend and blur (mean 0.85) [V]. y is almost all 0 [V]; role [I] |
| 11 | 1035 | k_16_16, 640 pitch → 0EDB0000 | Motion-blur velocity |
| 12 | 1036 | rt0 7e3, base 720 → 0F328000 k_16_16_16_16_FLOAT, exp -3 | HDR scene (the upscaler's hook) |
| 13-15 | 1063-1065 | 400 pitch → 0EF90000 352x182 | Bloom |
| 16 | 1115 | → 1F5F8000 | Front buffer |

### How the AO chain works [V], from the draw log's texture bindings
1. **Half-res depth.** Draws 856-858 build a half-res depth (640 pitch, base 1440).
2. **Setup.** Quad 883 (MRT: AO target + rt1) sets up the AO target.
3. **Moving-object mask.** Skinned meshes 886-888 mark moving objects in x.
4. **AO.** Then the AO quad, then 3 blur quads. Each resolves to 0EF90000 and reads it back as T0.
5. **Temporal blend.** One quad (run-1 draw 905) reads **T0 = this frame's 0EF90000 and T1 = 0EEA0000 (fmt 28)**. T1 is **last frame's final AO**, because the copy into 0EEA0000 comes later in the frame. So 0EEA0000 is both this frame's output and next frame's history.
6. **Re-stamp.** Moving meshes are drawn again into the AO target (run-1 draws 930-932: 19,098 / 108 / 5,244 indices), then copy 10.
7. **Apply.** One **full-screen quad into the HDR scene** (run-1 draw 958; rt0 base 720, 1280 pitch) reads T1 = 0EEA0000. It comes **after all the lights and before translucency** (54-57 draws). So Gears 2 applies AO to the whole lit opaque scene, not only to ambient light.

### Other findings
- **AO off works through the ini [V].**
  - With `AmbientOcclusion=False`, copies 5-10 disappear and nothing else changes (11 copies instead of 17).
  - The meshes then go only into the velocity buffer.
  - The scene still gets a full-screen quad after the lights (draw 863), so that quad is not the AO apply by itself.
  - The launcher's "Off" can therefore be the ini edit, as planned.
- **Destination addresses move between settings.** The velocity buffer is at 0EDB0000 with AO on and 0EEA0000 with AO off. **Detection must not use addresses.**
- **No shader hashes in this build's logs.** The release resolve log prints `vs/ps 0000000000000000`, and the draw log has state/ALU/fetch hashes only.
- **Detection by structure.** The final-AO resolve is the **first resolve of the frame's half-scene k_16_16_FLOAT target to a different address than its ping-pong**, after ≥3 resolves of that target to one address and before the scene-HDR resolve. Equivalently: the copy whose destination the temporal quad read as history.
  - A developer build should add ps hashes as a second check (`vs_hash`/`ps_hash` exist in `Draw()`).
- **No normal buffer [V].**
  - The base pass (draws 156-512) writes rt0 only.
  - The "MRT at draw 164" from notes/35 is a one-draw clear/setup with rt1 (draw 155 here).
  - XeGTAO rebuilds normals from depth.

### What this changes in the plan
- **Hook target.** The Phase 3 hook targets **copy 10** (final AO, also next frame's history), not "copy 13".
  - Replacing x there gives the apply quad our AO.
  - It also feeds our AO into the game's temporal history. That is harmless, because XeGTAO output is already stable, but the game blends it into the next frame. Measure whether to also replace copy 7, so the game's temporal filter sees XeGTAO input instead of its own raw AO.
- **y channel.** At copy 10, y is ~0. Copy the source's y through unchanged; do not assume it is depth.
- **Moving-object mask.** XeGTAO has no moving-object mask. Moving objects get history rejection from the game only if we keep its chain.
- **Not done yet:**
  - chapter 23;
  - 2x/3x scale (whether the chain scales to half of the scaled target);
  - the apply quad's blend state (multiply vs. modulate in the shader);
  - ps hashes (needs a developer build).

Files from this run, kept outside the repo (local test output): `ao0-base*` / `ao0-dump` / `ao0-off`. Previews were made with a small EXR→PNG script.

## 8. Phase 1 groundwork: camera and depth maths, checked against the game (2026-10-31)
Setup:
- Build: the release exe (Title Update 6).
- Run: chapter 2 with `--gow2_fg_inputs=true --gow2_upscale_camera_matrix=-1 --gow2_upscale_probe=1500`, a buffer dump of frame 1500, and a scratch user folder.

**Camera matrix [V]**
- It is at **c0-c3**, read as columns (`FromConstantsTransposed`).
- At frame 1500 (frame 661 is noted for comparison):

  | Constant | Value |
  |---|---|
  | c0 | 0.067059 0 −0.996321 −0.997319 |
  | c1 | −0.913874 0 −0.0731089 −0.0731821 |
  | c2 | 0 1.62903 0 0 |
  | c3 | 0 −2.49e−05 −9.99195 −0.00195312 |
  | Vertical FOV | 63.1° (frame 661: 50.5°; the FOV changes per frame, e.g. when aiming) |
  | Near plane | 10.0 |
  | Far plane | infinite |

- The matrix is camera-relative, Z-up, with no translation.
- clip.z = k·clip.w + b, with **k = 0.998999** and **b = −9.99000**.

**Depth is reversed-Z [V].**
- The resolved scene depth (copy 0, decoded from 20e4) holds **guest depth = 1 − clip.z/clip.w = (1 − k) − b/w**. That means 1 at the near plane and about 0.001 at infinity. On the host the same value is stored ×0.5 (`float24_remap`).
- So **linear view depth w = −b / (depth − (1 − k))**.
- Checked against the game's own half-res linear depth (copy 6, y channel): over 5,992 flat pixels the ratio ours/game has **median 0.9994**, 5th percentile 0.9972, 95th percentile 1.0006 [V]. The rest is half-float rounding in the game's buffer.
- The flip comes from the viewport Z transform, not the matrix. Phase 1 code should read the viewport's Z scale and offset (PA_CL_VPORT_ZSCALE / ZOFFSET) rather than hard-code "reversed", so that Gears 3 and Judgment get the right sign too.

**For XeGTAO:**
- `DepthUnpackConsts` = (mul, add) with viewZ = mul / (depth − add): mul = −b ≈ 9.99 and add = 1 − k ≈ 0.001. These come from the raw guest depth; undo the ×0.5 host remap first.
- `CameraTanHalfFOV`:
  - y = 1 / (length of the matrix's clip-y row) = 1 / 1.62903;
  - x = 1 / (length of the clip-x row, taken over its xyz part) = 1 / 0.91633. The ratio 1.62903 / 0.91633 = 1.7778 is 16:9 [V].

  The aspect ratio comes from these two values, so ultrawide needs no special case.
- **Translation is not needed.** View-space positions only use the projection.

Tools for repeating the check: local script `tools/depthcheck.py`, not in the repo (pure Python; it reads buffer_dump's EXRs).

## 9. Phase 1 done (2026-10-31)

**Code**
- `include/xenos/renderer/ao.h` + `renderer/ao.cpp` (layer 0, game-free):
  - `ParseMode` for `--gow2_ao`;
  - `ViewportDepthMap`: guest depth = scale·z/w + offset, taken from the draw's translated viewport, so reversed-Z is read from the game and not assumed;
  - `MakeGtaoConstants`: XeGTAO's depth unpack, tan(fov/2), NDC→view and pixel sizes from the camera matrix. It refuses matrices with no perspective divide or no depth range;
  - `ViewDepth` / `ViewPosition`, mirroring the shader for tests.
- `shadow_renderer.cpp`:
  - **New settings:** `--gow2_ao=game|gtao|off` (default `game`: nothing changes) and `--gow2_ao_log=N`.
  - **Camera-only mode:** `gtao` turns on the existing camera tracking (`up_active_`) with no upscaler and without the depth/motion helper passes; jitter stays 0 because `up_inputs_only_` is set. The scene resolve skips `RunUpscaler` in this mode.
  - **What is captured:** the camera capture also records the scene viewport's depth map and size.
  - **Constants:** `AoConstants()` makes the constants once per frame on first use. Phase 3 will call it at the AO resolve.
  - **Upscaler fallback:** if the upscaler fails at run time, gtao keeps camera-only tracking.
- `tests/layer0_ao_test.cpp` (5 cases):
  - the depth map, normal and reversed;
  - view positions round-trip at 16:9 and 21:9, finite and infinite far;
  - Gears 2's measured frame-1500 matrix against the game's own linear depth, within 0.5%;
  - refusals;
  - mode names.

**In game [V]** (dev build, ch2, `--gow2_ao=gtao --gow2_ao_log=300`):
- The log reports "camera tracking every frame".
- Once play starts, constants are made on 300 of 300 frames. The first ~450 frames are loading and menus, with no camera.
- Values: near 10.000, `view z = -9.99 / (0.000999987 - depth)`, depth = −1·z/w + 1.
- tan(fov/2) at frame 1500 is 1.0913 × 0.6139 (16:9). Earlier frames are 0.8391 × 0.4720 while zoomed or aiming.

## 10. Phase 2 done (2026-10-31): XeGTAO runs in game

**Code**
- `renderer/xegtao_shaders.h` holds four built-in compute shaders ported from Intel's XeGTAO (MIT; notice in `THIRD_PARTY_NOTICES.md`):
  - **depth pyramid:** 5 levels of view depth, R32F. One dispatch per level; each dispatch rebuilds its 16x16 tile from the scene depth, because the backend transitions whole textures;
  - **main pass:** normals rebuilt from depth in place; slices × steps per quality preset;
  - **denoise:** 1-3 passes, ping-ponging between two working textures;
  - **debug overlay** on the presented image.
- The port works within the shared compute layout (16 root constants, t0-t3, u0-u1, no samplers): Gather and SampleLevel became `Load`s, and depth is 32-bit.
- Sky handling for reversed-Z with an infinite far plane: anything that isn't a positive, finite distance counts as "far" and gets no occlusion.
- `shadow_renderer.cpp`:
  - `InitAo` builds the pipelines;
  - `RunGtao` creates the scene-size textures and runs the 5 + 1 + N dispatches into `ao_final_` (R16F, visibility 0..1);
  - `AoDebugView` runs in `PresentOurs`, after SMAA.
- New settings:

  | Setting | Values | Default |
  |---|---|---|
  | `--gow2_ao_quality` | low / medium / high / ultra (1×2, 2×2, 3×3, 9×3) | high |
  | `--gow2_ao_radius` | world units | 30 |
  | `--gow2_ao_power` | final value power | 2.2 |
  | `--gow2_ao_denoise` | 1 sharp / 2 medium / 3 soft | 2 |
  | `--gow2_ao_debug` | 1 AO only, 2 split (game left, AO right), 3 view depth / 2000, 4 raw term, 5 edges | 0 |

**Where it runs [V]**
- XeGTAO runs at the **scene-depth resolve** (copy 0, straight after the base pass).
- The first try ran it at the scene colour resolve (copy 12), and everything came out as "sky". Just before copy 12, the game writes EDRAM depth base 0 through a 640-pitch, 2xMSAA alias: draw ~1035 with AO on, ~925 with AO off. By then the scene depth is gone.
- Phase 3's copy-10 hook comes after copy 0, so the AO is ready when the game's AO is resolved.

**In game [V]** (ch2, frame 1500, 1x, captured with `--gow2_frame_shots`):
- The depth view is a correct linear-depth image.
- The AO shows contact occlusion under the bed and counter, around Marcus, and in the shelves and pipe joints.
- Radius 60 gives broader occlusion, closer to the size of the game's own AO.
- One denoise pass is grainy, as XeGTAO expects TAA. Three passes are clean.
- The window-screenshot route was dropped: it captured whatever window was in front. Use `--gow2_frame_shots` instead.

**Open items**
- **Tuning:** pick the radius, power and denoise defaults against the game's own AO in more scenes, in Phase 3, where the game's composite applies them.
- **GPU cost:** not measured yet.
- **Upscalers:** with DLSS/FSR/XeSS on, the depth is jittered. Use the de-jittered depth resolve, or accept it.
- **Vulkan:** not tried yet.

## 11. Phase 3 done (2026-10-31): XeGTAO in the game's own composite, at full resolution

**Why full resolution needs a stand-in [V]**
- The game's AO chain targets have a 640 pitch. That is under `--gow2_renderer_resolution_scale_threshold` (1120), so they stay at **1x at every resolution scale**.
- Packing XeGTAO into the game's 640x360 resolve would throw away most of its detail.

**How it works now (`shadow_renderer.cpp`)**
- `AoNoteResolve`, called from `ResolveColour` after `PrepareColourTarget`, finds the final AO by shape, not by address:
  - only R16G16F targets (k_16_16_FLOAT) whose resolve is half the scene's guest size (±8) count;
  - the first address that receives ≥3 resolves is the ping-pong;
  - the next resolve of such a target to a different address is the final AO.
- When it finds the final AO:
  - **The game's own resolve is kept.** Its temporal history stays the game's own, so there is no feedback loop.
  - `kXeGtaoPackCs` writes a **scene-size R16G16F stand-in**: x = XeGTAO's visibility, y = the game's y at that spot.
- For the rest of the frame, `AoOverrideViews` (in `Draw`, after `textures_->Bind`) changes the binding of any 2D fetch of that address and size to the stand-in:
  - the slot is marked as scaled, so the shader's texel maths stay in guest texels;
  - the binding ends if that address is resolved again, and at frame end.
- In ch2 the stand-in is read **once per frame**, by the apply quad. That quad runs after the lights and before translucency, so the game's own formula and placement apply XeGTAO's AO.

**Results [V]** (ch2, frame 1500, `--gow2_ao=gtao`):
- **Detection:** 300 of 300 frames: chain found, replaced, and read through the stand-in once. The same holds at 2x (2560x1440), on 1,500 of 1,500 frames.
- **Picture:** compared with the game's AO, XeGTAO is darker in fine creases (seat edges, bed frame, trims, pipe joints, shelf undersides) and lighter on broad floor areas. There the game's 640x360 AO laid a large soft blob.
- **Comparison tools:** local script `tools/pngdiff.py`, not in the repo, makes an amplified difference image and crops; the result files are `p3-cmp*`.
- **GPU cost** (high-end NVIDIA GPU, renderer GPU totals, high preset, 3 denoise passes; the game's own AO still runs):

  | Scale | Game AO | XeGTAO | Added |
  |---|---|---|---|
  | 1x | 0.96 ms | 1.05 ms | +0.09 ms |
  | 2x | 2.24 ms | 2.56 ms | +0.33 ms |

- **Defaults now:** radius 60, power 2.2, 3 denoise passes, high quality.

**Still open**
- Phase 3b: live toggles (game / XeGTAO / none, and the debug views) on F-keys, behind a developer switch.
- Skipping the game's own AO draws to recover their cost.
- DLSS/FSR (jittered depth); Vulkan; split-screen (one replacement per frame today); ultrawide; more scenes and chapters.

## 12. Phase 3b done (2026-10-31): live switches, and proof the game's AO doesn't reach the picture

**Code**
- `ao_debug.cpp` (layer 0), with declarations in `dev_tools.h`:
  - `--gow2_ao_debug_keys=true` (off by default, developer only) registers three keys with the SDK's keybinds:
    - **F9** cycles the AO the game's composite gets: XeGTAO / none / the game's own;
    - **F10** cycles the debug view: normal / XeGTAO only / split / depth / raw term / edges;
    - **F11** cycles the quality preset: low / medium / high / ultra.
  - A label at top left (an ImGui dialog, like the FPS bar) shows the state.
  - The keys only set atomics; the renderer reads them once per frame, in `AoEndFrame`.
  - It is installed and removed from `gow2_app.h` (`OnCreateDialogs` / `OnShutdown`).
- `shadow_renderer.cpp`:
  - new `--gow2_ao_show=gtao|none|game`, the starting point for F9 and a way to script tests;
  - every quality preset's main pass is built at start (`ao_main_pipes_`), so F11 doesn't stall on a shader compile;
  - with "game's own", no stand-in is made, and XeGTAO still runs for the debug views.
- A local launcher script (not in the repo). It runs the dev build on the installed COGBox game files, with its own save folder, `--gow2_ao=gtao --gow2_ao_debug_keys=true`, the FPS bar, and an optional chapter number.

**Proof that the game's AO is replaced, not added to [V]**
- Compared: `--gow2_ao=gtao --gow2_ao_show=none` (white stand-in) against the game started with `AmbientOcclusion=False`, ch2 frame 1500.
- The static scene is **pixel-identical**. The only differences (3.5% of pixels each way) are things that animate differently between two runs: Marcus, the hanging body, a flickering monitor.
- So the game's own AO term doesn't reach the picture when the stand-in is on.

**Not checked unattended**
- The keys and the label need a person at the keyboard: sending keystrokes could land in whatever window is in front.
- `--gow2_frame_shots` saves the image before overlays, so it can't show the label.

## 13. First hands-on test by the tester (2026-10-31)

**Setup:** ch2 hospital room, native 4K (the log shows the scene at 3840x2160), HDR, keyboard and mouse, run through a local launcher script.

**Verdict:**
- XeGTAO looks considerably better than the game's own AO.
- The occlusion is exact around object silhouettes, instead of blobs around them.
- It also shades characters, which the game's AO leaves out: the game masks moving objects, copy 6 x.
- Ultra adds occlusion in some rare small places that high misses. In the NVIDIA overlay it showed no visible GPU increase over high (not an exact measurement).

**Decision (user):**
- The launcher option offers XeGTAO with **high as the default and ultra as a choice**.
- The 4K log shows the final AO found on 600 of 600 frames. "Replaced" was lower (294-465 per 600) only because F9 was being cycled to "none" and "the game's own" during the test.

## 14. Skipping the game's own AO chain while XeGTAO is shown (2026-10-31)

**What it costs [V]:** with the game's AO on, a ch2 frame takes 0.96 ms of GPU time at 1x; with `AmbientOcclusion=False` it takes 0.72 ms. The game's chain therefore costs about 0.24 ms, and since its targets always stay at 1x, that cost is the same at any resolution.

**What is skipped** (`shadow_renderer.cpp`, `--gow2_ao_skip_game`, default 3; 1 = draws, 2 = copies, 0 = off). The target is the one the chain was found in (`ao_chain_key_`, learned in `AoNoteResolve`). Skipping only happens when:
- XeGTAO or "none" is shown;
- XeGTAO has already run this frame;
- this frame's final AO resolve hasn't happened yet, and neither has the scene colour resolve.

Within that, two kinds of work are skipped:
- **Draws:** only draws whose **only** colour target is the AO target, and which write neither depth nor stencil. They are dropped before any ownership or transfer work.
- **Copies:** the chain's ping-pong copies of that target. The final copy still runs, so the chain is found every frame.
- When the chain was skipped, the stand-in's y channel is set to 0 (the game's usual value), because the target holds stale data.

**The trap [V], found by bisecting with a temporary skip mask:**
- The chain's first draw is an MRT quad. Besides the AO target it writes rt1 (8888, EDRAM base 1808 in ch2), and the game uses that output outside the chain.
- Skipping that one draw washes out the whole picture: a bright haze over most of the screen.
- Skipping every other chain draw is harmless.
- Skipping only the copies was harmless from the start.

**Results** (ch2, frame 1500):
- **Picture:** identical to XeGTAO without the skip, apart from animation differences between runs (~4% of pixels each way, at both 1x and 2x).
- **Skipped per frame:** about 8 draws and 4 copies.
- **GPU time** (high-end NVIDIA GPU):

  | Scale | Game AO | XeGTAO | XeGTAO + skip |
  |---|---|---|---|
  | 1x | 0.96 ms | 1.05 ms | **0.93 ms** |
  | 2x | 2.24 ms | 2.56 ms | **2.33 ms** |

## 15. Step 2: the launcher option (2026-10-31)

**The option** (`cogbox/src/gears_settings.cpp`): "Ambient occlusion" on the Graphics tab. It is also in the in-game menu, which lists every launcher option.
- **Choices:** Off / Game's own / XeGTAO (high) / XeGTAO (ultra). The default is **XeGTAO (high)**, the tester's decision in section 13.
- **Availability:** feature `ao`. Gears 2 only, and only when the exe contains `gow2_ao_quality`. The release exe doesn't, so the option shows as "not yet" there and nothing is sent. Gears 3 and Judgment show "not yet (Gears 2 first)". Gears 1 doesn't list the option, like the other `[SystemSettings]` options.
- **What each choice sends:**

  | Choice | Arguments |
  |---|---|
  | Off | `|SystemSettings|AmbientOcclusion=False`, joined into the existing `--ue3_ini_edits` |
  | Game's own | nothing |
  | XeGTAO (high) | `--gow2_ao=gtao --gow2_ao_quality=high` |
  | XeGTAO (ultra) | `--gow2_ao=gtao --gow2_ao_quality=ultra` |

- **Not in the presets.**
- **Translations:** the label, the two XeGTAO choice names, the tooltip and the "Gears 2 first" reason, added to all 11 `cogbox/lang/*.txt` files. Each reuses that file's existing words for "Off" and "Game's own".

**Checked [V]**
- `cogbox/test/settings_test.cpp`, built with the pinned clang into `out/cogbox-test/`:
  - each choice gives exactly the arguments above;
  - a Gears 2 exe without the feature gets none;
  - Gears 3 gets none.
- `--summary` reports `FAIL ini values`. **That failure was already there**: the same build of the unchanged `gears_settings.cpp` reports it too, probably a compiler difference from the MinGW build the project uses.

**Building the launcher on Windows (developer note)**
- `build.sh` cross-compiles with MinGW on Linux.
- On Windows: `third_party/_dl/build_cogbox.ps1` (pinned clang, `llvm-rc`, the SDL3 3.4.18 VC package in `third_party/sdl3-vc`) writes `out/cogbox-build/CogBox.exe` and `SDL3.dll`.
- `cogbox/res/version_rc.h` is generated and gitignored.

## 16. Future work (not planned in detail; ideas raised during the AO work, 2026-10-31)

1. **Merge the game's own character velocity into the motion vectors.**
   - Today the motion vectors carry camera motion only (`notes/64` item 3), which is why DLSS/FSR/XeSS smear moving characters and are marked experimental.
   - Gears 2 draws a velocity buffer of its own for motion blur: half-res k_16_16, copy 11 in TU6 ch2 (section 7).
   - This should come first, because it helps every temporal technique.
2. **SMAA T2x** as an AA choice. It needs a 2-sample jitter, motion vectors, and SMAA's temporal resolve.
   - It should run at the scene colour resolve (before post-processing and the HUD, like the upscalers), not at present time, so the HUD doesn't ghost.
   - It depends on item 1 for moving characters.
   - Estimated at a few days.
3. **AO denoise tied to the AA choice:** fewer XeGTAO denoise passes (1-2) when DLSS/FSR/XeSS are on, since their temporal accumulation also smooths the AO. Keep 3 with SMAA or no AA.
4. **AO for Gears 3 and Judgment:** repeat the Phase 0 investigation (find each game's AO chain, its apply pass, and the MSAA details in Gears 3); the layer-0 code is shared.
   - *Done for Gears 3 (s.25-28); Judgment remains.*
5. **PCSS (percentage-closer soft shadows) for the dynamic shadows** (the tester's idea, 2026-11-01; not investigated yet).
   - **Goal:** contact-hardening shadows (sharp where the caster touches the receiver, softer further away) instead of UE3's fixed-width filtering, for the dynamic shadows of characters and moving objects.
   - **What is known:**
     - UE3 renders per-object / per-light shadow depth maps and projects them in screen-space passes. Modulated shadows go through an "attenuation buffer" that then multiplies the scene (Gears 3 notes/27; a red-only draw into 8888_GAMMA with blend src x dest colour, seen on Vulkan).
     - The shadow depth targets (pitch 1120 / 560) stay at 1x under the 1120 scale threshold even at 4K, so dynamic shadows keep console resolution.
     - Shadows are the GPU hot spot of these games (Gears 3 notes/27).
   - **Likely approach** (as XeGTAO's add pass is anchored on the fog shader):
     - Find each game's shadow projection pixel shader(s) and their constant layout (shadow matrix, depth bias, filter radius).
     - Substitute an HLSL PCSS variant for them, by shader hash: a blocker search in the shadow depth map, a penumbra estimate from the blocker distance and a light size, then PCF with that radius. The depth map has to be readable at that point (resolved copy or EDRAM source).
   - **Risks / open questions:**
     - The projection shaders may differ per light type, per game and per material permutation (the fog shader was stable; shadow shaders may not be).
     - Light size is not in the data and needs a chosen value per light type; spot-light and directional shadow frusta need different penumbra maths.
     - The 1x shadow maps limit how fine the contact edge can get.
     - Cost: the blocker search adds texture reads to an already heavy pass.
   - **Cheaper steps to weigh first:** higher shadow-map resolution (UE3 `MaxShadowResolution` / `ShadowTexelsPerPixel` through `--ue3_ini_edits`, or exempting shadow targets from the 1x threshold), then a better fixed filter (Poisson PCF) before full PCSS.
   - **Start with:** a Phase 0-style trace of one Gears 2 frame with a character shadow: which draws write the shadow depth, which pass projects it, and its pixel shader and constants.

Considered and **not** worth doing (reasons in the chat of 2026-10-31):
- An HBAO-style pass: GTAO is its more accurate successor.
- Multi-bounce AO: there is no albedo buffer in a forward renderer, so with a constant albedo it is just a brightness curve, which `--gow2_ao_power` already gives.
- Our own AO temporal accumulation: upscalers already accumulate over time, and 3 denoise passes look clean without one.
- Disabling Gears 3's 4x MSAA half-res translucency target: it is game logic, not removable AA, and saves little.

## 17. Chapter sweep (2026-10-31)

**Setup:**
- Dev build, `--gow2_ao=gtao` (high), skipping the game's chain.
- Each chapter booted with `GearGame_P?loadcheckpoint=0?chapter=N` for 2 minutes, with a fresh scratch save folder, the scripted pad pressing A, and one frame saved at frame 2400.
- Local scripts (not in the repo): `tools/aosweep.ps1` and `tools/chrun.ps1`.
- Chapters covered: 0-10 in full, plus 12, 15, 18, 21, 24 and 27 (chapter index, 0-based).

**Results [V]:**
- **No crashes.** Every chapter ran its full time.
- **Full replacement:** on chapters 1, 4-10, 12, 18, 21, 24 and 27, the AO chain was found and replaced on **100%** of the frames where XeGTAO ran (2,600-2,950 frames each).
- **Chapters 0 and 2:** no XeGTAO frames. They are cutscene or intro for the whole 2 minutes (frame captures confirm it for chapter 2), so there is no scene camera.
- **Chapters 3 and 15:** the chain is found for the first ~300 frames, then never again (308 of 2,940 and 307 of 2,935). An A/B on chapter 3 (the game's own AO against XeGTAO, with a resolve log at frames 600 and 2400) shows **the game itself stops drawing its AO there**:
  - frame 600 has the usual 5 half-res k_16_16_FLOAT copies;
  - frame 2400 (outdoor, "Defend your Assault Derrick") has none in **either** run: just scene depth, 18 shadow-map copies, velocity, the HDR scene, bloom and the front buffer, which is the AO-off layout.
  - Why it does this is unknown. A performance budget is likely: there are many shadowed lights outdoors. A per-area art choice is also possible.
- **Consequence:** XeGTAO needs the game's own apply pass, so where the game turns its AO off there is no AO at all, the same as the original.

**Possible extension, not planned:** an "add AO" path. In frames where the game draws no AO chain, composite XeGTAO onto the scene colour ourselves, before translucency. This is the same design Gears 1 needs, since it has no SSAO, but it changes the game's look in places its developers chose to leave without AO.

## 18. Why the game's AO vanishes in places, and what adding it there would take (2026-10-31)

**New tools** (developer only, off by default):
- `--gow2_ao_trace_frames=<frames>`: logs every full-screen pass in those frames (target, vs/ps hash, blend, 2D texture addresses), then the pixel shader's c0-c8.
- `--gow2_renderer_hlsl_dump_hashes=<hashes>` with `--gow2_renderer_hlsl_dump_dir`: writes the translated HLSL of those shaders. It needs an empty shader cache so they are translated in that run.

**The apply pass is the game's fog pass with the AO folded in [V]** (ch3 frame 600, ch1 frames 600-2400):
- It is `ps AC8AE1A05DCEEACE`: a full-screen quad into the scene, blend src One / dst SrcAlpha, RGB only.
- It reads t0 = the 8888 half-res image written as rt1 of the chain's first MRT draw (`ps C84DBBA59A174733`). Fog is packed into it; it is decoded with log/exp. t1 = the final AO.
- **Formula:** `colour = c0.x * T * c5.rgb + T * c3.rgb * (c7.w - AO)`, `alpha = AO * T`.
  - With `c3 = (0,0,0)` (UE3's OcclusionColor, black in both chapters) and `c7.w = 0`, this is `scene' = scene * AO * T + fog`.
  - So **the game's AO is a plain multiply of the lit opaque scene, applied before the fog.**
- This is also why the chain's first draw must never be skipped (section 14): its rt1 is the half-res fog.

**AO-off frames [V]** (ch3 frames 900, 1500, 2400):
- No chain. One fog-only pass, `ps D58B286B84EE971D` (vs `2E6EDA55613EAE88`), at full resolution, with the same blend, at the same point (after the lights, before translucency).
- Its constants are the same in all three frames.

**Adding AO there ("XeGTAO everywhere"):**
- In frames with no chain, insert one full-screen pass, `scene *= AO` (multiply blend, XeGTAO's full-res result), right before that fog draw.
- Since the game's own apply is `scene * AO` before fog, the look stays continuous where the game switches its AO off.
- **Cost:** XeGTAO already runs in those frames, so only the multiply pass is added.
- **Effort:** about half a day to a day. The work is the mid-frame graphics pass (SMAA-style), detection by fog hash plus structure, and checking transitions in ch3 and ch15.
- **Taste question:** the developers may have wanted no AO there. Offer it as a setting, not a default.
- **Limit:** a level with neither AO nor fog has no anchor draw.

## 19. "XeGTAO everywhere": AO added where the game turns its own off (2026-10-31)

**Code** (`shadow_renderer.cpp`, `xegtao_shaders.h`):
- **Settings:** `--gow2_ao_everywhere` (default on with `--gow2_ao=gtao`) and `--gow2_ao_fog_ps` (default `D58B286B84EE971D`, the fog-only pixel shader from section 18).
- **When it fires (`AoAddWanted`).** Inside `Draw`, after the target preparation, if all of these hold:
  - XeGTAO is shown and ran this frame;
  - the frame has had no AO chain so far, and nothing has been added yet;
  - the scene colour resolve hasn't happened;
  - the draw is a full-screen pass into one HDR scene target (R16G16B16A16F at the scene scale), with blend One/SrcAlpha and a fog pixel shader from the list.
- **What it does (`AoAddPass`):** one full-screen triangle (`kTransferVs` + `kXeGtaoMultiplyPs`) into that target, with the multiply blend (src Zero, dst SrcColor, RGB only). That gives `scene *= XeGTAO AO`, which is the game's own apply formula with its black occlusion colour, just before its fog.
  - The pass then clears `tex_memo_valid_` and marks the target written, the same way the EDRAM transfers do. The game draw then sets its own state again.
- The log line `added where the game had none: N` counts it.

**Results [V]** (ch3 = chapter 4, 1x):
- **Coverage:** every frame is either replaced (the game's AO area: 78/78, 100/100, ...) or added (the AO-off area: 100/100 from about frame 730 on). At the switch, 29 + 71 = 100. No frame gets both.
- **Picture:** at frame 2400 (outdoors, the derrick), contact occlusion under the railings and the turret mount, where the walkway meets the derrick, and in the rock crevices. The sky, trees and HUD are untouched, with no halos.
- **Comparison:** a pixel diff against `--gow2_ao_everywhere=false` isn't meaningful there, because the derrick moves, so the camera differs between runs.
- **ch15 (chapter 16) [V]**, with the scripted pad pressing A every 10 s through to 160 s so it gets past the intro:
  - replaced 101/101;
  - then 207 replaced + 76 added at the switch-over (5 frames got neither);
  - then added on 300/300 in every window to the end.
  - The same fog shader covers it.
  - Frames 1800-4800 are the snowy forest drive in the Centaur: snow, fog and the vehicle look right, with no halos.
- **Hands-on [V] (user):** AO appears in ch3's outdoor area and looks fine; in natural areas the effect is subtle, with trees picking up some shading.
- **Test-script pitfalls**, all in local test scripts (not in the repo), not in the game:
  - a switch given twice makes the SDK's CLI11 parse fail, after which **no** switch applies (main menu, fullscreen, defaults);
  - a PowerShell parameter named `$Input` is reserved and never receives its value.
  - `tools/chrun.ps1` now refuses duplicate switches, and `-Expect` closes the game within 25 s if a log line proving the switches applied doesn't appear.

## 20. XeGTAO with DLSS / FSR / XeSS (2026-10-31)

**Build:** vendor SDKs matching COGBox 0.3.0's DLLs, in gitignored `third_party`:
- DLSS SDK v310.9.16 (`nvngx_dlss.dll` 310.9.16);
- FidelityFX SDK **v2.4.0** (loader 2.3.0.4555, upscaler 4.1.2.4555, frame generation 4.0.2.4555). v2.3.0 ships older 2740 builds;
- XeSS SDK 3.0.2 (`libxess.dll` 2.0.2.68).

Configured with `tools/build.ps1 -Tu6 -CMakeDefs 'GN_WITH_DLSS=ON GN_WITH_FSR=ON GN_WITH_XESS=ON'`; the CMake cache had kept them OFF from the first configure.

**DLSS model** (`dlss_upscaler.cpp`, `--gow2_dlss_preset=recommended`):
- K (transformer) for DLAA, Quality and Balanced; M for Performance; L for Ultra Performance. Those are the SDK 310.5+ presets.
- The CNN presets (A-F) are deprecated and never selected.
- COGBox's default "native" DLSS mode is DLAA, so it uses K.

**Results [V]** (ch1 = chapter 2, `--gow2_ao=gtao`):

| Upscaler | Started as | Upscaled frames | XeGTAO |
|---|---|---|---|
| DLSS | DLAA 1280x720 HDR, preset recommended | every frame from ~300, 0 failures | replaced 600/600 per window |
| FSR | 3.1.6 native AA | 1151 of frames 1-1500, 0 failures | replaced 600/600 |
| XeSS | 2.0.2 native AA | 1149, 0 failures | replaced 600/600 |
| DLSS performance mode, 2x output | SR 1280x720 -> 2560x1440 | 1148, 0 failures | replaced 600/600; XeGTAO runs at the **render** size, 1280x720 |

- In every run the frame looks right, with no smearing or doubled AO around the bed, the counter or Marcus.
- **Jitter:** XeGTAO reads the jittered scene depth, so its AO jitters together with the scene it darkens. It is applied before the upscaler, so the upscaler's temporal accumulation also smooths the AO.
- **Still open** (future work 3, section 16): fewer denoise passes with an upscaler on. Not yet compared by eye.

## 21. Motion vectors for DLSS / FSR / XeSS: what the velocity work found (2026-10-31; camera motion fixed)

**Test:** ch1 (chapter 2), DLSS. The scripted pad strafes left 6 s, right 6 s, then forward. The upscaler captures are motion vector images (`--gow2_upscale_capture_frames`, BMP: grey = still, saturates at ±10 px). Local scripts (not in the repo): `motionstats.py`, `flowcheck.py` (block matching between consecutive captured frames), `bmp2png.py`.

**1. A bug, fixed [V]: reversed depth in the reprojection.**
- `ReprojectionMatrix` mapped the motion pass's depth straight to clip z/w. Gears 2's depth is reversed (1 − z/w, section 8), so every pixel looked about 10 units from the camera.
- **Effect before the fix:** any camera *translation* gave enormous parallax. While strafing, the vectors saturated everywhere (the old centre statistic: 283 px/frame mean, 7,278 max). Rotations were unaffected, which is why turning looked fine.
- **Fix:** `ReprojectionMatrix(..., depth_map_scale, depth_map_offset)` now undoes the viewport's Z transform (`up_depth_map_`, from `ao::ViewportDepthMap`). It is the identity for games with normal depth.
- **Test:** `tests/layer0_upscaler_test.cpp` "camera motion with a reversed-Z viewport". The old mapping is off by more than 20 px.
- **After the fix:** strafing vectors are 0.5-3 px, with nothing saturated.
- **Note:** the "centre motion" log statistic reprojects the screen centre at a fixed depth 0.5. With reversed depth that is a point about 20 units away, so the number is meaningless for Gears 2. Use the captures instead.

**2. A gap, fixed: the upscalers were never told the depth is inverted.**
- `Upscaler::Configure(..., depth_inverted)` now sets DLSS `NVSDK_NGX_DLSS_Feature_Flags_DepthInverted`, FSR `FFX_UPSCALE_ENABLE_DEPTH_INVERTED` and XeSS `XESS_INIT_FLAG_INVERTED_DEPTH`.
- It is set from `up_depth_map_.scale < 0`, and each wrapper rebuilds its feature when the flag changes.
- These SDKs use depth for disocclusion and motion-vector dilation.

**3. The real cause, fixed [V]: the upscalers read a cleared depth buffer.**
- **What it looked like:** block matching frames 1953→1954 (strafe) showed the background moving about **+23 to +28 px**, but the vectors said **−1 to −2.6 px**. The object vote looked erratic (38 of ~300 objects agreeing), so the first suspect was the camera move, and a scan for a camera-position constant followed (`--gow2_camera_scan_frames`). No per-frame register held one: the shared constants are identity rows, `vs c237` (a unit vector) and lighting values.
- **The check that found it:** a log of the motion pass's own inputs (both matrices and the move) was replayed offline (local script `repcheck.py`, not in the repo). With the logged move, the formula gives +16 px for a point 300 units away, close to the measured motion. The captured vectors matched only the result **with no translation at all**. The captured depth was 0 everywhere, i.e. every pixel at infinity, where a camera translation moves nothing.
- **Why:** the same trap as XeGTAO in Phase 2 (section 10). The upscaler copies the scene depth at the scene *colour* resolve, but by then the game has cleared EDRAM base 0 through its 640-pitch alias for the half-res effects. DLSS / FSR / XeSS were also given that empty depth, which they use for disocclusion and vector dilation.
- **Fix:** `UpscaleSaveDepth` copies the scene depth into `up_depth_` at the scene **depth** resolve (`Resolve`, right after `RunGtao`'s hook), and `RunUpscaler` uses that copy. It falls back to the old read on the first frame and after a scale change (`up_scene_scale_`). The depth capture now writes 1 − depth for reversed-Z, so it is readable.
- **Result (frames 1951-1955, captured depths 196-318 units):**

| Patch | Image motion (block matching) | Motion vector |
|---|---|---|
| door | +23 / +24 / +43 / +39 / +38 px | +19.6 / +23.7 / +42.7 / +38.2 / +38.3 px |
| pillar | +23 / +25 / +42 / +38 / +37 px | +24.1 / +24.0 / +42.9 / +38.1 / +37.5 px |
| floor | +26 / +27 / +47 / +42 / +41 px | +26.6 / +25.9 / +46.1 / +41.4 / +41.0 px |
| left wall | +13 / +16 / +25 / +23 / +22 px | +14.0 / +14.7 / +25.4 / +22.3 / +22.4 px |

  - The vertical components match too, and the GPU vectors equal the offline formula wherever they fit the capture's ±10 px encoding.
  - The object vote was right all along. Its "jumps" (−8 → −15 units/frame) are Marcus speeding up, and the image moves by the same factor. **No camera-position constant is needed.**
  - **Marcus** gets +35 to +62 px against a real −3 to +7 px: he moves with the camera, so camera-only vectors are wrong on him. That is the character-velocity merge (item 4).

**4. Characters: their own motion, from the game's velocity pass [V].**
- **The velocity pass** (`vs C5B0195E728D405F` and a few others; `ps AC9A377DA32C4E18` and others) draws every skinned mesh into a half-res k_16_16 target (EDRAM 1624, pitch 640), copy 11 → `0EDB0000` in TU6, with blend MAX. It runs on most frames. (The earlier "only its clear" came from traced frames where no character draw was captured.)
- **What its values say:** the main character shader runs with velocity scale c3.xy = 0, so its pixels hold exactly 0.5 = "no velocity", not "not moving". Some other velocity shaders carry real values in a saturating encoding (`0.5 + 0.5·u / max(|u|, MaxVel)`, clamped to [1/255, 1]). Decoding them was too uncertain (per-pixel fits gave 300-1500 px per unit), so the values are not used. **Only the mask is:** written = a character pixel.
- **What the draws carry:** VS c5-c8 = this frame's object-to-world relative to the camera (Marcus about 194 units ahead, 91 below); **c9-c12 = last frame's in absolute world coordinates** (≈ 20250, −10690, 160). So last frame's camera position = this frame's c12 − last frame's c8 for the same mesh: the absolute camera position after all, without any constant search.
- **The merge (`--gow2_upscale_character_motion`, default on):**
  - `VelocityDraw` collects each velocity draw's two transforms. It requires c0-c3 = the scene camera, affine rows, and the target; parts that share a transform are deduplicated.
  - `VelocityResolve` copies the target at its resolve (`kCopyFloat2Cs`), because the game reuses that EDRAM.
  - After the camera motion pass, `CharacterMotion` builds a table (≤ 32 characters, RGBA32F texture). Each row holds a character's reprojection = prev_view_proj × translate(−last camera) × then × inverse(now) × inverse(view_proj) (`ObjectReprojectionMatrix`) and its origin.
  - `kMotionObjectsCs` gives each masked pixel the reprojection of the nearest character origin within 250 units; other pixels keep the camera motion.
  - Last frame's camera comes from `PreviousCameraFromObjects` (median over matched meshes).
- **Test:** `tests/layer0_upscaler_test.cpp` "characters move with their own transforms": a player who moves with a turning, strafing camera gets 0 px (the camera-only vectors were off by > 20 px), and an NPC standing in the world gets exactly the camera's motion.
- **Result (ch1, DLSS, scripted turn / strafe / look up; character points block-matched, local script `charcheck.py`, not in the repo):**

| Frames | Background (camera) motion | Characters' real motion | Our vectors on them | Median error |
|---|---|---|---|---|
| 1921-1925 (look up) | −12 to −37 px (y) | 0 to −12 px | 0 to −12 px | ~1 px |
| 1831-1833 (turn) | −13 to −22 px | −6 to +1 px | −6 to −0.5 px | 3-4 px |
| 1801-1805 (strafe starts) | −9 to −17 px | +1 to +9 px | +2 to +12 px | 4-8 px |

  - With camera-only vectors, the characters had the background's motion: errors of 10-35 px.
  - **What remains:** limb animation. Skinning moves vertices relative to the mesh transform, which this does not model; two frames show 11 px in y while Marcus's body bobs. The mask is half resolution, so edges are ±1 px.
  - **Fixed after the tester's Ultra performance test** ("Marcus's back plates look like mosaic / puzzle pieces" while standing still): a character is several meshes. The body has its origin at the feet and an upright, yaw-only rotation; the attached parts (gun, helmet) have origins at chest height, sway with the idle animation, and are not drawn into the velocity pass every frame. Nearest by origin, a part took over the upper body on the frames it was drawn, giving ~1 px (max 9) of flickering motion on a still player, i.e. history rejected in patches. Now upright bodies are preferred (table w = 1), and parts are used only when no body is in range. Standing-still captures: all vectors exactly 0 (they were 0 / 1.1 / 0 frame to frame). The turn and look-up errors are unchanged (0.5-12 px). Frames at a camera cut (meshes' camera estimates 230 units apart) are not meaningful for block matching.
  - The character count is 1.6-8.7 per frame in ch1. The cost is one small dispatch and a 5×33 texture upload.

## 22. SMAA T2x (2026-11-01)

**What it is:** SMAA 1x on the 3D scene, drawn alternately at two sub-pixel positions, plus a temporal resolve that averages this frame with the last one reprojected through the motion vectors. The result is roughly 2x supersampling on edges and much calmer shimmer, on any DirectX 12 GPU, at a fraction of an upscaler's cost.

**Where it runs:** as a fourth "upscaler" kind (`--gow2_upscaler=smaa_t2x`, `rr::upscale::Kind::kSmaaT2x`), so it reuses everything DLSS / FSR / XeSS get:
- the scene-colour resolve hook, before bloom, tone mapping and the HUD (today's SMAA runs on the finished picture, where a temporal blend would smear the HUD);
- the jitter path, with T2x's two positions: SMAA's (0.25, -0.25) / (-0.25, 0.25) in NDC (y up), i.e. viewport offsets (+0.25, +0.25) / (-0.25, -0.25) here;
- the depth copy and the motion vectors of section 21, characters included; camera cuts reset the history.

It is created in the renderer (`T2xUpscaler`, which needs the shader cache), not by `CreateUpscaler`. It anti-aliases at the render size; the scene-resolution modes are ignored (logged).

**Passes** (`smaa/smaa_passes.h`):
1. `kT2xPrepCs`: the HDR scene goes into a tone-mapped copy (Reinhard on the largest channel, which is invertible) and its square root (the edge-detection input, since SMAA wants non-linear colour).
2. SMAA edges, then blending weights with T2x's sub-sample indices ((1,1,1,0) / (2,2,2,0), chosen by the jitter's sign; `Source(body, true)` adds `xe_smaa_sub` to the constant buffer).
3. Neighbourhood blending of the tone-mapped colour into this frame's RGBA16F image (ping-pong history).
4. `kT2xResolveCs`: last frame's image at the reprojected position (manual bilinear), clamped to this frame's 3x3 neighbourhood (what moved or appeared falls back to this frame), averaged 50/50, then inverse tone-mapped into the upscaler output.

**Verified (ch1, 1x):**
- Standing still, the output changes 10x less from frame to frame than the jittered input (mean difference 0.73-0.88 vs about 7.5, 8-bit units).
- In zoomed crops the diagonal beams and railings are visibly smoother than the raw scene, without texture blur.
- Mid-turn there are no double edges or trails (chair legs, floor cable).

**Cost (4K, 3x, high-end NVIDIA GPU):** renderer GPU time is about 4.15 ms per frame vs 3.65 with SMAA 1x. The +0.5 ms includes the depth copy and the motion vectors, which T2x needs.

**Launcher:** Anti-aliasing gets a sixth choice, "SMAA T2x (temporal)". It is Gears 2 only for now, works on **both DirectX 12 and Vulkan** (built-in shaders, no SDK; verified on Vulkan 1.4 on the test GPU, where standing-still stability was 0.76-0.86 vs about 7 for the raw frames, with the same edge result), and needs an exe containing `smaa_t2x`; otherwise the launcher uses SMAA, with a note. Scene resolution is hidden for it. A tooltip sentence and the 3 new strings are in the 11 language files (CRLF kept).

**Also checked:** FSR 3.1.6 and XeSS 2.0.2 (Quality, 4K) run with the section-21 inputs with no failures, character motion active. A visual check is still due.

## 23. Ultrawide (`--gow2_ultrawide`, notes/57) with XeGTAO, the motion vectors and SMAA T2x (2026-11-01)

The ultrawide patch makes the scene 1280x550 (x the resolution scale) with a wider field of view, so every frame-layout assumption was checked again.

**The frame [V]** (resolve log, ch1, frame 1502):
- Scene depth: copy 0, 1280x550.
- AO chain: copies 6-10, `k_16_16_FLOAT` at EDRAM 1264, 640x275; the final AO is at `0F2CC000`.
- Velocity: copy 11, `k_16_16` 640x275.
- Scene colour: copy 12, 1280x550.
- Copy rectangles are rounded up to multiples of 8, so 275 rows are copied as 280 and the scene colour as 552.

**Broken, fixed:**
1. **XeGTAO found the game's AO 0 times.** `AoNoteResolve` compared twice the copy's size with the scene, ±8: 2 x 280 = 560 against 550. It now compares the copy with half the scene (280 vs 275, ±8). After the fix: found and replaced on 600 of 600 gameplay frames, the chain skipped as at 16:9.
2. **The character mask scale** came from the velocity copy's size (280 rows), about 2% off vertically, so up to ~10 px at the bottom. It now comes from the scene: half the scene in guest pixels over the render size.

**Checked [V] (ch1, DLSS DLAA 1280x552 unless noted):**
- **XeGTAO's camera:** tan(fov/2) 1.1403 x 0.4900 (2.33:1). The AO is sharp and even across the full width: darkening per fifth of the width follows the game's own profile, with no stretching at the sides.
- **Background motion vectors** vs block matching (`bgcheck.py`, 120 points per frame, turning): median error 0.6-0.9 px, about 90% within 2 px. At the left and right edges, the real motion is −15 px and ours −14.2 to −15.6 px.
- **Characters:** 4-10 px error, the same as at 16:9 (limb animation).
- **AO everywhere** (ch3): added on 545 and then 600 of 600 frames once the game turns its AO off.
- **4K (3x) + ultrawide + SMAA T2x + XeGTAO:** AO replaced on 600 of 600 frames, T2x on every frame, character motion on every frame, no failures.

**Left as is:** the motion pass treats the 552-row copy as the 550-row view (uv = row / 552). That gives a vector error of about 0.4% of the motion, well under 1 px.

## 24. Gears 3 regression check of the shared renderer (2026-11-01, after the 0.3.5 sync)

Gears 3 builds layer 0 from `Gears2-Native/src/layer0_xenos` (its CMakeLists `GN_LAYER0_HOME`), so everything in sections 1-23 is also in a Gears 3 build. It was built from this tree (TU6, our SDK through `Gears3-Native/third_party/sdk.txt`) and run from the installed game data with scratch saves.

**Route:** ch8 opening cutscene. A chapter started with `--ue3_launch` opens paused, so A is pressed at swaps 300 / 450 with `--gow2_input_frames=true`. The cutscene camera moves during about frames 300-1800. The scripted pad never moved the player in gameplay (ch1 after its in-game cutscene, ch8 after the cutscene); the tester watched and saw no movement. The motion checks therefore use the cutscene camera.

**Results [V]:**
- **DLSS / FSR 3.1.6 / XeSS 2.0.2** (native AA, ch8): 1499 of 1499 frames upscaled each, 0 failures, 0 error lines. **SMAA:** pipelines and tables made, 0 errors.
- **Character motion (s.21)** never fires in Gears 3 (0 frames, ch1 and ch8). Its draw filter (k_16_16 target, c0-c3 = the scene camera, affine c5-c12) matches nothing there. Gears 3's object rows are c7-c10 and its camera position c5, so a Gears 3 version needs its own study.
- **Depth copy point:** new switch `--gow2_upscale_depth_at_resolve` (default true = copy at the scene depth resolve, s.21). Gears 3 gives the same accuracy either way, so its depth is intact until the colour resolve:

| Copy point | Median vector error vs block matching (ch8, frames 1501-1505) | Within 2 px |
|---|---|---|
| depth resolve (default) | 0.7-1.1 px | 66-70% |
| colour resolve | 0.9-1.1 px | 67-72% |

  The rest are characters moving in the cutscene; the image edges match within ~1 px.
- **XeGTAO** is off by default (`--gow2_ao` "game") and the launcher offers it for Gears 2 only, so nothing of it runs in Gears 3 yet.

## 25. XeGTAO for Gears 3 (2026-11-01)

**What Gears 3 draws [V]:**
- **Real-time AO in some levels only.** ch12 draws Gears 2's chain exactly: half-res `16_16_FLOAT` at EDRAM 1624, 640x360, four ping-pong copies, then the final one (`104F0000`). It is gone with `[SystemSettings] AmbientOcclusion=False`. Parts of ch3 have it too.
- **None in others.** In ch8 the AO switch changes no copy, no full-screen pass (the same 56 shaders) and no pixel (on/off differences are animation only, 51-55% darker vs lighter). Those levels bake their AO into the lightmaps (Lightmass, as the tester pointed out).
- **Fog pass:** `vs ADB0FB487B4376A8 / ps 724AB5F143DBBFD4`, full-screen into the scene (EDRAM 720, class 3 `2_10_10_10_FLOAT`, float on the host), blend One / SrcAlpha on RGB, after the opaque geometry and before the alpha-blended effects. It is the same shader in ch1, ch3, ch8 and ch12.

**How (shared layer 0, plus Gears 3 defaults in `gow3_app.h`):**
- **Replace** where the game has its chain: the Gears 2 path unchanged, at Gears 2's strength (radius 60, power 2.2). The XeGTAO darkening map has the game's structure and a similar strength (3.5% vs 3.1% mean darkening on the same ch12 frame).
- **Add** where it has none: the multiply before the fog pass (`--gow2_ao_everywhere`, anchor `gow2_ao_fog_ps` = `724AB5F143DBBFD4`). The anchor check now accepts a 16-bit float or 10:10:10:2 scene target.
- **Gentler when adding:** new `--gow2_ao_add_radius` / `--gow2_ao_add_power` (below 0 = the main ones; Gears 2 is unchanged). Gears 3 defaults: 35 / 1.6. The choice is made per frame from whether the game's chain was there last frame.
  - On the ch8 AO buffer, mean AO and pixels darkened by >10% were: 0.934 / 17.9% at 60 / 2.2; **0.961 / 10.6% at 35 / 1.6**; 0.972 / 7.8% at 25 / 1.3.
  - 35 / 1.6 keeps the character, plant and contact shading and drops most of the broad darkening of rock and wall shapes that the baked AO already covers.
- **Results:**
  - ch8: added on 600 of 600 frames.
  - ch12: found and replaced on 600 of 600, the chain skipped (3600 draws, 2400 copies per 600 frames).
  - 0 error lines.
  - Gears 2 unchanged: ch1 replaced 600 of 600, ch3 added 600 of 600; 115 tests pass.
- **Limitation:** in the ch1 frames checked, the first camera the renderer takes is some special pass, not the scene camera (its clip depth follows world height). XeGTAO refuses it ("not a perspective") and stays off, so the game's own picture shows. DLSS reads the same camera there; that is Gears 3's existing camera detection, unchanged here.
- **Launcher:** the AO option is now offered for Gears 3 too, defaulting to "Game's own" there (opt-in), with a Gears 3 tooltip sentence in 11 languages. Judgment is still not yet.

## 26. SMAA T2x in Gears 3 (2026-11-01)

T2x is shared layer 0, so it was already in the Gears 3 build; only the launcher held it back (untested).
- **Checked [V]:** ch8 at 2x scale, the setting of Gears 3's DLSS black-triangle test (notes/14 there: the jitter does not reach its half-res 4x MSAA translucency target).
  - T2x ran on every frame, with 0 errors and the passes compiled. Gears 3's 10:10:10:2 scene takes the no-tone-map path, and the output is clean.
  - **Jitter vs no jitter** (`--gow2_upscale_jitter_scale=0`, same frames): the near-black share is identical (5.88/5.86%, 7.44/7.43%, 8.46/8.45%). The few thousand "black only with jitter" pixels are thin lines along shifted edges (356 of 11,075 inside solid areas), not triangles.
  - **Characters:** Gears 3 gets camera-only vectors (s.21's character motion does not fire there). T2x's 3x3 history clamp limits this to softness on moving characters.
- **Launcher:** the SMAA T2x choice is offered for Gears 2 and Gears 3. The tooltip and "why" texts now say "Gears 2 and Gears 3 for now", in 11 languages.

## 27. First Gears 3 user test: device loss, F9, the dev launcher (2026-11-01)

**Reported by the tester:**
- A local launcher script that starts Gears 3 in a chapter gave a black picture with XeGTAO, and stopped early with the game's own AO.
- Through COGBox (`PortTu` under `[gears3]`) ch1 started with SMAA + XeGTAO, but F9 did nothing.
- The script played at 120 fps while COGBox is set to 90.

**Found:**
- **Black picture = device loss.** `DEVICE LOST: INVALID_CALL`, page fault at GPU VA 0, about 33 s into the XeGTAO run (ch1, 4K).
  - The game-AO run's log just stops after about 30 s, with no error and no crash report.
  - Neither reproduces here: windowed, at 1x or 3x, with the tester's settings, from the tester's current COGBox launch.
- **Suspect, fixed:** at 4K in ch1, the AO-chain detection matched something of the AO's shape once per 600 frames ("found 1"), and once skipped 6 draws and 4 copies of it.
  - The game's chain is now trusted only after 3 frames in a row (`ao_chain_streak_`), and the "add" pass also holds back on any frame where a chain was seen.
  - After the fix: ch1 at 4K with the tester's settings has found 0 and no device loss (150 s). Gears 2 ch1 replaced 600 of 600; Gears 3 ch12 replaced 600 of 600 and ch8 added 600 of 600; 115 tests pass.
- **F9:** `XenosInstallAoDebugKeys` / `Shutdown` were only in `gow2_app.h`. Now also in `gow3_app.h` (the log shows "developer keys on").
- **120 fps:** the script reads the newest COGBox launch log, which was then Oct 31's (`gow3_max_fps=120`), because the tester had not started Gears 3 from COGBox since. The two COGBox launches since carry 90.
- **ch1 (Raven's Nest):** XeGTAO is almost always off there (added on 10 of about 11,000 frames). The camera check refuses the first camera (s.25), so in practice the start of the campaign shows the game's own picture.

## 28. The scene camera in Gears 3: a planar reflection was taken for it (2026-11-01)

**The hands-on test** (Raven's Nest, ch1 with DLSS through the local launcher script, played to the hallway):
- F9 changed nothing.
- DLSS left the same sub-pixel aliasing as SMAA.
- Their log: "no camera constants (camera not a perspective)" on every frame of the session, and only ~100 draws jittered per frame.
- My own scripted Raven's Nest runs had in fact ended in a menu (credits on screen at frame 3900), so their earlier "XeGTAO replaces Raven's Nest's AO" was the menu background, not gameplay.

**Cause [V]:**
- The renderer takes as "the camera" the c0-c3 of the frame's first depth-writing draw into a full-size target.
- In Raven's Nest that draw belongs to a **planar reflection**: perspective along -y, but its depth row is a world plane (`clip z = 2.056 z - 292`, an oblique near plane at z = 142) instead of the distance.
- With that camera, the jitter went to the reflection's draws, so the real scene was never jittered and DLSS / T2x could do nothing. XeGTAO had no constants.

**Fix:**
- A camera candidate must pass the same perspective test as XeGTAO's constants (`MakeGtaoConstants`: the depth row proportional to the distance row). The reflection fails it, and the next draw's camera, the view, is taken.
- Unit test "a planar-reflection camera is not the scene camera" uses the logged matrix (116 tests pass).

**Effect:**

| | Before | After |
|---|---|---|
| Gears 3 ch1: frames with camera constants | 0 of 600 | 599-600 of 600 |
| ch1: draws jittered per frame | ~103 | ~353 |
| ch1: XeGTAO | off | replaces the game's AO (600 of 600; Raven's Nest has real-time AO) |
| Gears 3 ch8: draws jittered per frame | ~107 | 400-600 (DLSS there had also barely been anti-aliasing) |
| Gears 2 ch1: jittered per frame / AO | ~700 / replaced | 700-900 / replaced (unchanged) |

**Black triangles rechecked** (ch8 2x, DLSS and T2x vs no jitter, now that far more is jittered):
- The near-black share is identical (7.40 / 7.40%).
- The pixels that differ are whole edges shifted by several pixels: the cutscene timing differs between runs, while jitter is at most 0.4 px.
- The DLSS output is clean.

## 29. Gears 3 device loss at the ch8 cave (2026-11-01; cause found in s.30)

**Symptom:** `DEVICE LOST: INVALID_CALL`, page fault at GPU VA 0; the picture freezes and input and sound lock.
- The tester, ch8 (Badlands), a few seconds into the cave-in cutscene (falling rocks, smoke, dust): with T2x + XeGTAO, twice in a row (14:34, 14:37).
- Once in my own ch8 intro run (14:44, T2x + XeGTAO), already with the guards below.
- Earlier the same signature in ch1 (12:57, SMAA + XeGTAO, the build before s.27).
- Not reproduced in 9 further intro runs (T2x + XeGTAO, T2x, SMAA + XeGTAO).
- Different from Gears 3's known Raven's Nest loss (its notes/17: DEVICE_RESET / DEVICE_REMOVED, a GPU timeout).

**Guards added (14:40):** none of these may touch a multisampled target, which Gears 3 uses for effects through its 4x MSAA views:
- XeGTAO's depth input (`RunGtao`);
- the "add" multiply's target (`AoAddWanted`);
- the upscalers' scene-depth copy (`UpscaleSaveDepth`, and `RunUpscaler`'s late read).

Each logs once when it acts ("multisampled").

**The tester's matrix at the cave** (15:01-15:12, guards in):

| Mode | Result |
|---|---|
| SMAA, game AO | passed |
| SMAA + XeGTAO | passed |
| T2x, game AO | passed |
| T2x + XeGTAO (crashed twice before) | passed |
| DLSS + XeGTAO | passed |

- The add-pass guard's message never appeared.
- The two depth guards were silent then; they now log too (15:20 build). So the cave fix is most likely the multisampled depth, not yet proven.
- My intro loss with the guards in means a rarer cause may remain. The next loss should come with one of the new log lines, or with none (then it is something else).
- The D3D12 debug layer (Windows "Graphics Tools") would name the call; it is not installed here.

**Update 15:22 (the tester, ch12 Blimp, T2x + XeGTAO):** the same loss a few minutes in, when the airship starts to land after the blimps (a cutscene with heavy post effects).
- **None of the "multisampled" guards logged**, so they are not the answer, and the cave runs passing was luck or timing.
- Right before it: EDRAM alias warnings, with a D24S8 target (base 1440, pitch 1120) reusing tiles last written by `k_16_16_16_16` targets at base 1624 (the post chain).
- **Pattern so far:**
  - T2x + XeGTAO: the cave twice, the ch8 intro (my run), the ch12 airship;
  - SMAA + XeGTAO: once in ch1, on an older build;
  - DLSS + XeGTAO, T2x alone, SMAA alone: none yet.
- **Next:** the debug layer: the local launcher script's debug option adds `--d3d12_debug=true`; the backend already copies D3D12 debug messages into the log ("D3D12 debug: ..."). It needs Windows' "Graphics Tools" installed.

## 30. The device loss's likely cause: the AO chain "remembered" after the game stopped drawing it (2026-11-01)

**The tester's matrix in ch12 (15:42-15:50), with DRED on (the local launcher script's debug option):**

| Run | Result |
|---|---|
| ch8, T2x + XeGTAO | passed |
| ch12, T2x + XeGTAO | device loss at the airship landing |
| ch12, DLSS + XeGTAO | device loss, a few seconds later |
| ch12, DLSS, game AO | passed |

So XeGTAO is the common factor, not T2x.

**DRED recorded no command lists**, so no GPU work was in flight: INVALID_CALL with nothing executing points at bad CPU-side use of a resource, not a bad draw.

**The AO statistics before the DLSS loss:** the game *stopped* drawing its own AO during that cutscene. Replaced 600 of 600, then a window of found 303 / added 297, then added 600 of 600, then the loss.

**The bug:**
- `ao_chain_known_` was set once, when the chain was first confirmed, and never cleared. `AoSkipping()` kept skipping every single-target draw into the chain's EDRAM key (1624, 16_16_FLOAT, pitch 640), and every copy from it, after the game had stopped drawing AO.
- In the landing cutscene the same EDRAM target is reused for the post effects. Their draws and copies were skipped, and a copy that was never made and read later fits the VA-0 fault.
- **ch1's first loss** (12:57) fits too: a one-off false match set the same flag (s.27).

**Fix:**
1. The chain is forgotten on the first frame it isn't seen. Detection keeps running while it is skipped, since its copies still pass `AoNoteResolve`.
2. A copy is skipped only if it goes to the chain's own ping-pong address (`ao_chain_pp_dest_`), so even the one transition frame cannot drop another effect's copy.

**Checked:**
- Gears 2 ch1: found, replaced and skipped on 600 of 600 frames, 2400 copies per 600 (4 per frame, as before).
- Gears 3 ch12: the same.
- 116 tests pass.
- **The tester's retest (2026-11-01, after this fix):** T2x + XeGTAO through the ch8 cave and the ch12 airship landing, where it had crashed before. Both passed, no device loss.

**The ch8 cave and intro losses** (add path only, no chain seen in the logged windows) are not explained by this unless a chain was matched in the unlogged last window. The retest will tell.

## 31. Pre-commit cleanup (2026-11-01)

- **Removed:** the camera-constant scan (`--gow2_camera_scan_frames`, `CamScanDraw` / `CamScanEndFrame`) and its two per-frame log blocks ("motion inputs", "characters: camera then"). They were one-off diagnostics for s.21 and s.30, and the answers are recorded there.
- **Generalised and made opt-in:**
  - The AO trace's vertex-shader constants. Before, always c5-c12 for k_16_16 targets; now `--gow2_ao_trace_vs=first-last` for any traced draw, which is useful for finding shadow matrices (PCSS, s.16 item 5).
  - The raw float dumps. Before, written for every capture set (~20 MB each, 26 GB in one session); now only with `--gow2_upscale_capture_raw_class=<storage class>`, for the motion vectors and the colour resolves of that class.
- **Line endings:** CRLF as the repository's other files (`core.autocrlf=true`). Codegen-touched manifests restored.
- Committed in two steps (everything, then this cleanup), so the history keeps the removed code.

## 32. Judgment and Gears 1 build the same layer 0 (2026-11-01)

GearsJ-Native and Gears1-Native compile `Gears2-Native/src/layer0_xenos` too (their CMakeLists, `GN_LAYER0_HOME`), so every change here is in their builds as well. Neither was built or run in this work. What reaches them:

- **Off unless asked for:** XeGTAO (`--gow2_ao` defaults to "game"), SMAA T2x (`--gow2_upscaler=smaa_t2x`). The launcher offers both for Gears 2 and Gears 3 only.
- **Made Gears 2-only defaults** (off in layer 0, on in `gow2_app.h` through the `game_default` helper as in `gow3_app.h`):
  - `--gow2_upscale_depth_at_resolve` (the early depth copy). Gears 3 measured the same either way (s.24) and is back on the old timing.
  - `--gow2_upscale_character_motion` (Gears 2's velocity-pass layout; it never fired in Gears 3).
- **Applied to every game** (its upscalers): the scene camera must pass the perspective test (s.28). It fixed Gears 3 and is a no-op on Gears 2's cameras; Gears 1's own camera search (job 715) already requires a perspective matrix. The MSAA guards (s.29) only ever refuse work.
- **To check before or after a PR:** a DLSS smoke run in Judgment and Gears 1, which needs their game images.

**Smoke matrix after the cleanup** (each 70-90 s, a local script), all with 0 errors and 0 device losses:
- Gears 2 ch1: SMAA + XeGTAO; DLSS, FSR, XeSS (every frame upscaled, character motion on every frame); T2x; Vulkan + T2x; ultrawide + XeGTAO + DLSS. The game's AO was replaced on 600 of 600 frames wherever XeGTAO was on.
- Gears 3: ch8 DLSS + XeGTAO; ch12 T2x + XeGTAO.
