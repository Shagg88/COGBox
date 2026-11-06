# COGBox achievements, part 3: the in-game browser

Date: 2026-11-04. Status: design approved in chat, awaiting spec review. Builds on part 1
(`2026-11-04-achievements-data-layer-design.md`) and sits beside part 2 (`2026-11-04-achievements-popups-design.md`).

## Goal

While playing, the player can browse the achievements of the game they are in, of the other three games and of
"All" played games together: what is unlocked and when, what is left and how to get it, per-game and overall totals,
sorted and filtered the way they want. Secret achievements stay hidden until the player chooses to reveal one.

## Not in this part

- The launcher's Achievements page (part 4). It will reuse this part's browser logic and layout.
- Cloud rarity: rows have room for "Rare · 4.2%", but with part 1's `NoRarity` it never shows and "Rarity" sorts
  like "Unlock time".
- Saving the browser's choices (game, sort, Show) between runs: they last until the game closes.

## Where it opens

The browser is a page of COGBox's in-game menu (`ingame_menu.cpp`), so the controller, the mouse and the input
isolation (the game gets no input while the menu is open) are the menu's existing code.

- **F1 / Back + Start** opens the menu on the settings page as today. A new **Achievements** button sits with Resume,
  Restart to apply and Quit game.
- **F7** opens the menu straight on the Achievements page; F7 again closes the menu. The SDK's own basic F7
  achievements window is no longer made: each game app's `CreateAchievementsOverlay` returns nothing, and the menu
  owns F7.
- The game keeps running underneath, as with the menu today.
- A game started by hand (no `--cogbox_menu`) still has the Achievements button beside Resume and Quit game.

## Layout (approved as mockup "B" in the brainstorm)

The menu's plate, colours and crimson highlight. Left column, right list, buttons at the bottom.

**Left column**, top to bottom: the running game, tagged "This game"; the other games in series order (Gears of War,
Gears of War 2, Gears of War 3, Gears of War: Judgment, skipping the running one); then **All**.
- Each entry: name, progress bar, "<unlocked> / <total> · <earned> / <total> G".
- A game with no catalog ("not loaded" in part 1): "Not played yet". Selecting it shows "Start <game name> once from
  COGBox to load its achievements." in the list area.
- The page opens on the running game.

**Header** (right side): the selection's name ("All played games" for All); "<unlocked> / <total> achievements ·
<earned> / <total> G · <percent>%"; a progress bar; the **Sort** and **Show** boxes.
- Sort: Unlock time (default), Rarity, Game order, Gamerscore, Name (A-Z).
- Show: All (default), Unlocked, Locked.
- Percent is part 1's `PercentByGp`; All's numbers are part 1's `SumTotals` (loaded games only).

**Rows:**
- Icon (52 px at 1080p), name, the text on up to two lines, GP on the right, and "Unlocked <d Mon yyyy, HH:MM>" in the PC's local
  time or "Locked" under it. An unlock with no known time shows "Unlocked".
