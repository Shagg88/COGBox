# 18 - "Vertex explosions" (Shagg, ch22 play 2026-10-23 00:34) - research and what we already do

## Already right in shared layer 0 [V, hlsl.cpp @27958a8]
- Vertex words outside the fetch constant's buffer read as 0 (hlsl.cpp ~450, ~885) - xenia-edge 9e9d3cdd3's fix for
  over-allocated particle quads "exploding to garbage".
- Legacy multiply (0 * x = +0) in mul / mad / dot, dot summed from +0, mad not fused (hlsl.cpp ~81, ~121, ~669).

## Candidates [S = xenia-edge commits, research agent 2026-10-23; G = guess]
1. [S] Memexport ordering (xenia-edge 31a696a9a, 2860617cb, 16df25981): export output landing in guest memory AFTER the
   game wrote new data there, or a resolve copy-back overwriting export output (and the reverse). Xenia-edge removed its
   readback path (a84f4d1ff, 70ad7c527): consumers fetch from the GPU copy; upstream pre-loads the range before the
   export draw (RequestRange) and marks it GPU-written (RangeWrittenByGpu). [G] Our layer 0 reads back 26-1051 export
   ranges per 300 frames and feeds ~3,800-12,600 vertex ranges from exports - check the order: export submit ->
   readback lands -> consumer snapshot, and any CPU write to the range in between.
2. [S] Export write checks: eA validation bits, element index < eA.w size, 3-bit endian incl. 8-in-64/128 (ours has
   these per notes/15), addresses masked to 0x1FFFFFFF (2fce2d895), extended-range float16 in exports (02c6bc6e6).
3. [S] Stale converted index buffers (quad lists) after the guest rewrites them (1b0e9d00e) - check our index cache
   invalidation.
4. [S] Volition "vertex explosions" = scalar-multiply rounding pushing an index across a batch (3390fc219,
   mulsc_round_toward_zero) - cheap A/B if the others are clean.
