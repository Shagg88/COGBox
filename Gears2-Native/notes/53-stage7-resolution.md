# 53. Stage 7.1 - resolution scale (4K)

Goal: draw the game at N x 1280x720 on each axis - 2 = 2560x1440, 3 = 3840x2160 (4K) - with the same picture, only
sharper. Switch: `--gow2_renderer_resolution_scale=N` (1-4, default 1). Reference design: Xenia Canary's
draw_resolution_scale (BSD-3; ideas and the register maths in pipeline_state.h, which already follows it).

## How it works
- **Render targets** are created at guest size x N. All rectangles the renderer handles stay in HOST pixels; only
  the EDRAM tile maths (render_target_cache.cpp TileBounds / TilePixels / TileCount) converts host <-> guest, rounding
  outwards. So ownership, runs and transfer rectangles need no other change.
- **Viewport / scissor / depth bias**: pipeline_state.h already scales them (Canary GetHostViewportInfo /
  GetScissor with draw_resolution_scale). The fast clear's 2x redirect becomes 2N.
- **EDRAM transfers** map guest pixel -> EDRAM dword -> source guest pixel and keep the host sub-pixel
  (host = guest x N + sub), so N x N host samples of one guest pixel move together (t3.x = scale).
- **Pixel position** (ps_param_gen) for the game's pixel shaders: host centre (h + 0.5) -> (h + 0.5) / N - 0.5, a
  continuous guest position (at N = 1 still floor(position), unchanged). xe_ndc_scale.w carries N.
