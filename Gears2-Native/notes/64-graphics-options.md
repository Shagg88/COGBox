# 64. Graphics options: DLSS / FSR, SMAA, and the quality-of-life switches (user 2026-10-22 15:52-16:09)

User decisions: both **DLSS and FSR** as anti-aliasing / upscaling choices, **SMAA kept** as the light alternative
(launcher AA: Off / SMAA / FSR / DLSS); the other features (motion blur, AO, field of view, depth of field / lens flares,
shader warm-up) done "the right way": derived from our own reading of the game code, documented, switchable, off by
default; **no reliance on Canary patches** (their lists are at most a cross-check). Written so the Gears 3 port can
follow the same route (shared layer 0; its UE3 version is close).

## Not done: a standalone sharpener
An RCAS-style pass (ours, MIT, from AMD's published FSR 1 RCAS algorithm) was written (`kSharpenCs` in
builtin_shaders.h, not wired in). Parked (user 16:04): in modern games sharpening is a slider inside FSR / DLSS, not a
separate filter. Numeric check [V]: flat image unchanged; a 0.2 -> 0.8 ramp gets 0.176 at its foot (a slight dark line
at hard edges, normal for sharpeners; RCAS only prevents clipping).

## DLSS / FSR - what the upscalers need, and where it is in Gears 2's frame
Inputs (both): scene colour (HDR, before post-processing and HUD), depth, per-pixel motion vectors, the camera jitter.
From job 070's per-pass log of frame 1500 (ch02) [V]:
- **Scene colour:** copy 15, after draw 1128 - rt0 base 720 (7e3, 1280x720) resolved to 148E8000 (k_16_16_16_16 FLOAT,
  exp -3): the lit HDR scene right before the post passes (bloom / DOF / motion blur / tone map). This is the insertion
  point: run the upscaler on our scaled copy of this target and hand its output to the post passes.
- **Depth:** copy 0, after draw 538 (the depth prepass), dest 15018000; and the live depth target (base 0).
- **Velocity (characters only):** copy 14 after draw 1127 - draws 1111-1126 into a half-res k_16_16 target (rt0 base
  1624 fmt 4 -> 14370000), vs C5B0195E728D405F / ps AC9A377DA32C4E18. The VS skins each vertex (bones c[13+a0..15+a0]),
  transforms it twice (c5-c12: this frame's and last frame's transforms), subtracts, and the PS writes the clamped,
  scaled 2D velocity * 0.5 + 0.5 [V, disassembly]. Only moving / skinned meshes are drawn; half resolution; clamped.
- **Camera matrix:** the final position in that VS is world position x c0-c3 - UE3's view-projection matrix in VS
  constants c0-c3 [V for this shader; to confirm for the base-pass shaders, and whether it is camera-relative
  ("translated world" - then c238-c242 matter)].

### Motion vectors (renderer-side, game-agnostic where possible)
1. Per frame, record the view-projection matrix (c0-c3 of the scene draws) and keep the previous frame's.
2. Camera motion for every pixel: a compute pass reconstructs clip position from depth, applies
   prevVP x inverse(curVP), and writes screen-space motion at full (scaled) resolution.
3. Moving characters: the game's half-res velocity (decoded from *0.5+0.5, un-clamped where possible) where it is
   non-zero, over the camera motion.
4. Verify with a logging run: the matrices of two frames, a still camera (motion ~0) and a turning camera.

### Jitter
A per-frame sub-pixel offset of the 3D passes' viewport (the upscaler's sequence), applied by our renderer when it sets
the viewport for draws into the scene targets (base pass through lighting) - no game shader changes. Not applied to the
post passes, HUD or menus. The depth prepass and shadow maps: prepass yes (same camera), shadow maps no.

### Modes
- First: native-resolution AA (DLAA / FSR "Native AA") at the chosen render scale - replaces SMAA's job, no change to the
  post passes' resolution.
- Then upscaling: render the 3D passes at a lower integer scale and the post / HUD at the output scale. Our scales are
  integers of 720p, which happen to match the standard ratios: 2x -> 3x (1440p -> 4K) = "Quality" (1.5x), 1x -> 2x
  (720p -> 1440p) = "Performance" (2x). Needs per-pass scale in the renderer (today one scale for everything).