- Unlocked: the description. Locked: greyed icon and the game's "unachieved" text (how to unlock).
- In All: a small game label after the name ("Gears 1", "Gears 2", "Gears 3", "Judgment").
- A secret, locked and not revealed: a "?" icon, "Secret achievement", "Select to reveal"; the GP still shows.
- A revealed secret, or an unlocked one: shown normally, with a small "Secret" tag.
- When the rarity is known: "Rare · <percent>%" (part 2's `IsRare`) on the right; otherwise nothing.
- The name on one line (cut with "…", part 2's `FitText`); the text wrapped to two lines, the second cut with "…" if
  needed (`WrapText`), with a small gap under the name. The selected row's full name and text show in the menu's
  description line at the bottom.
- Empty results: "No unlocked achievements yet" (Show: Unlocked), "Every achievement is unlocked!" (Show: Locked).

**Reveal question**, over the page when a hidden secret is selected: "Reveal this secret achievement?", "Its name,
description and icon will be shown in every game and in the launcher.", buttons **Yes, reveal it** and **No, keep it
secret** (No has the focus). Yes adds it to `revealed.txt` and the list reloads.

**Buttons** at the bottom: **Back to settings** and **Resume** (crimson). Back always goes to the settings page, also
when F7 opened the menu.

## Controls

| Controller (PS prompts) | Does |
|---|---|
| LB / RB (L1 / R1) | previous / next entry in the left column |
| D-pad / left stick | move through the list (scrolls) |
| A (Cross) | on a hidden secret: the reveal question |
| X (Square) | next sort order |
| Y (Triangle) | next Show filter (All → Unlocked → Locked → All) |
| B (Circle) | back to the settings page; in the reveal question, No |

The hint line at the top reads "LB / RB: game   A: select   X: sort   Y: show   B: back" (PS names with the PS
prompts setting). Mouse: click a game, wheel to scroll, click a secret, Sort and Show are drop-downs. Esc works like
B. The chosen game, sort and Show are kept while the game runs.

## Data

- On each opening of the Achievements page: part 1's `LoadAllGames(XenosCogboxDir(), CurrentRarityProvider())`,
  which reads every catalog, unlock file, `revealed.txt` and icon path fresh.
- An unlock while the page is open (the SDK's unlock callback) marks the data stale; the next frame reloads it.
- The running game: if its catalog is missing (it could not be written), it is built from the SDK's
  `ListAchievements` / `IsUnlocked` instead, so "This game" is never empty.
- Icons: the running game's through the SDK's `AchievementIconCache`; the other games' from part 1's cached PNGs,
  decoded with the SDK's image decoder into `ImmediateDrawer` textures. Loaded only for rows on screen, kept until the
  game closes.
- Reveal: part 1's `AddRevealed`; then reload.
- Fonts: rows use part 2's achievement fonts (`XenosAchievementFontFor`: Inter, the 22 px raster for small text), so
  the catalogs' languages draw.

## Code

1. **Part 1's sorting** (`achievements_data`): `SortOrder` gains `kGameOrder` (catalog order), `kGamerscore` (highest
   first, then game order) and `kName` (A-Z ignoring case and accents: Latin letters with accents (Latin-1 and Latin
   Extended-A) compare as their base letter, so "Élite" sorts with E; other scripts by code point; ties in game
   order). Unit-tested.
2. **Browser logic** (new, `achievements_browser_model.{h,cpp}`, C++17, no SDK, no ImGui): the left column entries,
   the header numbers, the filtered and sorted rows for a selection, each row's display text (date formatting, the
   locked text, the secret placeholder and tag, the game label), the empty-result message, and the Sort / Show
   cycling. Fully unit-tested. Part 4 reuses it.
3. **Browser drawing** (new, `achievements_browser.{h,cpp}`): draws the page inside the menu window from the model,
   loads icons, asks the reveal question. Only visible rows are drawn (`ImGuiListClipper`).
4. **In-game menu** (`ingame_menu.cpp`): the Achievements button, the page switch, F7, and the Achievements page's
   button mapping. The four game apps' `CreateAchievementsOverlay` returns nothing.

All in Gears 2's `layer0_xenos`, built into all four games.

## When things go wrong

- Missing or damaged files: part 1 already reports the game as not loaded or skips bad lines; nothing stops.
- `revealed.txt` cannot be written: the secret stays hidden, the reveal question shows "Couldn't save - is the COGBox
  folder read-only?", and the log gets a warning.
- No COGBox folder found at all: only "This game", from the SDK's data; the others show "Not played yet".

## Testing

- Unit tests in `gn_tests [ach]`: the three new sort orders; the left column order and "This game"; totals; the Show
  filter; row text (date, locked text, secret placeholder, "Secret" tag after a reveal, game label in All);
  empty-result messages; Sort / Show cycling.
- In game, by the player: F1 → Achievements and F7 in two games; controller and mouse; every sort and Show; All; a
  secret reveal with No then Yes (Gears 3 "Respect for the Dead", id 19 - back up `revealed.txt` first if it should
  stay secret); a real unlock while the page is open appears without reopening.

## Changed after the in-game checks (2026-11-05)

- **Runtime:** the menu is installed before the SDK makes the runtime (`OnCreateDialogs` comes before
  `ConstructRuntime`), so the page looks the runtime up when needed and attaches the running title, its icon cache and
  the unlock callback once the game's module is loaded (retried on every reload). Before that fix the page had no
  "This game".
- **Look:** the left column's hover draws the full red border (ImGui's own highlight is off); the page is drawn
  1.15 x; the second lines are one step bigger (descriptions 16, status 15, totals 15, header 17 px at 1080p); rows
  are 82 px for the two-line text.
