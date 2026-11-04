# COGBox achievements, part 1: the achievement data layer

Date: 2026-11-04. Status: design approved in chat, awaiting spec review.

## The project and where this part fits

COGBox is getting a global achievement system for its four games: Xbox 360 / modern Xbox / PlayStation-style unlock
popups, an achievement browser in game and in the launcher, per-game and all-games totals, and a rarity system that
is empty for now but can later be backed by a cloud tracker. It is built in four parts, each with its own spec, plan
and build:

1. **Achievement data layer (this spec):** a cache of each game's achievement list and icons on the player's PC,
   reading every game's unlocks, totals, sorting, the "revealed secret" list and the rarity interface.
2. In-game popups: the three styles, the colour settings, the Xbox GP to PlayStation trophy grade table.
3. In-game browser: this game first, the other games and "All", sort by unlock time or rarity, totals.
4. Launcher page: the same browser in the COGBox launcher.

Parts 2 and 3 need only part 1. Part 4 needs part 1 and reuses part 3's layout. The whole project ships as one
PR to the COGBox repo when all four parts are done.

## Goals

- Every game's achievement names, descriptions, gamerpoints (GP) and icons come from that game's own data (the
  title's XDBF in `default.xex`), never from the COGBox repo. They are cached on the player's PC.
- Any of the four games and the launcher can show any game's achievements and unlocks, and totals for one game and
  for all games.
- A game that was played and then removed (its game files gone, its `user` folder kept) still shows, with its
  unlocks and totals.
- Rarity goes through one interface, so a cloud tracker can be added later without changing the popups or browsers.

## Not in this part

- Popups, the in-game browser, the launcher page (parts 2-4). The "reveal this secret achievement?" question is
  part 3 / 4 UI; this part only stores which secrets were revealed.
- Any cloud code: only the interface and the `NoRarity` placeholder.
- How achievements unlock: unchanged (the game's own calls, the SDK's AchievementManager).

## What exists today (ReXGlue SDK)

- `KernelState::LoadAchievementsData` reads the running title's XDBF into `AchievementInfo` (id, label,
  description, unachieved_description, image_id, gamerscore, flags) and loads unlocks from
  `<user_data_root>\achievements\<TitleID>.toml` (`[unlocked."<id>"] filetime = <FILETIME>`; an older array form
  is migrated). `AchievementManager` keeps the list, the unlock times, unlock / notification callbacks, and saves the
  TOML on each unlock.
- Each game has its own user folder, `COGBox\<Game>\user`, so nothing today sees all four games' unlocks. The
  launcher (`Gears2-Native/cogbox`, C++ / Dear ImGui, built by `cogbox/build.sh`, libraries: imgui, miniz,
  stb_image) cannot read achievements: they are inside the encrypted `default.xex`. The launcher has no uninstall;
  a game's `user` folder stays when its game files go.

## Design

### The four games

A fixed table in the shared code: folder, title ID, name.

| Game | Folder | Title ID |
|---|---|---|
| Gears of War | `Gears1` | `4D5307D5` |
| Gears of War 2 | `Gears2` | `4D53082D` |
| Gears of War 3 | `Gears3` | `4D5308AB` |
| Gears of War: Judgment | `Judgment` | `4D530A26` |

### Files on the player's PC

| File | Written by | Read by |
|---|---|---|
| `COGBox\achievements\<TitleID>\catalog.txt` | the game, every start | everyone |
| `COGBox\achievements\<TitleID>\icons\<id>.png` | the game, when missing | everyone |
| `COGBox\<Game>\user\achievements\<TitleID>.toml` | the SDK, on unlock (unchanged) | everyone; COGBox never writes it |
| `COGBox\achievements\revealed.txt` | game or launcher, when a secret is revealed | everyone |

The unlock file stays the one record of what is unlocked: it is never copied, so the launcher's "Back up saves" and
a restore keep working, and nothing can drift.

### How a game finds the COGBox folder

The launcher passes `--cogbox_dir="<COGBox folder>"` to game exes that support it (its existing `exe_has("...")`
capability check, as for `--cogbox_menu`). Without it (a game started by hand) the game writes no cache, and its
browser shows only the running game from the SDK's own data.

### Code

1. **Achievement data reader** (`Gears2-Native/src/layer0_xenos/`, shared, no SDK or ImGui dependency, so the
   launcher builds the same `.cpp` from `cogbox/build.sh`). It reads one catalog and one unlock file, joins them into
   a per-game list, works out per-game and "All" totals, sorts, reads and writes `revealed.txt`, and defines the
   rarity interface with its `NoRarity` placeholder.
2. **Catalog writer** (shared, called from each game app's `OnPreLaunchModule`, once the title's list is loaded):
   writes `catalog.txt` from `AchievementManager::ListAchievements()` and the icons from the title's XDBF images.
3. **Users** (parts 2-4) talk only to the reader and the rarity interface.

Gears 1, Gears 3 and Judgment already build Gears 2's `layer0_xenos` files, so one copy serves all four games.

### `catalog.txt`

UTF-8, `key=value` lines, so the launcher reads it without a library. In values, a line break is written `\n` and a
backslash `\\`.