### SDKs (downloaded by job 250 into third_party, never committed; user OK 16:09)
- NVIDIA DLSS SDK (github.com/NVIDIA/DLSS, latest v3xx tag; NVIDIA RTX SDKs License, binary DLL). On the user's RTX 3080
  Ti: Super Resolution / DLAA yes; frame generation no (RTX 40/50).
- AMD FidelityFX / FSR SDK (github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, latest v2.x; AMD licence, binaries).
  FSR 4 (ML) targets RDNA 4; on other GPUs (the user's NVIDIA) the SDK's analytical FSR 3.1-class path.
- Both behind one small interface in layer 0 (inputs above -> output texture), D3D12 only.

## The other switches (game side; each from our own reading of the code, both game versions)
1. Motion blur off - find where the motion blur post pass is enabled / its strength comes from (the velocity pass above
   and its post shader), switch it cleanly.
2. Ambient occlusion - first check whether our scaled rendering already gives full-detail AO (copies 9-13 run at
   half of the scaled size); only fix what is actually wrong.
3. Field of view - find the engine's FOV computation (the ultrawide work found the aspect / FOV factor sites), add a
   multiplier.
4. Depth of field / lens flares - prefer the engine's own settings over code changes.
5. Shader warm-up + saved pipelines - shared layer 0 (notes/28 s.10).
   **Step 1 written (not yet built / tested):** `--gow2_renderer_pipeline_library` (off) -> BackendOptions::
   pipeline_library_path = <shader cache dir>\pipelines-d3d12.bin. d3d12_backend.cpp: an ID3D12PipelineLibrary1 opened
   from the file at Init (a driver / GPU change or a damaged file -> a new, empty library); each graphics pipeline gets
   a stable name (XXH3-128 of the root signature blob hash, the fixed-function part of the D3D12 description with its
   pointers cleared, and the VS / PS / GS bytecode); GetGraphicsPipeline first tries LoadGraphicsPipeline (synchronous,
   ~1 ms) - a hit is never a pending draw; misses compile as before (sync or on the workers) and are stored
   (StorePipeline under a mutex; "name exists" ignored). Saved (Serialize to <file>.tmp, then rename) at most every
   60 s from Submit while new pipelines were stored, and at shutdown. Stats: loaded / stored / saves in the renderer's
   CPU-split log line. Shared by Gears 3 (layer 0).
   **Job 250 result [V]:** ch02 capped twice with it on: run 1 stored 497 pipelines (one save at ~60 s, 4.4 MB file in
   build\RelWithDebInfo-tu6\out\shader-cache) and skipped 853 draws while pipelines were pending; run 2 loaded 260
   from the file and skipped **0** draws. Shots of the two runs differ (run 1's pending draws, and ch02's known animation
   variants), so not an image test. Stays off by default until it has been played with.
   **Step 2 (later):** a per-chapter pipeline manifest built during loading screens, so even a first visit to an area
   doesn't compile mid-game.

## Upscaler groundwork (written 17:10-17:30, built by job 251)
- `include/xenos/renderer/upscaler.h` + `renderer/upscaler.cpp`: the interface (Configure / Evaluate / Status), the
  conventions (jitter = the image shift in render pixels, +y down; motion = previous - current position in render
  pixels; row-major matrices as dp4 rows of four float constants; guest depth), Halton(2, 3) jitter with FSR's phase
  count, 4x4 inverse, `ReprojectionMatrix` (prev VP x inverse(VP) x (uv, depth) -> NDC) and `CameraMotion` (the
  shader's formula on the CPU). Unit tests (tests/layer0_upscaler_test.cpp): Halton values, phase counts, inverse,
  a still camera gives 0 motion, motion equals projecting the point with last frame's camera, the depth-remap scale.
- `kMotionVectorsCs` (builtin_shaders.h): depth (t0) -> R16G16 float motion (u0), b0 = the reprojection matrix.
- `renderer/dlss_upscaler.cpp` (GN_HAVE_DLSS): NGX init with our project id, capability check (SR available, driver
  new enough), DLAA when render == output size, flags MVLowRes (+ IsHDR), evaluate with render-pixel MVs (scale 1),
  jitter, reset, pre-exposure, frame time. Compiles against the SDK v310.9.16 headers (syntax check); not yet in the
  build (CMake wiring + nvngx_dlss.dll copy come with the renderer side).
- FSR wrapper: next, against the SDK's ffx_api_dx12.h (copied to out\sdk-headers\fsr by job 251; the loader DLL
  amd_fidelityfx_loader_dx12.dll is loaded at run time, no import library).

### Which draws get the jitter, and where the camera matrix comes from (to be settled by job 251's probe)
Plan: jitter only draws that (a) render into the scene depth target, (b) come before the frame's scene-colour resolve,
(c) are perspective (VTE divides xy by w) and (d) whose vertex shader reads c0-c3 with those four constants equal to
the frame's camera matrix. (d) keeps full-screen passes (shadow projection, fog: screen-space quads that sample scene
depth by pixel) unjittered - jittering them would offset their depth lookups by the sub-pixel jitter. The camera
matrix = c0-c3 of the frame's first depth-writing draw into the scene depth target (the depth prepass); the previous
frame's is kept for the motion pass. `--gow2_upscale_probe=N` logs, every N frames, the draws grouped by c0-c3 value
(with depth target, perspective, depth writes, reads c0-c3) and the frame's resolves, to check (a)-(d) in ch02 / ch10.

