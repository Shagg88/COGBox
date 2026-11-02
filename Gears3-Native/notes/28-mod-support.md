# 28 - Mod support beyond replacing files (KiltedKernel)

Nov 1, 2026 · @KiltedKernel (with Claude)

What this branch adds to Gears 3's `--ue3_mods`, from KiltedKernel's Gears 3 port (github.com/KiltedKernel/
gears-of-war-3-pc-port), where the same rules run its mods folder and the trilogy (Gears 1 and 2 campaigns converted
into Gears 3 mods). All MIT. Nothing changes for a mod that only replaces or adds files.

## What it does

| Piece | Where | What |
|---|---|---|
| modcore | `src/layer2_ue3/modcore/` (MIT, standard C++ only) | mod.ini rules, load order, bare-file mapping, config fragment merge, merged TOC lines, MD5 / SHA-1, mod fingerprint. The same files as KiltedKernel's repo `modcore/`. |
| mod extras | `src/layer2_ue3/mod_extras.cpp`, `include/ue3/mod_extras.h` | `CollectMods` now: applies mod.ini rules (requires, game, enabled = false: left out with the reason logged); maps bare Xbox 360 files at a mod's top (packages, Coalesced bundles, texture caches, movies); skips `config\`. Then `<mod>\config\*.ini` fragments are merged, in load order, into every `Coalesced_*.bin` (the game's or a mod's copy) under `<user data>\mod-config`; they join the mods' files, so the existing overlay gives them TOC lines and the config edits (`--ue3_ini_edits`) edit them. Logs a co-op / System Link mod fingerprint. |
| objects | `src/layer2_ue3/objects.cpp`, `include/ue3/objects.h` | Reads the engine's objects in guest memory by name (GNames / GObjObjects from the engine's globals; classes, properties, instances). |
| mod chapters | `src/layer3_gow3/mod_chapters.cpp` | Chapters 32-49 a mod declares in config (`[Chapter32 GearCampaignChapterData] MapName=...`) get their map into `Default__GearTypes.ChapterLevelNames[N]`, the FName made by the game's own constructor (`FunctionDispatcher::ExecuteTrap` on the game thread). While such a chapter's map is loaded: its number kept in `CurrentChapter` / `ChapterNum`, checkpoint loads stream back the sections the map had at the save, `Trilogy*` Kismet object lists rooted. Content packages a mod names (`content = MPCH_COGMarcusG2MP_SF`) loaded with `UObject::LoadPackageAsync` and rooted. `--gow3_mod_chapters=false`: off. |
| addresses | `gow3_addresses.h` | GNames / GObjObjects globals, FName constructor, LoadPackageAsync, for the disc and TU6 (TU6 matched with gow3-recomp's `tools/map_tu6_addresses.py`; the globals through 12 / 10 matched readers each). |
| hooks | manifests, `game_patches.cpp` | `Gow3ModFName` at the FName constructor's entry (fills the chapter table on the game thread during the boot); `Gow3ModChaptersTick` from the existing `Gow3MaxTickRate` hook (content packages, checkpoint streaming). |

Fragment syntax (docs/MODDING.md in KiltedKernel's repo): `##### <ini file>` picks the packed file whose name ends with
it, then `[Section]` and `Key=v` (replace), `+Key=v` (add), `-Key=v` (remove), `*Key=v` (replace the value with the same
first field, e.g. a menu entry).

## Verified [V] / not yet [G]

Built here from the repo with the SDK at nightly-20261002-bd833a2a + `rexglue-sdk-full.diff` + patches 46-53 and the
pins (LLVM 23.1.3, VS 2022 17.14 / MSVC 14.44), TU6 (0.0.6.11), with the trilogy's Gears 1 chapter 1 mod:
- [V] mod.ini read (name, version), fingerprint logged; 28 mod files, 6 replaced / 28 added, 34 TOC lines per TOC.
- [V] 2 config fragments merged into 3 bundles; chapter 32 -> GOW1_SP_Prison_P read from the merged config.
- [V] engine tables found (names / objects), `Default__GearTypes` found, `ChapterLevelNames[32]` = GOW1_SP_Prison_P
  through the game's FName constructor, table 31 -> 33 entries.
- [V] content packages MPCH_COGDomG2MP_SF and MPCH_COGMarcusG2MP_SF loaded (LoadPackageAsync from the tick) and found.
- [G] starting the chapter. From Select Chapter it should work (the table is filled at the title screen); not run yet:
  the scripted pad (`--gow2_input_script`) presses were logged but the title screen did not react in a background
  window here. `--ue3_launch="GearGame_P?chapter=32"` at start races the boot: the table can be filled after the URL
  has looked its map up (then the game starts with no map); the FName hook narrows that but did not win every run.
- [G] the disc build (only TU6 built here).

## For the launcher (to work out with Shagg)

- A mod's chapters (32-49) in the chapter list, started with `GearGame_P?loadcheckpoint=0?chapter=N` (the table is
  filled once the title screen is up).
- mod.ini's extra keys shown (requires, game, side).
