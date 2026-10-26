# 15 - ch22 (Act 4 ch4) squad black / dark red in gameplay - time-boxed and parked 2026-10-23 00:02

Symptom: in ch22 gameplay the AI squad (Dom, Dizzy, ...) is black or blackish red (Shagg: "slightly red, as always");
Marcus is mostly fine; cinematics are correct. Parked after one evening's time box (Shagg: "if we don't figure it out now
we move on").

## Facts [V]
- Not the merge: our old layer 0 copy shows it too (job 062). Not the resolution scale: black at 1x and 2x (job 063).
- It IS our renderer: the SDK's own Xenos emulation (--gow2_gpu=sdk) draws the squad perfectly (job 064).
- Shadow-mode compare, ch22 gameplay frames 2550-3150 (job 065, run 20261022-234117):
  - scene depth 99.99% equal; nearly all small buffers 100%;
  - per-object shadow depth D24S8 base 1440 pitch 1120 (544x416 / 544x320 -> 14028000 fmt 22): 75-84% equal, depth max
    diff 16777215 (cleared vs drawn) at (524,208); the tool labels the differences "reference approximation (Canary
    ownership heuristic)", ~4100 tiles where the ownership rules disagree;
  - screen shadow / attenuation buffer 8888 base 720 1280x720 (draws ~721-793, -> 14028000 fmt 6): 13-28% of pixels
    equal, but ours only slightly LIGHTER (max diff 52/255, one channel; last writer box VS B5A1208D037C2F4F,
    ps 7E4D5C16BFD4550D, write mask 1, multiplicative blend) - the light-function family of notes/10;
  - lit scene 2_10_10_10 (-> 143C0000) ~17% equal.
- Not fixed by (Shagg watched each run): --gow2_renderer_ownership=canary (+ fill_ownership=false) (job 066);
  turning off, one at a time, msaa_transfers, fill_rect_lists, clear_msaa_twins, msaa_colour_resolves,
  resolve_view_targets (job 067; transfers=false gives a black screen).
- Our renderer skips some "vertex shader export mode is multipass" draws (memexport) in ch22: ~800 in the first 300
  frames, 25-130 per 300 frames later; the SDK path runs them.

## Leads not yet tested
1. The skipped multipass-memexport draws (implement or log which shaders / what they write) - the SDK runs them.
2. Dump the per-object shadow depth and the lit-scene copy as images from both renderers on one gameplay frame
   (needs a resolve-dump tool: patch to the shared renderer).
3. The red tint: a lighting value losing green/blue, or a buffer read in the wrong format [G] - check the small
   k_16_16_16_16 328x184 copies (97% equal in job 065) and the half-res 7e3 4x MSAA copy (97%).