### Job 252 probe results [V] (ch02 / ch10, TU6, frames 600-2400)
- **One camera group per frame:** ch02 929 of ~1390 draws (ch10 4,833 of ~5,300) share one c0-c3 value, all into the
  depth target at EDRAM base 0, pitch 1280 (1280x720), 705 / 4,434 of them writing depth, nearly all reading c0-c3;
  it starts at draw 1 (the depth prepass). A second group with the same c0-c3 draws into base 1440 / pitch 800
  (a smaller target - not jittered); screen-space quads have other c0-c3 (e.g. 0.5 -0.5 ...) and don't read them.
  No draw in these chapters has XY pre-divided by W, so "pre-divided" alone can't tell quads from geometry - the
  c0-c3 match does.
- **The matrix is camera-relative:** read as clip = x*c0 + y*c1 + z*c2 + w*c3 (row vector times matrix), it is a
  16:9 perspective (y scale 2.119, x 1.19) looking mostly along the camera's forward axis, with **no camera
  translation** in it (c3 = (-0.0023, 0, -9.99, 0): only a tiny residual and the near-plane term). UE3 renders in
  camera-relative ("translated world") space - the camera position is folded into each object's local-to-world
  matrix - so c0-c3 alone gives camera **rotation** but not camera **movement**.
  => Conventions fix: the matrix is applied as row vector x matrix, i.e. our Mat4 is the transpose of the constants
  (handled in RunUpscaler); camera translation must come from elsewhere - job 253's probe logs c4-c15 of the first
  depth-writing camera draws in a moving-camera run (scripted stick input) to find it.
- **Scene colour:** the frame's only full-screen colour resolve of the 7e3 (k_2_10_10_10_FLOAT, host fp16) target at
  base 720: ch02 copy 16 after draw 1310, ch10 copy 19 after draw 5228 (dest fmt 32, exp bias -3). Earlier, a depth
  resolve of the same size (copy 0) and later the tone-mapped 8888 resolve (last copy) - neither matches the rule.
  Draws 1311-1335 still use the camera matrix after the scene resolve (drawn over the tone-mapped image) - left
  unjittered, as planned.

### Job 253 results [V] and the camera-move fix (job 255)
- DLSS 3.x (DLAA) and FSR 3.1.5 (native AA) both start and run on every scene frame of a 40 s ch02 walk: 1,196 of
  1,196 frames upscaled, 0 failures, 460-924 jittered draws per frame; renderer cost +0.8 ms GPU / +1.6 ms CPU per
  frame (DLSS) at 1x. Frames look clean at a glance (no ghost trails in frames 750 / 1200), but that was before
  camera movement was in the motion vectors.
- The moving probe (frames 360 vs 420): every static draw's c8 (row 4 of c5-c8, UE3's camera-relative
  LocalToWorld) shifted by the same (-2.009, -0.033, -3.132) - minus the camera's move. So the camera's movement can
  be read off the objects drawn.
