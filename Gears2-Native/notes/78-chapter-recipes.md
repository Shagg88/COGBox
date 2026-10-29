# 78 - Chapter-opening "recipe packs" (shader pipelines prepared behind the loading screen)

Status: C3 (the recipe pack, below) built in job 612; A/C test pending. Earlier status: idea backed by measurements. Order: after the open GitHub issues and the SteamOS / CrossOver
D3D12 test build (notes/76, notes/77). Marks: [V] measured, [G] guess.

## The idea (Shagg, 29 Oct)
Make every chapter's first play smooth without shipping any game data:
- Ship **recipe lists** only: `pipelines.list` records (shader fingerprints + pipeline state, 144 bytes each, no game
  data - patch 0058).
- On the player's PC, behind the loading screen, find those shaders in the **player's own game files** (patch 0068's
  package reader, same fingerprint key) and have the driver build them for **their** GPU and driver.
- Package as **one common pack per game** (shaders used in many chapters, built once at the main menu) plus **a small
  per-chapter list** of what is new in that chapter's opening.
- A recipe that is missing or can't be prepared falls back to today's behaviour (the draw pops in a moment late; no
  freeze). The worst case is never worse than now.

Why not ship shaders or driver caches: translated shaders are game-derived (no game data in releases), and driver
pipeline caches only work on the exact GPU + driver that made them (Microsoft: cached PSOs are adapter/driver-specific).

## Overnight recording (29 Oct 04:51-08:30, C:\COGBox\work\recipes\20261029-045119\)
`C:\COGBox\Record-Chapter-Recipes.bat` -> `work\tools\record_chapter_recipes.ps1`: every chapter started straight in
(Gears 2/3: `GearGame_P?loadcheckpoint=0?chapter=N`; Judgment: its campaign + Aftermath maps), empty shader cache per
chapter, 150 s each, scripted pad (A, then BACK/A), developer builds (job 608, frame log on), 30 fps (no launcher
settings passed), default graphics settings.

- All 29 Gears 2 and 31 Gears 3 chapters ran the full time, no crashes [V]. Gears 3 ch0 reached gameplay by ~90 s [V].
- Recipes per chapter opening [V]: Gears 2 106 / 260 / 435 (min / median / max), 7,817 total;
  Gears 3 217 / 408 / 1,127, 14,821 total (biggest: ch14 1,127, ch18 1,037, ch16 889). Lists ~1-2 MB per game.
- Pop-in sits in the opening [V]: skipped draws are almost all in a chapter's first 20-40 s
  (Gears 3 ch14: 8,140 in the first 20 s, a handful later; Gears 2 ch5: 1,848 + 4,896 then ~0).
- Shader holds and pipeline waits 0 ms in every window checked [V].

## Repeats across chapters [V]
| | Gears 2 | Gears 3 |
|---|---|---|
| Unique recipes | 5,955 of 7,817 (24% repeats) | 8,995 of 14,821 (40% repeats) |
| Unique pixel shaders | 2,396 | 4,248 |
| ...in 2+ chapters | 41% | 34% |
| ...in half the chapters or more | 67 | 90 |
| ...in every chapter | yes (e.g. A4A965C189287B99) | yes (e.g. 368EFF02BB04803C) |
Playing in order, only ~40% of a chapter's opening pixel shaders are new (Gears 2 2,396 of 5,917; Gears 3 4,248 of
9,660); some chapters are 4-6% new.

## Limits to design around
- (Superseded by C3 below: the drawn vertex shaders ARE in the files, as the code part of a record with its layout
  dwords filled in.) Vertex shaders are patched by the XDK at bind, so their bytes in the files never match (job 367): prepare **pixel
  shaders** (warm-only build, patch 0068); the driver keeps the stage, so the real pipeline later costs ~0.1 ms instead
  of 15-35 ms (job 362c). About 70% of drawn pixel shaders are in the package data [G from earlier notes].
- Player settings (MSAA, upscaler, HDR) change render-target formats / sample counts in the recipes; pixel-shader
  warming still helps across settings. Consider a second recording with common settings.
- Budget: the old "build everything at load" test (~10,000 shaders) doubled a cold chapter's frame time. Per-chapter
  lists of a few hundred, on spare workers, stopping when the load ends, live draws first.
- SteamOS: vkd3d-proton keeps its own shader cache; measure separately.
- Chapter detection: several Gears 2 chapters share one persistent map; the per-chapter list may need the chapter
  number or all lists for that map.

## Build plan (2-3 jobs + one play test)
1. Ship the common pack + per-chapter lists (launcher/package), pick the list when the chapter's map opens.
2. Match recipe pixel-shader fingerprints against the shaders found in the opened packages; translate and warm-build
   them on spare workers within a budget, stop when the load ends.
3. Measure: same chapter openings on a cleared cache, with and without, using skipped draws and the frame log.

