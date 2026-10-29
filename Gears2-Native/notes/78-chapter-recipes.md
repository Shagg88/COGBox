# 78 - Chapter-opening "recipe packs" (shader pipelines prepared behind the loading screen)

Status: idea backed by measurements, not built yet. Order: after the open GitHub issues and the SteamOS / CrossOver
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
- Vertex shaders are patched by the XDK at bind, so their bytes in the files never match (job 367): prepare **pixel
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