- Fix (job 255, `--gow2_upscale_camera_move`, default on): each scene draw whose VS reads c5-c8 (and whose c5-c8 is
  an affine matrix) gives a sample keyed by vertex data address + VS hash + c5-c7; this frame against last frame,
  the move most matched objects agree on (within 0.05 units, at least 3 and 30% of them) is the camera's move
  (`CameraMoveFromObjects`), and the motion pass uses prev x translate(move) x inverse(cur). No agreement: turning
  only, as before. A move over 2000 units in a frame is a cut: the upscaler's history is reset. Unit-tested
  (camera-relative reprojection against direct projection; voting with moving objects, duplicates, too few votes).

- Job 261 sweep [V]: DLSS with HDR output runs (0 failed); with ultrawide it never started - the 1x ultrawide
  frame is 1280x550, under the 576-pixel minimum for the scene target. Job 262: minimum 512.

## Engine settings through the game's own config (written 17:55; first built and tested in job 255 - job 254 stopped at its build step)
The cooked config bundle GearGame\CookedXenon\Coalesced_<lang>.bin holds every ini the game reads (63 files in
Coalesced_int.bin, format in ue3/config_bundle.h; our reader writes the real file back byte for byte [V]).
Xe-GearEngine.ini [SystemSettings] has the engine's own switches: MotionBlur, DepthOfField, AmbientOcclusion, Bloom,
UseHighQualityBloom, Distortion, LensFlares, FogVolumes, DynamicShadows, MaxAnisotropy=4, MaxShadowResolution=800,
ShadowFilterQualityBias, DetailMode=2, the TEXTUREGROUP_* LOD biases (World 1, WorldSpecular 2, Effects 1, ...).
GearCamera.ini has the camera FOVAngle of every camera mode (default 80, cover 95, targeting 50, roadie run 105, ...);
GearPawn.ini the vehicle DefaultFOV. TU6's TU__*.ini overlays touch none of these (party beacons, a camera-shake
damping factor). `--ue3_ini_edits` (layer 2, so Gears 3 can use it) edits the bundle at start-up - set a value, or
scale the number a value starts with - writes the result under <user data>\ini-edits and shows the game a view of
the bundle folder in which those files are the edited copies (an overlay device; job 257) and gives the edited files their hashes in the game's start-up hash table (job 258); the game's files are never
written. Planned launcher switches: motion blur, depth of
field, lens flares (each =False), field of view (FOVAngle*= / DefaultFOV*= factor), 16x anisotropic through
MaxAnisotropy=16 instead of the code patch, sharper shadows (MaxShadowResolution). To check first: that the 360
build honours these at run time (job 255: MotionBlur / DepthOfField / LensFlares off -> their passes should disappear
from the probe's resolve list; FOVAngle x1.2 -> c0-c3's x/y scale should shrink by about that).
**Jobs 255-256 [V]: the first redirect never worked.** Edits were written (log), but the camera matrix stayed the
same with FOVAngle x1.2, the frame kept the same 21 resolves with the effects off, and the title screen still said
"PRESS START" with that text edited to "INI TEST" (job 256). Cause (from the SDK's code): VirtualFileSystem::OpenFile
resolves a file's folder through the symbolic links and then looks the name up in that folder, so a link on the
file's own path is never used. Fix (job 257): link the folder to an overlay device that lists the same files, the
edited bundles pointing at our copies, and check at start-up through the same folder-then-name lookup. (Job 255's
MaxSmoothedFrameRate=45 check was invalid as well: it ran with -BENCHMARK, which removes the frame cap anyway.)
**Job 257 [V]:** with the folder overlay the game read the edited bundle - the title screen showed "INI TEST" (12 of 12
bundles checked through the file system) - but then raised XamShowDirtyDiscErrorUI and stayed on the title screen.
The image carries UE3's start-up file hash table (40 entries, "<lower-case name>\0" + SHA-1: the 12 Coalesced bundles
and the startup packages; coalesced_int.bin's entry equals the real file's SHA-1). Fix (job 258): put the edited
file's SHA-1 into that table in memory (found by name and old hash; its read-only page unprotected for the write and
protected again), so the game's own check stays on and passes - not the community patch that skips the check.