4. [S] Research (2026-10-22): UE3 DynamicLightEnvironment = one directional + one SH light, visibility by CPU line
   traces; ModShadowColor defaults to black; Xenia GoW3 tracker (xenia-canary/game-compatibility #39) has light /
   occlusion-query notes but no dark-character report.

## After the time box (research while Shagg plays, 2026-10-23 00:10-00:35)
- Multipass skips probably harmless [G, from Gears 2's source reading]: Canary (draw_util.cc
  IsRasterizationPotentiallyDone) and rexglue v0.10.0 (src/graphics/util/draw.cpp) treat vs_export_mode kMultipass as
  "no rasterization" and drop the draw unless its VS writes eM - as our layer 0 does. Check one skipped VS hash for
  eA/eM writes to be sure.
- MSAA sample positions (Canary cc4981a, "missing shading in tiled MSAA titles"): unlikely for ch22, because the SDK
  path (rexglue v0.10.0, RTV on NVIDIA) also draws at D3D12's standard positions and gets ch22 right. Low priority.
- Shared-code gap found [V]: d3d12_backend.cpp uses D3D12_FILTER_ANISOTROPIC whenever max_anisotropy > 1, even for
  point-filtered fetches (xenia #1563: off with UseComputedLOD=false / point). Our shadow and light-function shaders
  fetch depth / shadow maps with UseComputedLOD=false. Not tested; sent to Gears 2.
- Research (00:40) [S]: Xenia's kMultipass = "VS outputs ignored, memexport only"; Canary runs it only when the VS
  writes eM (draw_util.cc / d3d12_command_processor.cc) - same rule as ours. Xenia fixes in this area: bc89a5822 (red/
  blue swap and the write mask), da47dfaac (signed round bias), 53f98d1fe (memexport anywhere in control flow, 8/16bpp).
  Our hlsl.cpp memexport (ported from the SDK's DXBC translator) handles rb swap, endian incl. 8-in-64/128, round half
  away from zero [V]; it writes whole eM vectors (no per-component write mask) [V] - the one difference from bc89a5822
  worth checking, but the SDK path (same port) draws the squad right, so probably not this bug [G].

## Best lead so far (00:50, research + job 065 data)
- [S] UE3 docs / Smedberg GDC09: in GAMEPLAY a character is lit only by its DynamicLightEnvironment (one directional +
  SH) plus ONE composite modulated self-shadow (bCastCompositeShadow) that multiplies the directional term by
  ModShadowColor (default black; the docs' example is red (1,0,0)); CINEMATICS use per-shot lights / shadows instead.
  A per-object shadow is a light-space depth map sized to the bounds plus a filter border.
- [V] job 065: cutscene frames 150/300 - the per-object shadow depth (D24S8 base 1440 pitch 1120, resolved -> fmt 22)
  matches the SDK exactly in depth ("depth max diff 0"), only stencil differs. Gameplay frames 2550-3150 - DEPTH
  differs, max diff 16777215 (cleared vs drawn) at (524,208) of a 544-wide copy, i.e. the right-hand border strip.
- [G] So: in gameplay our per-object shadow map has wrong depth (likely its filter border / an area the game cleared),
  every squad member samples itself as shadowed, and the directional light becomes ModShadowColor -> blackish red.
  Marcus (player pawn) may use a different shadow path. Next test (needs the PC): dump that resolve from both
  renderers on one gameplay frame (a per-pixel depth diff map), and log the clears / draws into base 1440 before it.

## 2026-10-23 00:40 - strongest lead: never-transferred 64bpp tiles (Gears 2 chat 00:37 + Shagg's play log) -> patch 0005
- [V] play log 20261023-003356: "unknown (142800 colour)" per 300 gameplay frames = ~476 never-transferred
  colour -> depth tiles a frame; frame 1001: the per-object shadow depth #16 D24S8 base 1440 pitch 1120 reuses 81 + 57
  tiles last written by k_16_16_16_16 targets (base 1624 pitch 640 / 400). The depth transfer skipped every 64bpp
  colour source (and colour word transfers needed equal bits per sample): those tiles keep stale host content.
- [V] Shagg's menu/Continue session showed NO black characters at the same spot -> history-dependent, as stale tiles
  would be [G].
- Patch 0005 (Gears3-Native\patches\to-gears2): --gow2_renderer_transfer_mixed_bpp, Canary's 32/64bpp sample mapping
  in kTransferPs / kWordTransferPs + 4 self-tests. Waiting for Shagg's go; test = ch22 jobs (black before) with it on.

2026-10-23 01:00 Shagg (Sovereign session 20261023-005759): characters FLICKER black frame to frame, not solid black
(the frame-3600 shot caught Marcus in a black frame). [G] Fits stale never-transferred tiles (patch 0005): what the
shadow map holds there changes with whatever wrote that memory last, frame by frame. Pending-shader skips were 0 in
most of those 300-frame ranges, so skipped draws are a weaker explanation.

## 2026-10-23 01:31 - patches 0004-0007 in play (launcher session gears3-20261023-012701) [V]
- "unknown (N colour)" 0 in gameplay (was ~140k / 300 frames) with 0005 on, yet Shagg: "Nothing was fixed" - black /
  flickering characters and NPC explosions remain. => the stale tiles were NOT the cause of the black characters.
- 0007 never acted ("page checks that kept the mirror ... 0") -> the explosion cause is elsewhere.
- NEW: shot 6000 (Marcus's quarters, Sovereign): top third of the screen covered by a yellow brick pattern of
  tile-sized rectangles with magenta squares; Shagg: "it was all a mess". [G] 0005's colour word transfers between
  32bpp and 64bpp colour targets put reinterpreted words into a buffer the game reads (e.g. bloom) - before 0005
  those tiles kept older content. Also possible: 64bpp owners whose last write was a known clear now go to the
  transfer instead of the depth path's known-clear branch. Asked Shagg to switch 0005 off (extra-args.txt #).
- Next for black characters: Gears 3 game patches --gow3_patch_no_light_env_shadows / --gow3_patch_no_dynamic_shadows
  (Canary Performance A) - needs a codegen build (Shagg's OK).

## 01:50 - 0005 yellow-brick regression: suspects (0005 off for now)
- [G] Colour word transfers 32bpp <-> 64bpp now copy tiles whose owner was (logically) cleared or never really
  written; the old code left the destination's own content. The depth path has a run.known (clear) branch that 0005
  now bypasses for 64bpp owners; the colour path has no known-clear branch at all.
- Next if 0005 is wanted again: depth-only (64bpp colour -> depth) under its own switch, colour mixed-bpp off.

## 01:47 - 0009 (watch narrow) on: flicker still there (Shagg) [V log gears3-20261023-014648]
- "page watch ... narrow widening on" each summary; Gears 3 build 070 (Gears 2 d980aa2). Flicker unchanged.
  (In narrow mode the 4 MB-loss counter is not counted, so this session can't say how often the old widening would
  have hit GPU pages; the earlier sessions predate the counter.)
- Next: --gow3_patch_no_light_env_shadows=true (Shagg adds it to extra-args.txt), then --gow3_patch_no_dynamic_shadows.

## 01:58 - night-run clues
- Shagg: in ch22, when NPC explosions appeared, the characters were NOT black - the two never show together. [G] one
  data path with two failure modes: characters skinned through memexport; wrong export output = garbage normals
  (black / dark) or garbage positions (explosions). Marcus (player) mostly fine fits if only AI characters use it.
- Session gears3-20261023-015015 (light-environment shadows OFF, reuse off): shot 6000 shows a very dark room with
  Marcus almost black -> darkness persists without light-environment shadows [V one shot].
- Night jobs: 071 (ch22 S1 control / S2 no light-env shadows / S3 no dynamic shadows; ch1 E1 control / E2 readback
  off / E3 memexport off), 072 (ch22 M1 readback off / M2 memexport off / M3 control).

## 02:10 - job 071 S1-S3 (ch22, build 070, scripted start, shots 2700 / 3000) [V]
- S1 control, S2 --gow3_patch_no_light_env_shadows, S3 --gow3_patch_no_dynamic_shadows: in all three the squad
  members (left and right of Marcus at frame 3000) are black-red; Marcus is fine. Turning off the light-environment
  shadows or ALL dynamic shadows changes nothing -> the black squad is NOT a shadow problem.
- Remaining lead: the AI characters' own vertex data / lighting inputs - memexport skinning (job 072: M1 readback off,
  M2 memexport off), and Shagg's clue that explosions and black never coincide.

## 02:30 - job 072 M1-M3 (ch22, build 070, scripted start, shots 3000 / 3300) [V]
- M1 --gow2_renderer_readback=false (export readback off), M2 --gow2_renderer_memexport=false, M3 control: in all
  three the squad member right of Marcus is black / black-red; Marcus fine. -> the black squad is NOT caused by
  memexport output or by export readback (the 01:58 "one data path" guess is wrong for the black part).
- M2 only (memexport off): a stack of boxes right of the container shows a garbage texture (coloured blocks), frame
  3000 and 3300. So something on screen in ch22 depends on memexport (likely a texture/particle the game builds with
  exports); turning memexport off is not a usable workaround.
- Remaining leads for black: texture data (wrong/moved DXT or untiled texture for the character diffuse/normal map),
  per-character lighting constants (SH/light-env constants written by the CPU into a buffer the shader reads).
  Job 073's tile trace + draw log with texture fetch constants should name the draw and its textures.
- 02:35 check [V]: both job 071 hooks really fired (gow3.log of runs 015925 / 020041: "light environment shadows
  skipped", "dynamic shadows skipped"); dynamic shadows off cut draws by frame 3000 from 2.62M to 2.04M (-22%),
  light-env off by 0.5%. So the "not a shadow problem" conclusion stands.

## 02:40 - job 073 T1 (ch22, --gow2_gpu=sdk shadow compare every 150 frames, build 070 / layer 0 d980aa2) [V]
- The run hit its 200 s limit at about frame 2850 (shadow mode is slow), so the frame-3000 draw log was not written.
  The scripted route ran differently (Marcus downed at 2550): the pad script counts frames, game time is real time.
- Frame 2700, the SDK's picture shows the squad correctly coloured. Our side, per resolve:
  - copy 1, per-object shadow depth (D24S8 base 1440 pitch 1120, 544x576): 69% bytes equal; all explained as the
    reference's own ownership estimate (180k px) - same as job 065;
  - copy 5, screen shadow / light attenuation (8888 base 720): 13.8% px equal, "unexplained" 794k px; the largest is
    one channel 0xC7 vs 0xFF, written by draw 890 (vs B5A1208D037C2F4F ps 7E4D5C16BFD4550D, write mask 1, blend src 6
    dst 0 = multiply by the destination) - the light-function family again (notes/10);
  - copy 7, the lit scene seen as 2_10_10_10 (our 7e3 target base 720 reinterpreted, 0 draws): 17.8% px equal, all
    757k different px "transferred but different"; worst e.g. (1219,386) ours 02B10843 vs ref 175731C5 = ours far
    darker there (R 67 vs 453 in 7e3 bits);
  - copy 20, the same after 5 more draws: 53% px equal; copy 40 (final 8888 front buffer): 95.6% px equal, max diff 4.
- Also seen: 6 DXT1/DXT5 textures (256x256 to 2048x2048) loaded on the CPU path from pages our resolves had written
  ("CPU-path load reads our resolve pages") - texture memory reused after a render-target resolve. In this mode the
  CPU bytes are the right ones; worth checking that the play mode (no resolve readback) never reads the GPU mirror for
  such pages after the game has streamed a texture there.
- The log only names the largest-difference pixels, not where the squad is. Patch 0013 (compare dump) writes ours /
  reference / last-writer draw per pixel on chosen frames; tools/compare_dump.py makes the pictures and lists the
  draws that wrote the dark pixels inside a box around a squad member.

## 02:50 - real hole found: resolve pages never armed in the write watch -> patch 0014 [V code + log]
- shadow_renderer.cpp (layer 0, resolves to guest / T2): after each resolve, MarkGpuWritten(dest extent) marks the pages
  GPU-current, but nothing arms the CPU write watch there; the memexport path does Watch() first. The texture cache arms
  only pages it loads a texture from. So: resolve -> pages GPU-current, unarmed -> the game reuses the memory (e.g.
  streams a texture in) -> no callback -> pages stay GPU-current -> GPU-path texture loads read our OLD resolve from the
  mirror (wrong / black texture), CPU-path loads read the right bytes but are counted.
- Evidence: the renderer's own counter "CPU-path loads that read our pages (this range, should be 0)" in the ch22
  control run (job 071 S1, play mode): 74 in frames 1-300, then 0-21 per 300 frames in gameplay. Job 073 named six
  such textures: DXT1/DXT5 256x256 to 2048x2048 at 18E0A000, 1A0C7000, 1B2BE000, 1B54F000, 1B598000, 1B680000.
- Fits the reports: missing textures, black ground, characters black / flickering (a texture reloads from stale or
  fresh bytes depending on what else touched its pages).
- Patch 0014: --gow2_renderer_watch_resolves (default off): arm the watch before marking, as memexport does. Use with
  --gow2_renderer_watch_narrow=true (on in the launcher). Test: the "should be 0" counter goes to 0, then look at the
  squad / ground.

## 02:58 - CORRECTION to 02:50 (evidence for 0014 downgraded to [G])
- Job 073 ran in shadow mode, where our resolves are NOT written to guest memory (no "resolves to guest (T2)" lines),
  yet it logged the same "CPU-path load reads our resolve pages" textures. There the only GPU-current marks come from
  memexport - and Gears 3 MOVES DXT textures with memexport (job 041, notes/18 E2/E3). So those textures, and most of
  the "should be 0" counter in play mode, are probably legitimate moved textures (read correctly after the export
  readback), not CPU writes the watch missed.
- The code gap stays real on reading (resolve pages are marked GPU-current without arming the watch; memexport arms),
  but whether it ever bites in Gears 3 is unproven. 0014 stays a cheap, switchable test; the counter can't prove it
  either way. A better test: count GPU-path loads of pages GPU-current from a RESOLVE whose CPU bytes changed (needs a
  hash of the CPU bytes at mark time) - only if 0014 shows any visible change.

## 03:00 - link to notes/10 [G]
- Job 073's light-attenuation difference (copy 5: ours one channel 0xC7 vs 0xFF, draw 890 = box VS B5A1208D037C2F4F,
  ps 7E4D5C16BFD4550D, multiply blend) is the same family as notes/10's light-function draws (ps FC367470036D7CFE),
  which sample DXT textures at 0x1B1BF000 / 0x1B6E7000 / 0x1B6EE000 - the same address range as the textures Gears 3
  moves with memexport (jobs 073-075: 0x1B2BE000, 0x1B54F000, 0x1B598000, 0x1B680000). If such a draw reads a moved
  texture before its export readback has landed, it gets the old bytes for that frame -> darker on some frames
  (flicker?). Not tested; shadows-off did not change the squad, but light functions are not shadows. The compare
  dump (G3-0013) will show which draw / texture the dark squad pixels come from.

## 03:25 - ch2 rainbow / yellow bricks: reproducible, and the cause located (jobs 076-077) [V]
- Job 077 K1 (ch2 load, play mode, build 070, default switches, shot every 30 frames): rainbow at frames 300-390, 480,
  1020-1230; yellow bricks at 450, 660-990; all inside the 'Abandon Ship' intro cutscene; gameplay from 1260 fine.
  Same on the second load (job 076 C2) -> deterministic, 2 minutes, no human.
- Job 077 K2 (ch2 shadow compare every 150 frames): copy 34 = k_8_8_8_8 base 720 pitch 1280 1280x720 -> 1F5F8000
  (the final picture): frame 300 98.3% px equal; frames 450-1800 0.14-0.18% equal, max diff 247, verdict "transferred
  but different ~920,000 px, unexplained 0": its pixels were last written by a TRANSFER, e.g. (313,279) ours 3E8003CF
  vs ref 00F7FAF5 - our value is 10:10:10:2 scene bits, the reference's a normal 8888 colour. So the final 8888
  picture holds the HDR scene's bits from the other format at the same base (k_2_10_10_10 / 7e3 base 720 pitch 1280)
  = the rainbow. Draws 2377-2402 before that copy: 1 drawn by us + 24 dummy points.
- The lit scene itself (copy 16, k_2_10_10_10 base 720 -> 143C0000) is ~52% equal at every frame (same as ch22).
- Same EDRAM base/pitch, two formats: #2 k_8_8_8_8 base 720 pitch 1280, #5 7e3 base 720 pitch 1280, #10 k_2_10_10_10
  base 720 pitch 1280. Job 078 traces the source tiles (995/996/725/726) and shows our picture in shadow mode.

## 03:40 - job 078 T1 tile trace (ch2 shadow, tile 995, frame 600) [V]
- Copy 34 broken again from frame 450 (frame 300 fine). Tile 995, end of frame 600, in order:
  copy 16 (lit scene resolve): #10 k_2_10_10_10 gets a transfer from #5 7e3 (format change) - normal;
  draws 2375 / 2379: ownership moves through #11 / #13 (bloom chain at base 1624);
  draw 2406 (prim 4 rect, vs A9C7FE3382D41904, ps 2F284B68A112ED3E - the tone-map pass): ownership rect -> #2
  k_8_8_8_8 base 720, TRANSFERRED from #5 7e3 (guest words, format change), then "colour drawn into it (slot 0,
  mask 15)"; copy 34 resolves #2. Yet the pixels' last writer is the transfer (ID 1), not draw 2406.
- So the tone-map draw is issued (counted drawn; depth func always, stencil always, no blend) but covers none of those
  pixels in frame 600, while at frame 300 the same shaders (draw 2396) wrote the whole screen (diff 1). Play mode:
  rainbow = no coverage, yellow bricks = top third not covered [G]. Suspects: the rect's positions / viewport /
  scissor / window offset in the cutscene, or the pixel shader killing pixels. Not a skipped draw on comparison frames.
- Job 080 adds B4 (shader + pipeline waits 50 ms) and B5 (async pipelines off) per Gears 2, and B1's dump gives the
  per-pixel writer map of copy 34.
- 03:58 [V, disc ucode out\shaders-038\vs-A9C7FE3382D41904.sdk.txt]: the tone-map VS takes its position straight from
  the vertex buffer - `vfetch_full r1, r0.x, vf0, FMT_32_32_32_32_FLOAT, Stride=8`, `max oPos, r1, r1` - no
  constants. So "draw issued, covers nothing / part" = the vertex bytes we read for that draw were wrong (stale,
  not yet written, or an older ring copy) [G until B7 / Gears 2's job 302 V]. Gears 2 300b: async pipelines off
  still flashes -> not a skipped draw.

## 04:15 - job 080 (build on Gears 2 ee53490 with G3-0012/13/14; all runs FATAL 0) [V]
- B1 compare dump, ch2 frame 600, copy 34 (final 8888 at base 720): the writer-ID map has ID 1 (transfer) on ALL
  921,600 pixels - the tone-map draw (draw 2405, record 4046: vs A9C7FE3382D41904 ps 2F284B68A112ED3E, depth/stencil
  always, no blend, mask F) wrote ZERO pixels. Picture (tools\compare_dump.py): ours = rainbow, ref = the proper
  tone-mapped frame, lit scene = the 10:10:10:2 input (notes/img/ch2-f600-ours-ref-scene.jpg).
- Rainbow still present (shots ~2.6 MB) with: B2 watch_resolves + narrow (8 frames), B4 shader / pipeline wait 50 ms
  (7), B5 async pipelines off (15), B6 constant shortcuts off (6), B7 vertex / index reuse + texture memo off (7);
  baseline K1 13. None of these is the cause.
- The tone-map VS's vertex range is NOT read through the memexport path (the census logs only the memexport VS itself
  as an export reader) -> its vertices come straight from guest memory at our draw time.
- Next: G3-0015 (--gow2_renderer_log_vs=A9C7FE3382D41904) logs those vertex bytes per draw; compare good vs rainbow
  frames.

## 04:20 - job 080 B3 (ch22 compare dump 2550 / 2700, our picture shown) [V]
- The squad was behind the camera (Marcus alone in all shots 1950-3150) - no black-squad pixels to trace yet.
- Frame 2700: final picture (copy 37) 42k px differ, all tiny (mean brightness 0.259 vs 0.259); the lit scene (copy
  17) and light attenuation (copy 2) "differences" have the same mean brightness as the reference (0.228 vs 0.229,
  0.913 vs 0.911) -> the low "% equal" figures for those copies are rounding-level, not the squad bug.
- Job 081: same build, camera turned left at 2450 / 2950 to bring the squad into view, dumps at 2700-3300 (Q1 shadow)
  + a play-mode run with shots every 60 (Q2).

## 04:40 - job 081 [V shots]
- Q2 (play mode, build ee53490): the squad is black / black-red ahead of Marcus at frames 3000-3300 (with blue armour
  lights) - the bug is present in this build.
- Q1 (shadow, dumps 2700-3300): the camera turn looked at empty containers - no squad in the dumps.
- Gears 2 301: their ch10 flash never happens with --gow2_gpu=sdk (0/7). Gears 3's ch2 rainbow DOES (jobs 078 T2,
  080 B1) - mailed.
- Job 082: shadow mode, hold forward from 1950 (follow the squad), dumps 2400-3600.

## 04:55 - job 082: in SHADOW mode our squad matches the reference -> the black squad is takeover-only [V]
- Shadow compare (emulator drives, our picture shown, build ee53490), Marcus following the squad: frame 2700, final
  picture (copy 41 -> 1F5F8000): in a box around the squad member ahead of Marcus (x 450-700, y 300-560) ours and the
  reference have the same mean brightness (0.118 vs 0.118); the remaining diffs are tiny (tone-map draw 1712). Frame
  2100: the squad member at the bottom left is normally lit in our picture. notes/img/ch22-f2700-shadow-ours-ref.jpg.
- Play mode (takeover) on the same build shows the squad black / black-red (job 081 Q2). So the bug needs takeover -
  like Gears 2's ch10 flash (their job 301). Takeover-only inputs: textures from OUR resolves (T2), our PM4 / fences,
  no emulator resolves.
- Job 083: shadow mode + --gow2_renderer_resolves_to_guest=true (our resolves feed the textures) - squad black?

## 05:20 - job 083: our resolves feeding the textures (T2) do NOT turn the squad black [V]
- Shadow mode + display ours + --gow2_renderer_resolves_to_guest=true (exe ee53490), follow-squad script.
- Frame 2100 final picture (copy 42): two squad members + Marcus in view, ours = reference (mean brightness 0.226 vs
  0.227; remaining diffs tiny, all from the tone-map draw 1812). notes/img/ch22-f2100-t2-shadow-ours-ref.jpg (top ours,
  bottom reference). Frame 2700 (copy 49, Marcus at a wall): 0.134 vs 0.134; box 450,300-700,560 0.108 vs 0.109.
- So T2 is ruled out. What play mode still has that 083 did not (layer 0 source, takeover preset + gpu_system.cpp):
  --gow2_gpu=ours = OUR graphics system: our PM4 processor runs the ring (pm4_execute), our draws / shader loads
  (pm4_own_draws), our occlusion reports (pm4_own_zpd), split PM4 / renderer threads (pm4_split), fast constant
  writes (pm4_fast_constants); plus the SDK's drawing off and no comparisons.
- Job 084 splits them (P1 SDK system + takeover, P2 + our PM4, P3 + our draws/zpd, P0 play, P4 no split,
  P5 slow constants), shots every 60.

## 05:45 - job 084: the squad is already black with the SDK's graphics system + takeover -> NOT our PM4 processor [V shots]
- P1 (--gow2_gpu=sdk --gow2_renderer=takeover: the emulator runs the ring, its drawing off, our renderer draws):
  frames 1500-1620 (chapter title "Batten Down the Hatches", squad walking ahead of Marcus) - the squad members are
  black / dark red. notes/img/ch22-084-p1-takeover-squad-black.jpg (left: job 083 shadow at 1500-1800, no squad in view
  yet; right: P1). P2 (+ our PM4 processor) and P3 (+ our draws / ZPD) look the same; P0 (play mode) shows the
  black-red squad member next to Marcus at 3600-3960 (play mode reaches gameplay later by frame count).
- Also visible: the takeover runs' picture is warmer / browner than the shadow run's greener grade [G: colour grading
  table read stale in takeover? check against the reference].
- So the cause is on the renderer side of takeover: the emulator's drawing (and its resolves into guest memory) off,
  no comparisons. T2 stats in P1: 0 resolves skipped, but "CPU-path loads that read our pages" 34 / 78 / 3 / 5 per
  300 frames ("should be 0"): streamed DXT textures (format 18/20, 128-2048) loaded from pages our resolves wrote.
- Job 086: W0 P1 control, W1 + watch_resolves/narrow (0014), W2 + texture_stale_log, W3 shadow + gpu_skip_draws by
  hand, W4 shadow + compare 0, W5 play + watch_resolves.
- 05:55 [V] close-up: notes/img/ch22-squad-shadow-vs-takeover-crop.jpg (left: job 083 shadow frame 2100, squad armour
  grey and lit; right: job 084 P1 takeover frame 1560, the squad member's whole body is a blotchy black / dark-red
  pattern, not just dark) -> looks like WRONG TEXTURE BYTES, which fits the stale-resolve-pages lead.
- Correction: the "warmer colour grade in takeover" note above is withdrawn - the frames compared showed different
  scenes (the shadow run leaves the opening cutscene by frame 900, takeover only after ~1300; same pad script).

## 06:00 - why only takeover: the mechanism in layer 0 code [G - code reading, job 086 tests it]
- texture_cache.cpp (GPU load path, ~line 905): when a texture loads, every page whose version changed is copied from
  guest memory into the GPU mirror - EXCEPT pages still "GpuCurrent" (last written by one of our resolves, T2): those
  are kept as they are in the mirror. A fresh texture entry counts every page as changed, so the same rule applies.
- A page stops being GpuCurrent only when a CPU write bumps its version, and a CPU write is only seen if something
  armed a watch on that page. Our resolves do not arm one unless --gow2_renderer_watch_resolves=true (patch 0014,
  default off).
- So: our resolve writes pages X -> later the game streams a character texture (DXT) into X -> nobody watched X, the
  version is unchanged, X is still GpuCurrent -> the texture load keeps our OLD resolve bytes -> blotchy black/red.
- Shadow mode hides it [G]: the emulator draws too, its own texture cache uploads textures from the same resolve
  pages and protects them, so the streaming write fires a write callback that our watch also sees. In takeover the
  emulator uploads no textures -> no protection -> no callback.
- Test: job 086 W1 / W5 (watch_resolves + watch_narrow) should bring the squad back; W2 logs the textures.

## 06:35 - job 086 W0-W2: the resolve watch does NOT fix the squad -> the 06:00 mechanism is wrong for this bug [V shots]
- Exe from job 085 (Gears 2 e1ec04a). W0 (SDK + takeover) frame 1560: squad member blotchy black / dark red (as 084).
  W1 (+ --gow2_renderer_watch_resolves + --gow2_renderer_watch_narrow): the same, pixel for pixel by eye
  (notes/img/ch22-086-w0-w1-squad-crop.jpg). Gears 2 307a: the watch rules don't fix their ch10 flash either.
- W1 log: "GPU-written pages marked CPU-written by the 4 MB widening" 0 (narrow on) vs W0 89015 / 100400 per 300
  frames - the switches did act.
- W2 (texture_stale_log=30): 294 events, mostly streamed DXT1 textures (fmt 18, 32-1024 px) reloaded after a CPU write
  with no overlapping resolve, plus resolve targets (fmt 22 / 54) reloaded right after their own resolves (expected).
  Nothing points at the squad's textures.
- Still pending in 086: W3 shadow + --gpu_skip_draws (emulator's drawing off, compares on), W4 shadow + compare 0,
  W5 play + watch.

## 06:45 - job 086 W3: shadow mode + --gpu_skip_draws=true is BLACK -> the emulator's drawing provides what the squad needs [V shots]
- W3 (shadow + display ours + T2 + --gpu_skip_draws=true, comparisons on): frame 1560 squad member blotchy black /
  dark red, the same as takeover. Shadow with the emulator drawing (082 / 083) is lit. W4 (shadow, compare 0) never
  had the squad close in its shots (timing differs), so it neither confirms nor contradicts.
- Read of layer 0 (MemexportOnlyDraw / PollExportReadbacks): Gears 3 moves DXT textures with memexport copies (job
  041); our export results reach guest memory only through an async readback whose memcpy raises no watch callback.
  Two emulator outputs reach guest memory in shadow mode: its resolves (--readback_resolve) and its exports
  (--readback_memexport). Job 087 switches each off with the emulator still drawing (Z0 control, Z1 resolves off,
  Z2 exports off, Z3 both off).

## 07:20 - job 087 Z1 [V shot]: emulator drawing, but its resolves kept out of guest memory -> squad black-red again
- Z1 (shadow + display ours + T2 + compare 0, --readback_resolve=none, --readback_memexport=true): frame 1740, two
  squad members blotchy black / dark red. So the squad needs RESOLVE results in guest memory (CPU-visible) - which our
  renderer never writes there (T2 writes the GPU mirror only). Fits layer 0's own counter "CPU-path loads that read our
  pages ... should be 0" (3-78 per 300 frames in ch22): DXT/BC textures are untiled on the CPU from guest memory.
  (Z0 control: no squad close-up in its shots - the control rests on 082/083.)
- Xenia Canary: one GPU copy of guest memory; resolves / memexport write it and EVERY texture loads from it.
- Shagg 07:18: "do it the Canary way". Patch G3-0018 (tools/make_patch_0018.py): before a CPU-path texture load reads
  GPU-current pages, those pages are read back from the mirror into guest memory (GPU work finished first; only
  GPU-current runs, so newer CPU bytes elsewhere are kept). --gow2_renderer_cpu_loads_from_mirror, default ON.
  Job 088 test-builds it from a copy of layer 0 (build\RelWithDebInfo-0018) and runs ch22 SDK+takeover (on/off) + play.

## 07:35 - job 087 finished; job 088 stopped (base changed); plan aligned with Gears 2
- 087 Z0 (control) and Z2 (emulator exports kept out of guest memory): no squad close-up in their shots - inconclusive.
  Z1 (emulator resolves kept out) is the result: black-red (07:20 entry).
- 088 stopped before building [V log]: Gears 2 committed e38f63e (G3-0016) meanwhile, so shadow_renderer.cpp no
  longer matched the base 0018 was made on (the hash check worked; nothing was built or run).
- Gears 2 07:28 / 07:40 (Shagg asked both chats to agree; "do it the Canary / edge way"): step 1 = port Canary's
  readback_resolve into layer 0 - every resolve of ours also copied back into guest CPU memory
  (--gow2_renderer_resolve_readback=none|fast|full, Canary's default "fast"); step 2 (only if needed) = port Canary's
  RTV ownership / transfer code. Their 301 O: emulator resolves -> 0 flashes / 7; 307 R: our resolves -> 5 / 10.
- That covers G3-0018's purpose (CPU-visible guest memory gets our resolve bytes) in Canary's own form -> 0018
  WITHDRAWN in favour of their step 1; G3-0019 (GPU decode of BC textures) on hold until step 1 is measured.

## 08:15 - job 089 (Gears 2 patch 0020, Canary readback_resolve; exe on abc1a57): PARTIAL at best [V shots]
- SDK takeover, frame 1560 (notes/img/ch22-089-r1-full-r2-fast-r3-none.jpg, left to right):
  R1 readback=full: squad member (hat) still dark red / black (in shadow by the wood stack - darker scene, timing
  slower: 12171 GPU waits / 300 frames). R2 fast: the same member LIT (grey armour, blue lights) but another squad
  member left of him still dark red. R3 none (control): blotchy black-red, as before.
- Readback counters as designed (full: landed = readbacks; fast: ~40% landed, rest "previous copy not ready").
- Gears 2 311 (ch10): full 0/10 flashes, fast 2/10, today 1/10. Next from them: patch 0021 = BC (DXT) textures loaded
  on the GPU from the mirror (Canary texture_cache) - --gow2_renderer_gpu_block_textures. That removes the CPU-memory
  dependency entirely; test it on ch22 when committed.

## 08:35 - job 089 R5/R6: Canary readback_resolve=full FIXES the ch2 rainbow [V shots]
- R5 ch2 play, --gow2_renderer_resolve_readback=full: 53 shots (frames 0-1560), NONE rainbow (no ~2.6 MB frames),
  scenes lit normally. R6 same with =none: rainbow at 330-390, 480-510, 750-780, 1050-1200 (as before).
  notes/img/ch2-089-r5-full-vs-r6-none.jpg (left full, right none; frames 360 / 1110).
- Cost: full = 32.4 fps vs 61.9 fps (GPU waits on every resolve). "fast" not tested on ch2 yet.
