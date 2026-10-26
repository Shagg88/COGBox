# 07 - Answers from the Gears 2 chat (2026-10-22 08:26), from their notes and code

1. Renderer differences from the stock path (all known ones):
   - Occlusion queries: not answered early; everything counts as visible (as the emulator). Early answers made
     distant surfaces flicker (their jobs 139-141, notes/51 s6).
   - A texture fetch whose dimension doesn't match the texture (e.g. cube fetch of a 2D texture) reads zero, as in
     Canary (--gow2_renderer_texture_mismatch_zero, default on, job 159). The old white stand-in = flat white water.
   - 16-bit fixed formats (k_16_16, k_16_16_16_16): multiplicative blends into them are only counted (the 1/32 scale
     makes them wrong); no visible case in Gears 2 yet. Watch for it in Gears 3 (the title background uses
     k_16_16_16_16).
   - Tessellation: quad patches drawn (ch18 water); triangle patches / adaptive mode not (Gears 2 never uses them).
     Watch for Gears 3 using them.
   - k_16_16_16_16 support: M2 build B (upl-n105, their jobs 108-109): R16G16B16A16_SNORM, shader output scaled by
     2^(bias-5) (the SDK's snorm16_render_target_full_range); resolves use the SDK rule. Transfers in/out of 16-bit
     float and SNORM16 targets go through UINT views (--gow2_renderer_transfer_bits, their notes/44 s15), or the GPU
     rewrites NaN and -32768 bit patterns.
2. Sweep script: copy tools/chapter_sweep.ps1 (MIT, theirs): boots each SP_*_P map via the launch option (-Cpu adds
   per-map CPU sampling). Their jobs 155/186 used ?loadcheckpoint=0?chapter=N. Shot comparison: hash-against-known-
   variants block in Invoke-Runs, jobs/queue/done/221-cpu-step1-clean.ps1. Their 222 sweep: all 25 SP_*_P maps passed.
   - -BENCHMARK: fixed game time step (uncapped runs show frame cost, not real-time speed); also changes movie timing.
   - -POOLSIZE: never used. They patch the texture pool size directly (PoolSize 196 at 0x8280D534) and make
     RHIGetTextureMemoryStats (0x8280DDC8) return FALSE (boma's Canary "Texture Streaming Fix"), default on since their
     job 154: pool size fixed corrupted ch18 title text, the stats patch fixed slow texture sharpening.
     -> Gears 3 needs the same two sites found (TODO: match 0x8280D534 / 0x8280DDC8 in our image).
3. XamMediaVerification: not seen in Gears 2 (their runs start in chapters, so the title path is unchecked). Harmless.
4. Play defaults (their notes/59): always on: our renderer (takeover), threaded recording, transfer-bits views, mismatch
   reads zero, no occlusion answers, texture-pool fix. Off unless chosen: 1x resolution, SMAA, HDR, ultrawide, fps
   unlock, 16x aniso. Their Launcher.bat sets these.
5. Threads: nothing special for streaming/audio threads; only their PM4 thread at THREAD_PRIORITY_HIGHEST and High
   process priority in tests. Audio timing only checked by ear.

## Gears 3 follow-up: texture streaming (2026-10-22 08:40) - located, NOT patched (Shagg: patch only if needed)

Shagg's rule for these: find the sites, don't patch; watch tests for the symptoms Gears 2 saw (corrupted chapter title
text, textures slow to sharpen); patch only if a symptom appears, and tell the Gears 2 chat either way.

- Pool size: VERIFIED in code - no patch needed in Gears 3. The pool setup (sub_829677A8) reads [TextureStreaming]
  PoolSize from the ini (GConfig call at 0x82967810, value at 0x50(r1)), then checks the command line: Parse(cmdline,
  "-POOLSIZE=") at 0x8296782C (string 0x821CAD60) - if present its number replaces the ini value (r26, 0x82967850 vs
  0x82967858) - and "-REDUCEPOOLSIZE=" (0x821CAD78) at 0x8296786C. Gears 2's version (0x8280D4C8) has no command-line
  check, which is why it patches 0x8280D534. If ever needed: launch with -POOLSIZE=196.
- RHIGetTextureMemoryStats (Gears 2 0x8280DDC8, made to return FALSE): not located yet (10-instruction leaf; the
  matcher's best guesses 0x82EFD830 / 0x82EFC390 / 0x82E80CD0 are weak). Find it only if textures are slow to sharpen.

## 2026-10-22 10:08 - Gears 2: split-screen co-op works (their job 228)
Route on Gears 2: --local_players=2 (SDK patch 25, in the SDK copy we take in job 023), a second pad, then
Co-op Campaign -> Play Split-Screen -> Campaign Lobby. Gears 3's menus differ (not checked) - try once one chapter
plays well on our renderer. Their CPU-path-loads counter is 0 everywhere; they will port our two layer 0 fixes from
the source (not our files) and report whether Gears 2 hits the resolve-only-view skip.

## 2026-10-22 10:15 - Gears 2 ported our two layer 0 fixes: no regression
Their earlier "regression" worry was their job script (input script glued onto the launch argument -> no A presses).
In Gears 2: fix #2 (resolve-only view target) fires once at frame 1 (k_8_8_8_8 base 720 pitch 1280 -> 1F5F8000, raw),
no errors, steady frames - the same frame-1 resolve we see in Gears 3; fix #1 (stacked-3D GPU load) never fires
(counter 0). Their job 230 redoes the screenshot comparisons.

## 2026-10-22 12:36 - Gears 2 wrote the multi-rectangle fill rule in THEIR layer 0 (their job 239)
Option --gow2_renderer_fill_multi_rect (default on): fill shader, 9..48 vertices (multiple of 3), non-indexed,
xy_div_w, 7-float fetch -> ownership = union of its rectangles clipped to the scissor. Same as ours
(--gow2_renderer_fill_rect_lists, 12:31). TODO when we next re-copy their layer 0: take theirs, REMOVE ours (one rule,
one name). They want to hear what copy 1 (544x544 shadow resolve, base 1440) looks like with the union; they would
split into per-rectangle claims if the gaps between strips still cause diffs.

Update 12:40: Gears 2 renamed theirs to OUR option name --gow2_renderer_fill_rect_lists (same rule) and added
"(multi-rect N)" to the "fill-bounded ownership draws" log line. At the next re-copy of their layer 0 simply take
their version (same name, same behaviour) - no merge conflict expected beyond the identical lines.

## 2026-10-22 12:37 - Gears 2 merged our MSAA pieces (colour resolve, 1x->4x transfer, twin clear) into their layer 0
The new shaders compile there. In Gears 2 they should never fire (no real MSAA targets). Our next re-copy of their
layer 0 then carries the same code - check that our three options keep their names and defaults.

## 2026-10-22 12:55 - Gears 2 asked how we applied our title update
Answer sent: we have not applied it (option c). We build the base disc XEX (D7). The game probes `UPDATE:\` at boot,
finds nothing and carries on (job 009 log) [V]. No address re-finding done. Gears 2 plans a shared toolkit patch
(recompiler + runtime load default.xex + default.xexp, mount the TU files as `update:`); we can adopt it later for TU1.
