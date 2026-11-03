# 80. XeGTAO ambient occlusion, upscaler motion vectors and depth, SMAA T2x (Gears 2, Gears 3)

All of this is shared layer 0 (`Gears2-Native/src/layer0_xenos`), so Gears 3, Judgment and Gears 1 build it too
(s.11). Per-game settings are in `gow2_app.h` / `gow3_app.h`, the launcher option in `cogbox/`. Facts are marked
[V] verified, [I] inferred, as in the other notes. Frame and copy numbers are Gears 2 Title Update 6, ch2, frame 1500.

## 1. What it adds

| Feature | Switches | Default |
|---|---|---|
| **XeGTAO** (Intel, MIT; `THIRD_PARTY_NOTICES.md`) at full resolution: replaces the game's half-res AO in the game's own composite (s.4), and is multiplied in before the fog pass where the game draws none (s.5). The game's own AO chain is skipped while ours is shown. | `--gow2_ao=game\|gtao\|off`, `--gow2_ao_quality` (low/medium/high/ultra), `_radius`, `_power`, `_denoise`, `_add_radius`, `_add_power`, `_skip_game`, `_everywhere`, `_fog_ps`, `_show` | `game` (nothing changes) |
| Launcher "Ambient occlusion": Off (the game's `AmbientOcclusion=False`) / Game's own / XeGTAO high / XeGTAO ultra; Gears 2 and Gears 3 | feature `ao`, needs an exe with `gow2_ao_quality` | Game's own |
| **Developer keys** F9 (AO shown: XeGTAO / none / game's own), F10 (debug views), F11 (quality), with a label | `--gow2_ao_debug_keys`, `--gow2_ao_debug` | off |
| **DLSS / FSR / XeSS fixes:** depth copied before Gears 2 clears it, reversed-Z in the reprojection, the SDKs' inverted-depth flags (s.6) | `--gow2_upscale_depth_at_resolve` | on in Gears 2 (`gow2_app.h`) |
| **Characters' own motion** in the motion vectors, from Gears 2's velocity pass (s.6) | `--gow2_upscale_character_motion` | on in Gears 2 (`gow2_app.h`) |
| **SMAA T2x**: SMAA with a two-position jitter and a temporal resolve, as an upscaler kind, D3D12 and Vulkan (s.7) | `--gow2_upscaler=smaa_t2x`; launcher Anti-aliasing "SMAA T2x (temporal)" | off |
| **Scene camera check**: a candidate camera must be a real perspective (s.3); fixes a planar-reflection camera taken for the view in Gears 3 | - | always |

## 2. Gears 2's AO chain, and how it is found

**The frame [V]** (resolve and draw logs, 17 copies):
- Copy 0: scene depth (D24FS8, EDRAM base 0, pitch 1280).
- Copies 5-10: the AO chain on a half-res `k_16_16_FLOAT` target (EDRAM 1624, pitch 640). Its first draw is an MRT quad whose rt1 (8888, base 1808) is the half-res fog, used outside the chain.
  - Copy 6: x = moving-object mask, y = half-res linear depth. Copy 7: raw AO. Copies 8, 9: blurs. All to one address (the ping-pong).
  - A temporal quad reads this frame's ping-pong and last frame's final AO; moving meshes are stamped again; copy 10 goes to a **different address**: the final AO, also next frame's history.
- Copy 11: the motion-blur velocity (`k_16_16`, pitch 640). Copy 12: the HDR scene (where the upscalers hook in).
- With `AmbientOcclusion=False` copies 5-10 disappear and nothing else changes. Destination addresses move with the settings, so **detection never uses addresses**.
- No normal buffer: the base pass writes rt0 only. XeGTAO rebuilds normals from depth.

**Detection** (`AoNoteResolve`, from `ResolveColour`), by shape:
- an R16G16F target whose copy is half the scene's guest size (±8; ultrawide copies 275 rows as 280, s.8);
- the first address that gets ≥3 copies is the ping-pong, the next copy of that target elsewhere is the final AO;
- the chain is trusted only after **3 frames in a row** (`ao_chain_streak_`): a one-off false match at 4K once skipped another effect's work.

**The apply [V]** is the game's fog pass with AO folded in (`ps AC8AE1A05DCEEACE`, full screen into the scene, blend One / SrcAlpha): `scene' = scene * AO * T + fog` with UE3's black occlusion colour. So the game's AO is a plain multiply of the lit opaque scene, after all lights and before translucency and fog.

## 3. Camera and depth maths (`ao.h` / `ao.cpp`, `tests/layer0_ao_test.cpp`)

- **Camera [V]:** VS c0-c3 of the frame's first depth-writing draw into a full-size target, read as columns (`FromConstantsTransposed`). Camera-relative, Z-up, no translation; near 10, far infinite.
- **Depth is reversed-Z [V]:** guest depth = 1 − clip.z/clip.w, so linear depth w = −b / (depth − (1 − k)) with clip.z = k·w + b. Checked against the game's own half-res linear depth: median ratio 0.9994. The flip comes from the viewport's Z scale/offset, which is read per draw (`ViewportDepthMap`), not assumed, so games with normal depth work unchanged. Host depth holds guest depth × 0.5 (`float24_remap`), undone before use.
- **XeGTAO constants** (`MakeGtaoConstants`): depth unpack (mul −b, add 1 − k), tan(fov/2) from the lengths of the clip-x / clip-y rows (so any aspect works), NDC→view, pixel size. It refuses matrices with no perspective divide or no depth range.
- **The same test picks the scene camera for every game:** a candidate whose depth row is not proportional to its distance row is skipped. Gears 3's Raven's Nest draws a planar reflection first (perspective along −y, depth row a world plane: an oblique near plane); taking it had left the real scene unjittered (DLSS / FSR / XeSS / T2x did nothing) and XeGTAO without constants. Unit test "a planar-reflection camera is not the scene camera" uses the logged matrix.

## 4. XeGTAO in the game's own composite

- **Passes** (`xegtao_shaders.h`, ported from Intel's XeGTAO): a 5-level view-depth pyramid (R32F, one dispatch per level), the main pass (normals from depth; slices × steps per preset: low 1×2, medium 2×2, high 3×3, ultra 9×3), 1-3 denoise passes, a debug view. Fitted to the shared compute layout (16 root constants, t0-t3, u0-u1, no samplers): `Load`s instead of Gather / SampleLevel. Non-finite or non-positive distance counts as sky (no occlusion). Every preset's main pass is built at start, so F11 doesn't stall.
- **When:** at the **scene depth resolve** (copy 0). By the scene colour resolve the game has overwritten EDRAM depth base 0 through a 640-pitch 2xMSAA alias for its half-res effects.
- **The stand-in:** the chain's targets (pitch 640) stay at 1x under the 1120 scale threshold at every resolution, so packing XeGTAO into the game's 640x360 copy would waste it. At the final AO copy, `kXeGtaoPackCs` writes a **scene-size R16G16F stand-in** (x = XeGTAO, y = the game's y). For the rest of the frame `AoOverrideViews` rebinds any 2D fetch of that address and size to it, marked as scaled so the shader's texel maths stay in guest units. The binding ends when that address is copied again, and at frame end. The game's own copy still runs, so its history stays its own.
- **Proof it is replaced, not added [V]:** with the stand-in white (`--gow2_ao_show=none`) the static scene is pixel-identical to the game with `AmbientOcclusion=False`.
- **Skipping the game's chain** (`--gow2_ao_skip_game`, 1 draws, 2 copies, default 3): only while XeGTAO or "none" is shown, XeGTAO has run this frame, and neither the final AO copy nor the scene colour copy has happened yet.
  - Draws: only those whose **only** colour target is the chain's target and that write no depth or stencil. The chain's first MRT draw writes the fog (rt1) and must run: skipping it washes the picture out.
  - Copies: only to the chain's own ping-pong address (`ao_chain_pp_dest_`). The final copy runs, so the chain is still found.
  - **The chain is forgotten on the first frame it isn't seen** (`ao_chain_known_`). Gears 3 reuses that EDRAM target for post effects in cutscenes once the game stops drawing AO; skipping their draws and copies left a copy unmade and read later: a device loss (INVALID_CALL, page fault at VA 0).
- **Cost [V]** (renderer GPU time, ch2): 1x 0.96 ms with the game's AO, 0.93 ms with XeGTAO high and the skip; 2x 2.24 / 2.33 ms.

## 5. AO where the game has none: the multiply before the fog

- Gears 2 turns its AO off in some areas (outdoor parts of ch3, ch15) [V]: the frame then has no chain and a fog-only pass, `ps D58B286B84EE971D`, at the same point with the same blend.
- `AoAddWanted` / `AoAddPass` (`--gow2_ao_everywhere`, default on with `gtao`; anchor `--gow2_ao_fog_ps`): in a frame with no chain so far, just before a full-screen draw into one scene target (16-bit float or 10:10:10:2) with blend One / SrcAlpha and a listed fog shader, one full-screen triangle multiplies the scene by XeGTAO's AO (src Zero, dst SrcColor, RGB). That is the game's own apply formula, so the look stays continuous where it switches its AO off. Once per frame; never on a frame where a chain was seen; never into a multisampled target.
- Coverage [V]: every frame is either replaced or added, none both. A level with neither AO nor fog has no anchor.
- `--gow2_ao_add_radius` / `_add_power` (below 0: the main ones) make the added AO gentler where wanted (Gears 3, s.9).

## 6. DLSS / FSR / XeSS: depth and motion vectors

- **Reversed depth in the reprojection [V]:** `ReprojectionMatrix` now takes the viewport's depth map and undoes it; before, every pixel looked ~10 units away, so camera translation gave huge parallax (saturated vectors while strafing). Test "camera motion with a reversed-Z viewport".
- **The SDKs are told the depth is inverted:** DLSS `DepthInverted`, FSR `FFX_UPSCALE_ENABLE_DEPTH_INVERTED`, XeSS `XESS_INIT_FLAG_INVERTED_DEPTH`, from the viewport's Z scale; each wrapper rebuilds its feature when it changes.
- **Depth copied at the scene depth resolve** (`UpscaleSaveDepth`, `--gow2_upscale_depth_at_resolve`): as for XeGTAO (s.4), Gears 2 has cleared the scene depth by the colour resolve, so the motion pass and the SDKs had depth 0 everywhere (every pixel at infinity, where translation moves nothing). Falls back to the old read on the first frame and after a scale change. After the fix, block-matched image motion and the vectors agree within ~1 px [V]. Gears 3's depth is intact until the colour resolve (both points measured the same, s.9), so this is a Gears 2 default.
- **Characters' own motion** (`--gow2_upscale_character_motion`, Gears 2):
  - Gears 2's velocity pass draws every skinned mesh into the half-res `k_16_16` target (copy 11, blend MAX). Its values are not used (mostly 0.5 = "no velocity"); **only its mask is**: written = a character pixel.
  - Its draws carry VS c5-c8 = this frame's object-to-world relative to the camera and **c9-c12 = last frame's in absolute world coordinates**, so last frame's camera position = c12 − last frame's c8 of the same mesh (`PreviousCameraFromObjects`, median over meshes).
  - `VelocityDraw` collects the transforms (c0-c3 must be the scene camera, rows affine); `VelocityResolve` copies the mask (`kCopyFloat2Cs`) before the game reuses the EDRAM; `CharacterMotion` builds a table of ≤32 characters, each a reprojection (`ObjectReprojectionMatrix`) and an origin; `kMotionObjectsCs` gives each masked pixel the nearest character's reprojection within 250 units, others keep the camera motion. Upright (yaw-only) bodies are preferred over attached parts (gun, helmet), whose idle sway otherwise flickered the vectors of a still player.
  - Result [V]: characters' vector error 1-8 px (was 10-35 px with camera-only vectors); a still player gets exactly 0. Limb animation inside a mesh is not modelled. Test "characters move with their own transforms".
  - It never fires in Gears 3 (its object rows are c7-c10), so it is a Gears 2 default.

## 7. SMAA T2x

- An upscaler kind (`rr::upscale::Kind::kSmaaT2x`, `T2xUpscaler`, made in the renderer because it needs the shader cache), so it gets the scene-colour hook (before bloom, tone mapping and the HUD), the jitter, the depth copy and the motion vectors of s.6. Camera cuts reset the history. It works at the render size; the scene-resolution modes are ignored.
- Jitter: SMAA T2x's two positions, (0.25, −0.25) / (−0.25, 0.25) in NDC (y up), i.e. viewport offsets ±(0.25, 0.25).
- Passes (`smaa/smaa_passes.h`): `kT2xPrepCs` (an invertible tone map of the HDR scene and its square root for edge detection; a 10:10:10:2 scene skips the tone map), SMAA edges and blending weights with the sub-sample indices (1,1,1,0) / (2,2,2,0), neighbourhood blending into an RGBA16F ping-pong history, then `kT2xResolveCs`: last frame reprojected (manual bilinear), clamped to this frame's 3x3 neighbourhood, averaged 50/50 and tone-mapped back.
- Result [V]: standing still, the output changes ~10x less frame to frame than the jittered input; no double edges or trails mid-turn. Built-in shaders, so D3D12 and Vulkan. Cost at 4K (3x): about +0.5 ms over SMAA 1x, including the depth copy and the motion vectors.
- Gears 3 [V]: no new black triangles from the jitter (its half-res 4x MSAA translucency target, Gears 3 notes/14); characters get camera-only vectors there, which the 3x3 clamp limits to softness.

## 8. Ultrawide (`--gow2_ultrawide`, notes/57)

The scene is 1280x550 × scale; the AO chain and velocity are 640x275, copied as 280 rows (copy rectangles round up to 8). Two fixes [V]: the AO detection compares the copy with half the scene (280 vs 275, ±8), and the character mask's scale comes from the scene, not the copy. XeGTAO's tan(fov/2) gives 2.33:1 with no special case, and the AO follows the game's own profile across the width; background vectors match block matching within ~1 px at the edges.

## 9. Gears 3

- Gears 3 draws real-time AO in some levels only (ch1, ch12, parts of ch3: Gears 2's chain exactly); elsewhere (ch8) its AO is baked into the lightmaps and the AO switch changes no pass [V].
- Its fog pass is `vs ADB0FB487B4376A8 / ps 724AB5F143DBBFD4` into a 10:10:10:2 scene, the same shader in every level checked: `gow3_app.h` sets it as `gow2_ao_fog_ps`.
- Where the game has its chain, XeGTAO replaces it at Gears 2's strength; where it adds, `gow3_app.h` sets radius 35 / power 1.6, which keeps contact and character shading and drops most of the broad darkening the baked AO already has (ch8 mean AO 0.961 vs 0.934 at Gears 2's 60 / 2.2).
- The launcher offers the AO option and T2x for Gears 3 too. Character motion (s.6) does not fire there.
- The developer keys are installed in `gow3_app.h` as in `gow2_app.h`.

## 10. Multisampled targets and device losses

- Gears 3 draws effects through 4x MSAA views of the scene. Nothing of ours may touch a multisampled target: XeGTAO's depth input, the add pass's target and the upscalers' depth copy each refuse one and log once ("multisampled").
- The Gears 3 cutscene device losses (INVALID_CALL, VA 0) were the remembered AO chain of s.4: fixed by forgetting it on the first frame without it, and by skipping only the chain's own copies. Retested through the ch8 cave and the ch12 airship landing, where it had happened: no loss.

## 11. Other games, and per-game defaults

- Judgment and Gears 1 compile this layer 0 too; neither was built or run in this work. XeGTAO and T2x are off unless asked for, and the launcher offers them for Gears 2 and Gears 3 only.
- Gears 2-only behaviour is a default in `gow2_app.h` (only while the switch still has its compiled-in default, as `gow3_app.h` does): `--gow2_upscale_depth_at_resolve` and `--gow2_upscale_character_motion` are off in layer 0.
- Applied to every game: the scene-camera perspective test (s.3; Gears 1's own camera search already requires a perspective matrix) and the multisample guards (s.10), which only ever refuse work.
- XeGTAO is opt-in in both games' launcher option ("Game's own" by default).

## 12. Developer switches and tests

- Off by default: `--gow2_ao_log=N`, `--gow2_ao_debug_keys`, `--gow2_ao_debug`, `--gow2_ao_show`; `--gow2_ao_trace_frames` (every full-screen pass of those frames: target, shader hashes, blend, texture addresses, PS constants) with `--gow2_ao_trace_class` (also draws into targets of that storage class) and `--gow2_ao_trace_vs=first-last` (VS constants; how the c5-c12 layout of s.6 was found, and the way to find shadow matrices); `--gow2_renderer_hlsl_dump_hashes`; `--gow2_upscale_capture_frames` with `--gow2_upscale_capture_raw_class` (exact float dumps of the motion vectors and of copies of that storage class, ~20 MB each).
- Tests: `tests/layer0_ao_test.cpp` (depth maps, view positions at 16:9 and 21:9, Gears 2's measured camera against the game's own linear depth, refusals, the reflection camera) and `tests/layer0_upscaler_test.cpp` (reversed-Z camera motion, per-character motion).
- Checked in game [V]: Gears 2 (chapter sweep for detection; ch1 with SMAA, DLSS, FSR, XeSS, T2x, Vulkan + T2x, ultrawide), Gears 3 ch1, ch8, ch12.

## 13. Open

- **One device loss at Gears 2's ch1 → ch2 transition** (DLSS + XeGTAO ultra, HDR), about 7 s after the Prescott movie as ch2's gameplay began: INVALID_CALL, page fault at VA 0, no command lists recorded, nothing of ours logged before it. Not reproduced: the same transition replayed with the same settings, and a fresh ch2 start, both ran clean. The AO state (`ao_chain_known_`, the stand-in, `ao_ready_`) is reset every frame, the movie's too. Whether it is ours is open; the deciding comparison if it is seen again is the same transition with "Game's own".
- Judgment's AO (its chain and apply pass are not studied); Judgment / Gears 1 smoke runs.
- Fewer XeGTAO denoise passes when an upscaler's temporal accumulation is on.
- Split-screen: one replacement per frame today; untested.
- PCSS for the dynamic shadows (idea): find each game's shadow projection shaders (`--gow2_ao_trace_vs`) and substitute a blocker-search / penumbra variant by hash; cheaper first steps are higher shadow-map resolution (the shadow targets stay at 1x under the scale threshold) and a better fixed filter.