## Tests 29 Oct [V]
- Best case B (recorded pipelines + the chapter's own recorded shaders, not shippable): Gears 2 ch1 skipped draws
  218,664 -> 7, ch5 107,245 -> 1,867.
- Real C (pixel fingerprints only, warm-only builds, job 611b): Gears 2 ch1 206,549 -> 81,497; Shagg saw no
  difference. 526 shaders built; the warm template only exists after the first draw, so most of the load was missed.

## C3: the recipe pack (job 612) - B rebuilt from the player's files
Analysis of Gears 2 ch1 (78 vertex shaders its recipes use, 29 packages + GlobalShaderCache) [V]:
- None of the 78 is in the files as is; every one is the **code part (tail)** of a vertex record, differing in 2-62
  dwords. 98% of those are fetch instructions (vertex buffer slot, offset, stride, format filled in at bind); the
  rest are ALU exports the console turns off (`C80F8000 00D0D000 E2000000` -> `C8000000 00000000 02000000`),
  presumably outputs the paired pixel shader does not read.
- Rule: a recipe vertex shader = the file record's tail with its control-flow dwords, fetch instructions, ALU exports
  and write-nothing ALU instructions taken from the recipe (42% of its dwords for ch1). With the real matching code
  (`xenos/renderer/recipe_pack.h`), **76 of 78** rebuild bit-exactly; the 2 others are probably in packages not in that
  copy. Every rebuild is checked against the recipe fingerprint, so a wrong one is never used.
- Pixel shaders: 168 of ch1's 306 recipe pixel shaders were in the 29 packages of that copy (the game reads all of
  them; the run logs give the real count).
- Pack (`work\tools\make_recipe_packs.ps1` from a recording -> `work\recipes\recipes-<game>.rcp`): all chapters'
  pipeline records + per vertex shader those dwords and 4 probe positions (FNV-1a), so most file records are ruled out
  without a full rebuild.
- Game side (`--gow2_renderer_recipe_pack=<rcp>` with `--ue3_shader_prebuild=true`): the reader hands over every
  vertex record too; listed shaders go into the shader cache (`ShaderCache::Get`, as a draw's) at up to ~4 ms a frame,
  and each recorded pipeline is queued (start-up backlog priority, patch 0058) once both its shaders are ready - no
  warm template, no waiting for the first draw. Log: "chapter recipe pack (frame N): ..." every 300 frames.
- Job 612 run 1 [V] (Gears 2 ch1, cold): skipped draws 191,268 (A) -> 11,296 (C), but C stuttered for ~40 s (p99
  134 / 171 ms in frames 1-1200 vs 50 / 150 ms for A): every chapter's recipes whose shaders were in the shared
  packages were built, ~3,300 pipelines where ch1 needs 416, on all 12 workers.
- Job 612c: pack v2 has per-chapter lists; a list starts once half of its marker pixel shaders (ones no other chapter
  uses; 9 in 10 of all its pixel shaders if it has under 8) have been found, so shared packages cannot start one.
  Checked offline on ch1's own 17 packages in load order: ch1 starts after the 8th, ch2 (same Hospital map) after the
  11th, nothing else - 643 pipelines. Recipe pipelines are queued only while fewer than
  --gow2_renderer_recipe_pack_jobs (6) pipeline jobs are in flight. Shaders are parked until a started list needs them.
- Not yet: player settings other than the recording's (MSAA / upscaler / HDR change the recorded states),
  SteamOS.

## Other projects (29 Oct, read-only check)
santiquiroz's 7,662 skipped draws came from one short, deliberately cold scene (213 pipelines in ~12 s), not a whole
run; our chapter openings need 106-435 (median 260), so the work is the same size. We make fewer shader variants per
microcode hash than they do. Their real difference is ROV on NVIDIA (fewer pipeline states); not worth porting for this.

## Job 612c A/C test (29 Oct 10:36-, direct chapter starts, cold) [V]
Skipped draws A / C (two tries each): Gears 2 ch1 119,541 / 181,351 -> 92 / 120; ch5 89,457 / 94,066 -> 52,685 /
52,547; ch10 420,086 / 442,100 -> 120,516 / 192,283; ch17 105,724 / 86,130 -> 31,390 / 15,827; ch26 72,139 / 98,708
-> 4,307 / 4,296.
- What decides it is the time before control (Shagg): ch1 (long intro) and ch26 (cutscene) were built in time;
  ch5/10/17 start paused (the script's first A ~10 s) and drop straight into play - nearly all their remaining skips
  are in the 10-20 s after the unpause, 0 after (e.g. ch10 C1: 2,299 / 118,045 / 0 / 0 per 300 frames). At 6 jobs the
  pack had ~135-200 of ~700-800 pipelines after 10 s.
- ch10 C1 looked worse than A1 at 30 s (black surfaces, "Building shaders 99% (6 left)") with 0 skipped draws in that
  window - not understood yet; C built 708 pipelines vs A's 534 (a second list, ch28, and areas this route never
  reached). Hence job 613's pause rule and 5 s screenshots / frame CSV.
- These runs start chapters directly (no main menu, no real loading screen); a real play session is the next check.

## Job 613
- Recipe pack: up to 64 jobs while the frame is a loading screen / menu (under 64 draws) or already missing draws (32+
  skipped); 6 otherwise; queueing pauses after ~150 clean frames, never within 1,800 frames of a list starting (a
  cutscene plays clean and is build time).
- PR #43 (legoliamneeson), ported by hand: the reader's LZX decoder (prebuild_lzx.cpp; Gears 2's 47 files: same 28,053
  pixel / 8,747 vertex records as libmspack, failed chunks 3 -> 1, 4,675 blocks identical), gaps between chunks scanned,
  unreadable files flagged; resolve-texture reuse (with a stored scaled size and failed creations cached);
  background pipelines.list writer (+ error count); lowest priority for warm-only builds; FrameStep timing; strict
  occlusion queries as a launcher option (Real occlusion (experimental), default off, Gears 3 / Judgment, DirectX 12;
  known race: a report re-marked pending before its ZPD reaches the render thread could get the previous answer).
  Not taken: early stage restore (needs PR's warm stage store), compile-all-at-launch (planned as an opt-in later),
  the 0 -> 3 ms pipeline wait default.