- **Occlusion queries**: host sample counts / N^2.
- **Resolves (phase A)**: one host sample per guest pixel (the one nearest the guest pixel's centre) into the 1x
  destination, so everything the game reads back stays 1x. Phase B replaces this for textures.

## Phases and pass criteria (written before the jobs)
**Phase A (job 187): scaled render targets, 1x resolves.**
- N = 1 must be unchanged: ch10 capped and ch18 capped shots identical to known variants (after one warm-up run,
  since the pixel shader text changed and every shader recompiles once).
- N = 2 and N = 3: ch10 / ch02 capped run to the end, no new error lines, no crash, builtin shaders compile
  ("shader missing" absent). Pictures look like N = 1 (same 720p output; small differences from the point-sampled
  resolves are expected, no misplaced or missing parts). Render-target memory logged.
- Speed at N = 2 uncapped recorded (more pixels: slower GPU side is expected).

**Phase B: scaled resolves -> scaled textures, output at N x 720.**
- A scaled copy of guest memory for ranges our resolves write (address x N^2, the same tiling with pitch and height
  x N, as Canary's scaled resolve buffer), allocated in chunks as resolves touch them.
- Texture cache: a single-level 2D texture whose pages were all last written by our scaled resolves loads from the
  scaled copy at N x size; shaders get the guest size for unnormalised coordinates and fetch offsets (per-slot scale).
- The front buffer is presented from the scaled destination: the window shows N x 720.
- Pass: N = 2 picture at 2560x1440, visibly sharper, downscaled to 720p it matches N = 1 closely (no shifted,
  blocky or missing effects) in ch02, ch10, ch18; N = 1 unchanged.

**Phase C: settings and sweep.** 4K (N = 3) over all 29 chapters; any leftover per-effect fixes behind switches.

## Job 187 result (phase A) [V]
- Scale 1 unchanged: ch10 capped frames match known variants (frame 900 is byte-identical to job 185's trim-run
  variant); ch18 capped 4/6 identical, the other two differ in 131 pixels by at most 3 levels (the same kind of
  timing speckle seen before) [G: noise].
- Scale 2 and 3 run to the end with no error lines; pictures render correctly (720p output; the frames differ from
  scale 1 only by animation timing - checked by eye on crops).
- Render-target memory: ch10 ~150 MB at 1x, ~600 MB at 2x, ~1350 MB at 3x.
- GPU cost (ch10 capped): 1x 11.5 ms, 2x 29.3 ms, 3x 62.8 ms per frame - of which EDRAM transfers 6.4 / 19.6 /
  44.4 ms. Transfers scale with pixels and are the 4K bottleneck (depth transfers are 9 passes on NVIDIA).
- Found while reading the log: the transfer self-test has failed since job 175 (threaded recording on by default):
  six chains read back 0. Not caused by the scale work (same in job 186's sweep runs, all bit-exact in job 167).
  Cause [G]: DestroyTexture frees RTV/DSV descriptor slots at once; with threaded recording the encoded
  OMSetRenderTargets of the destroyed scratch targets is replayed after a new target reused the slots. Fix (job 188):
  drain the recording thread before freeing.

## Job 188: phase B + fill-bounded ownership
- Resolves also run once per host pixel into a scaled destination (same key as the 1x one, N x size, R32 words);
  GuestWatch marks those pages "scaled current" (any later CPU write or unscaled resolve ends it).
- Texture cache: a single-level tiled 2D texture whose base, pitch and bytes per texel match a scaled destination
  that covers it (8x8-block coverage map) and whose pages are all scaled-current is loaded from that destination at
  N x size (kScaledLoadCs: the destination endian swap, the texture's endian swap, then kUntileCs's conversion).
  The slot goes into a per-draw mask (xe_ndc_offset.w bits); translated shaders divide GetDimensions by N for those
  slots, so unnormalised coordinates and fetch offsets stay in guest texels.
- The front buffer is presented from its scaled destination at N x the swap size.
- `--gow2_renderer_fill_ownership` (A/B, default off): the fill/clear shader takes ownership only of the tiles its
  rectangle covers (conservative vertex bounds, scissor-clipped). Job 185 showed its scissor reaches past its
  rectangle, so the trim never applied; job 180's top depth-transfer pairs are all asked for by this shader.

## Job 188 result [V]
- Transfer self-test: all 7 chains bit-exact again in every run (the drain-before-destroy fix).
- Fill-bounded ownership (ch10 capped 1x): EDRAM transfers 6.7 -> 0.81 ms/frame, with the trim 0.41; GPU 12.1 ->
  7.8 ms. 4K (scale 3) with both: GPU 62.8 -> 19.6 ms/frame (transfers 44.4 -> 1.1). Pictures: the moving-character
  timing variants, plus a deterministic difference of at most 2 levels in a few thousand pixels of static scenes
  (pause menu band, ch18 fog) [G: transfers it no longer makes went through format conversions]. Both switches on by
  default from job 189.
- Phase B: output 2560x1440 / 3840x2160 presented; scaled textures 7-12 per scene (58-139 MB). But on crops the scale-2
  picture still has 1x-sized stair steps on characters and the flying Reaver: some scene textures still go through
  the 1x path. Job 189 logs why (resolve-written textures not taken at the scale, by reason).

## Job 189 result and the change to a scaled copy of guest memory (job 190)
- [V] Misses at scale 2 (ch10): the scene colour is resolved as a 64 bpp surface at 10198000 and sampled as a 32 bpp
  format (54) 1280x720 texture - a byte-level reinterpretation; 0FE00000 is a block of memory several smaller resolves
  share (pitches 640, 352, 800) and is later sampled as one 1280x720 8888 texture; the 800x800 depth texture there is
  only partly resolved. Matching textures to resolve destinations can't express these.
- So (job 190) the scaled data now lives where the game's data lives, per guest address, as Canary does: each guest
  texel byte address A our resolves write holds its S x S samples at (A - chunk) * S^2 + sub-sample * bytes, in guest
  byte order, in one R32_UINT chunk texture (used as a flat word array) per 16 MB of guest memory. A texture (32/64
  bpp, single level, any pitch / tiling / format) whose base-level pages are all scaled-current loads from it with the
  same addressing as kUntileCs. The per-destination textures remain only as the resolve's first step and for display.
- [G] 1x uncapped ch10 was 26.4 ms/frame in job 189 vs 14.7 in job 185, but every CPU part of the renderer grew by
  the same ~1.5x from job 187 on (index 0.35 -> 0.69 ms, frame 0.52 -> 0.84 ms) while GPU work did not: this looks
  like a slower CPU (other load on the PC, lower clocks), not a code change. Job 190 measures again.

## Job 190 result; job 191
- [V] The scene colour (10198000, the 64 bpp surface read as 32 bpp format 54) is now taken at the scale. Left 1x:
  textures whose extent crosses a 16 MB boundary (0FE00000 + 2.5-3.6 MB, ch18's 11C98000 depth) and ch02's partly
  resolved 800x800 shadow map (pages never written by a resolve).
- Job 191: windows are 32 MB, start on a 1 MB boundary where a resolve first needs one, and may overlap: a new window
  copies the overlapping words from older ones (kScaledCopyWordsCs), writes go to every window touched, reads use any
  window holding the whole extent.
- [G] 1x uncapped ch10 again 26.4 ms (renderer CPU 15.8 ms/frame). Job 191 logs the PC's top CPU users and clock.

## Job 192 result; job 193
- [V] PC load: Windows Defender (MsMpEng) used ~1.2 cores and total load was 94-97% while no run was going; Windows
  Search too. [G] That is the 1.5x slower CPU side since job 187 (the harness writes 8-16 MB screenshots now).
- [V] ch10's last 1x texture: 1280x720 8888 at 0FE00000, read once a frame; that frame's resolves into 0FE00000 are
  smaller surfaces (800-pitch depth up to +23F000, 640x360 colour, 352-pitch 64 bpp): 625 of its pages are
  scaled-current, 295 (from +271000) were never resolved into. Neither path holds game data there.
- Job 193 (`--gow2_renderer_scaled_partial`, default on): such a view is taken at the scale when at least half its
  pages are scaled-current and the others were never resolved into (never GPU-written, never scaled) and its first
  page is current.

## Job 193 result; job 194 (phase C)
- [V] ch10's 1280x720 view is now taken at the scale (300 partial views per 300 frames). Measured on the 4K frame:
  detail inside each guest pixel's 3x3 block is 2.3-3.8 levels against 3.8-4.9 between blocks - real 4K detail, not
  an upscale. The distant Reaver keeps coarse steps [G: the game's own half-resolution depth-of-field / fog passes].
- GPU at 4K (capped runs): ch10 21.7, ch18 25.2, ch05 26.6 ms/frame; no error lines, no crash.
- Job 194: all 29 chapters at 1x (the new fill defaults; shots against sweeps 155/175/186) and at 4K.