Next (PC, Shagg's go): per-draw log for draws whose vertices come from export ranges at an explosion moment
(--gow2_draw_log_frames around a report time), max |position| and NaN/Inf count.

## Play log 20261023-005759 (Sovereign, Act 1) [V]
- Shot frame 3600: a large flat brown surface stretched across the corridor (exploded mesh) and Marcus rendered solid
  black - so black characters are not only ch22, and explosions occur on the way to the notes/17 device-loss spot.
- memexport lines only for frames 301-1500 (draws 1382 / 87 / 90 / 49, vertex ranges read from exports 1151 / 10800 /
  36074 / 38375), none after; explosions at ~3600 -> not obviously export-fed [G - the counter may only print with
  export draws in the range].
- "unknown (N colour)" 63,802-115,824 per 300 frames in that corridor (stale tiles, patch 0005).
- [G] link to notes/17: exploded triangles covering the screen many times over at 2x could be the GPU stall before
  the device loss. Next: per-draw log with max |position| around a reported explosion; 0006 on the hang.

## 01:05 - NPCs only (Shagg) -> memexport readback race; patch 0007
- Shagg: the explosions are random background NPCs, not Dom/Marcus. Session 20261023-005759: 10k-38k vertex ranges
  per 300 frames read from memexport output in those corridors -> NPCs are skinned through memexport [G, strong].
- [V] PollExportReadbacks writes landed readbacks through TranslatePhysical (rexglue: physical_membase_, never
  write-protected; watches protect membase_ + heap_base_), so no watch callback: a readback landing after the game's
  CPU rewrote the range overwrites the newer CPU bytes silently (the code comment claims the watch sees it).
- Patch 0007 --gow2_renderer_memexport_readback_guard: copy only pages still GPU-current; count pages kept.
- Shot 13200 of the same session: more stretched sheets near Dom (no hang; session ended at ~13,800 frames, before
  the ~20,500 device-loss point).

## 01:20 - 0007 v2 (export pending mirror) replaces v1 (readback guard)
- Gears 2 chat (01:09) found the main race [V by reading]: a CPU store anywhere in a 4 MB block ends GpuCurrent() for
  every exported page in it (guest_watch.cpp widens page callbacks to the block), and UploadVertexSlot then uploads
  guest memory - still the PRE-export bytes until the readback lands - over the export results.
- v1's guard used GpuCurrent() per page, which those block bumps make unreliable -> dropped. v2
  (--gow2_renderer_export_pending_mirror): pages covered by an in-flight readback keep the mirror.
- Free A/B (no code): --gow2_renderer_memexport_readback=false (pale skin may return, job 041).

## 01:50 - next suspects (0007 never acted, so not the export race) and free A/B switches (no build)
1. [G] Vertex / index reuse memos (jobs 174/178, default on): a range drawn again in the same command list and
   release epoch reuses its first ring copy without reading guest memory again. If the game rewrites a small
   dynamic buffer between two NPC draws without a release point we see, the second NPC gets the first one's vertices
   -> stretched sheets. A/B: --gow2_renderer_vertex_reuse=false --gow2_renderer_index_reuse=false.
2. [G] Export readback / CPU path (Gears 2's free test): --gow2_renderer_memexport_readback=false (pale skin may come
   back - job 041 - but explosions should change if this path is involved).
3. [S] Checked and fine: UAV barrier after each export draw / before readers (d3d12_backend.cpp PrepareDraw:
   UAV barrier between consecutive exporting draws, transition out of UAV before ordinary draws); eM write masks are
   kept per component like Canary (whole eM temps written at export, as dxbc_shader_translator_memexport.cc).
Plan: one launcher session with 0009 (watch narrow) first; if explosions remain, a session with (1), then (2).

## 02:20 - job 071 E1-E3 (ch1 start, Marcus's quarters, standing still) [V]
- E1 control fine; E2 (memexport readback off) and E3 (memexport off): several objects (bookshelf, desk) solid black
  = the DXT textures Gears 3 moves with memexport (job 041) - readback is required, as known. No NPCs in view, so no
  explosion data (Shagg 02:03: standing still shows nothing). Walking routes wait for patch 0011 (pad sticks).

## 02:55 - job 074 walking routes (build 070) [V shots]
- W1 ch22: the route walks through the shipyard (Marcus moves, takes fire), runs into a fence corner near 5100 and is
  downed (red bleed-out from 6000). No explosion visible in 24 shots (every 300 frames).
- W2 ch1: walks to the cabin door by 1500, then stands there (the door needs a button). No explosions seen.
- Both runs: FATAL 0, ~62 fps. Job 075 = routes v2 (ch1 presses X/A at the door and walks the corridors; ch22 another
  direction), shots every 150 frames.

## 03:00 - job 075 routes v2 (build 070) [V shots]
- R1 ch1: reaches the round cabin door by 1350 ("Get Dom", checkpoint), but X / A do not open it; the objective star
  is to Marcus's RIGHT (another doorway). He stands at the door to the end (shot sizes constant from 1500).
- R2 ch22: walks into container walls and around a truck; no squad in view, no explosions in 57 shots.
- 'CPU-path loads of our resolve pages' (play mode, ch1): 34 / 101 / 2 / 0 / 4 in the first 1500 frames (loading,
  textures moving), then 0 for the rest of the run - fits memexport texture moves at load, not a steady leak.
- Job 076: calibration (C1 ch1: does Y look at the objective; how far a 20-frame right turn goes; C2: where ch2 starts).

## 03:05 - job 076 calibration [V shots]
- C1 ch1: from frame ~1440 the tutorial "OBJECTIVES - to display your current objectives, hold LB" is up and the game
  waits: Marcus does not turn or walk (Y and right-stick steps did nothing; shots 1440-4200 identical). That is why
  jobs 074-076 never left the cabin. Job 077 K3 holds LB there.
- C2 ch2 ("Act I - Chapter 2: Abandon Ship"): walking works (no tutorial). Frame 900: yellow tile-sized bricks over
  the top third; frame 1200: the whole lit scene in wrapped / banded colours, HUD fine (one-frame format garble, like
  Gears 2's rainbow flash). Notes/15 has the rendering side; job 077 K1/K2 repeat it with dense shots and a shadow
  compare.

## 03:30 - job 077 K3 (ch1, LB held at the tutorial) [V shots]
- LB frees Marcus: he turns and walks around the cabin (opening credits running), then ends at the round door again
  (no X press after the LB step). Job 079 D1: LB, walk to the door, X / A presses, then forward walks with turns.

## 03:50 - job 079 D1 (ch1 route v3) [V shots] - ch1 routes parked
- LB frees Marcus; X / A at the round door do nothing (the door stays shut through the credits). The objective star
  pointed RIGHT at the open hatch (bunk room side). A 20-frame right-stick step turns him most of the way round (K3),
  so a route needs ~5-8 frame steps. Parked: scripted ch1 routes cost ~10 min a round; ch2 ("Abandon Ship") walks
  without a tutorial and is closer to the Sovereign corridors (SICKBAY / device-lost spot, notes/17).