```
# COGBox achievement catalog 1
title=4D53082D
game=Gears of War 2
language=1
[achievement]
id=1
gamerscore=10
flags=...
image=...
label=...
description=...
unachieved=...
[achievement]
...
```

- Written to a temporary file, then renamed over the old one: a reader never sees half a file.
- Rewritten in full on every start (a language change or a title update shows). Icons are written only when
  `icons\<id>.png` is missing.
- Every achievement in the game's list is included (title update and DLC ones too), so totals match the game's own.
- Unknown keys are ignored (later versions can add some); the `1` in the first line is the format version.

### Secret achievements

An achievement is secret when its XDBF flags do not have the "show unachieved" bit (0x8, as Xenia names it;
confirmed on Gears 3, below). Until it is unlocked, browsers
show "Secret achievement" and hide its name, description and icon. The player can reveal one (parts 3 and 4 ask
"Do you want to reveal this secret achievement?"; yes reveals it, no leaves it secret). A reveal is saved in
`revealed.txt`, one `<TitleID>:<id>` per line, so it stays revealed in every game and the launcher. Revealing never
unlocks anything; an unlocked secret is always shown.

The game's own data decides what is secret; COGBox keeps no list of its own. Checked 2026-11-04 on Gears 2 TU6: all
79 achievements (DLC included) have the 0x8 bit, so none is secret - including the seven map-pack achievements that
achievement sites list as secret (they were hidden before the map packs shipped; the console took that from the DLC or
the online service, not the title data). Confirmed on Gears 3 TU6: of its 72 achievements only "Respect for the Dead" (id 19, flags 0x1, unachieved text
"Secret.") lacks 0x8 - the one secret achievement of the base game.

### What the reader gives

- Per achievement: id, label, description, unachieved description, GP, secret, revealed, icon path, unlocked, unlock
  time (FILETIME, UTC; shown in local time), rarity.
- Per game: loaded or not ("Play once to load achievements"), unlocked / total count, earned / total GP, and a
  percentage by GP (earned GP / total GP, as the Xbox dashboard shows). A game with 0 total GP shows 0 %.
- "All": the same numbers summed over the games that have a catalog. An installed game that was never run since this
  feature shipped is listed as not loaded and adds nothing; a played game whose game files are gone keeps counting.

### Rarity

```cpp
struct Rarity { bool known = false; float percent = 0; };  // percent of players who have it
class RarityProvider {
 public:
  virtual ~RarityProvider() = default;
  virtual Rarity Get(uint32_t title_id, uint32_t achievement_id) = 0;
  virtual void ReportUnlock(uint32_t title_id, uint32_t achievement_id, uint64_t filetime) {}  // future cloud upload
};
```

`NoRarity` returns "unknown" for everything and ignores reports. The game calls `ReportUnlock` from the SDK's unlock
callback. Part 2 uses rarity for the Xbox "Rare achievement unlocked" style, so with `NoRarity` that style never
shows yet.

### Sorting

- By unlock time: unlocked achievements, newest first, then locked ones in the game's list order.
- By rarity: achievements with a known rarity first, rarest first; then the rest in the unlock-time order. With
  `NoRarity` this is the same as sorting by unlock time.

### Data flow

1. The game starts: the SDK loads the list and unlocks; the catalog writer writes the catalog and missing icons.
2. An achievement unlocks: the SDK saves the TOML and raises its callbacks: the popup (part 2) shows, and the
   rarity provider gets `ReportUnlock`.
3. A browser (in game or launcher) reads all the files fresh each time it opens; they are small.

### Errors

- A missing or damaged catalog: that game is "not loaded". A damaged unlock file: no unlocks for that game, plus a
  warning in the log. A bad line is skipped. Nothing stops the game or the launcher.
- The writer cannot write (read-only folder, disk full): a warning in the log, the game carries on.

## Testing

Catch2 tests in `gn_tests` (as the Exit Game tests):

- Catalog: write then read gives the same data, including escaped line breaks, Unicode (Russian and Korean names)
  and an empty description; unknown keys are ignored.
- Unlock file: the SDK's current format, the old array format, a damaged file (no crash, no unlocks).
- Join: an unlock without a matching achievement is ignored; an achievement without an unlock is locked.
- Totals: per game (count, GP, GP-based %); "All" counts only games with a catalog; 0 GP does not divide by zero.
- Sorting: newest unlock first, locked last; `NoRarity` gives the time order; a test provider with known values puts
  the rarest first.
- Secrets: the flag from XDBF flags; `revealed.txt` written and read back; an unknown entry is ignored.

In each game: after one start, the catalog and icons exist; unlocking one (by playing, or a test build that unlocks
one at start; the SDK has no debug unlock) updates the TOML, and the reader sees it.

## Changed after the later parts (2026-11-05)

- The launcher passes `--cogbox_dir` when the exe contains "COGBox achievement popups" (part 2's popup setting
  description), not the folder setting's own description: every exe with the popups has the folder setting, and the
  launcher searches an 82 MB exe once for both (about 0.3 s per text searched).
