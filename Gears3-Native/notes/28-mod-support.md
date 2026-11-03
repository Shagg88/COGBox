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

Built here from the repo (rebased on 0.4.1, d7962fb) with the SDK at nightly-20261002-bd833a2a +
`rexglue-sdk-full.diff` + patches 46-53 and the pins (LLVM 23.1.3, VS 2022 17.14 / MSVC 14.44), Title Update 6
(0.0.6.11), with the trilogy's Gears 1 chapter 1 mod:
- [V] mod.ini read, fingerprint logged; 28 mod files, 6 replaced / 28 added.
- [V] 2 config fragments merged into 3 bundles; chapter 32 -> GOW1_SP_Prison_P read from the merged config.
- [V] engine tables and `Default__GearTypes` found on the game thread during the boot (FName hook, every 256th call);
  `ChapterLevelNames[32]` = GOW1_SP_Prison_P through the game's FName constructor, table 31 -> 33 (retried until
  GearTypes' properties are linked, a few seconds after its default object exists).
- [V] content packages MPCH_COGDomG2MP_SF and MPCH_COGMarcusG2MP_SF requested right after the table is filled, opened,
  their objects rooted.
- [V] **the chapter loads and plays** with SDK patch 60 (`Gears2-Native/patches/sdk-patch60-overlay-added-files.diff`):
  chapter 32 from the title screen (`--ue3_exec_at="45s:open GearGame_P?game=geargamecontent.GearGameSP?chapter=32?listen"`,
  as Select Chapter builds it) opens GOW1_SP_Prison_P and its sections, carriers and characters; the opening runs,
  Marcus in his cell with Gears 1's WALK tutorial. Without the patch: a mod's *added* file was deleted from the overlay
  on its first open (`VirtualFileSystem::OpenFile` checked it at the game's folder / name), NO_SUCH_FILE, loading
  screen forever. Found with `--log_level=debug` (`[NtCreateFile] FAILED ... 0xc000000f`) after a stock map in the
  same chapter slot loaded fine.
- [V] content packages are kept before the first map change: scanned as soon as each is found, then every 30 frames
  until something is kept. A travel in the same tick as the request (the first engine tick, `--ue3_exec` at start) is
  too early: their classes were collected and the chapter's first spawn read a None class (guest 0xEC). Select Chapter
  is always later.
- [G] `--ue3_launch="GearGame_P?chapter=32"` at start: the boot URL reads ChapterLevelNames before chapters 32+ can be
  filled (also in KiltedKernel's runtime); start new chapters from the title screen / Select Chapter (or `ue3_exec`).
- [G] Select Chapter with the scripted pad: presses logged, the title screen did not react in a background window.

Debug switches added: `--gow3_mod_file_log` (every .xxx / .txt the game opens, chained to the shader prebuild's
file-open observer), `--gow3_mod_content_packages=false`.

## For the launcher (to work out with Shagg)

- A mod's chapters (32-49) in the chapter list, started with `GearGame_P?loadcheckpoint=0?chapter=N` (the table is
  filled once the title screen is up).
- mod.ini's extra keys shown (requires, game, side).
