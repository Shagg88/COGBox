# COGBox achievements, part 4: the launcher's Achievements tab

Date: 2026-11-04. Status: design approved in chat, awaiting spec review. Builds on part 1
(`2026-11-04-achievements-data-layer-design.md`), part 2 (`2026-11-04-achievements-popups-design.md`) and part 3
(`2026-11-04-achievements-browser-design.md`).

## Goal

The COGBox launcher shows the same achievements as the in-game browser - each game, the other games and All, totals,
sorting, the Show filter and the secret reveal - and holds the popup settings, **in the launcher's existing design**
(Shagg's): his tabs, headings, setting rows, buttons, lamps, message box, fonts and colours. Nothing of the launcher's
look changes beyond one new tab and one new front-screen button.

## Not in this part

- Live game data: the launcher never runs a game itself, so it reads the cached files only (part 1).
- Saving Sort / Show between launcher runs: kept while the launcher is open.
- Cloud rarity (as in parts 2-3: the space is there, `NoRarity` fills nothing).
- Popup previews: popups draw only in game.

## Where it is (approved as a mockup over the player's own 0.4.2 screenshots)

- **Each game's page** gets an **ACHIEVEMENTS** tab between GAMEPLAY and ABOUT (DISPLAY, GRAPHICS, CONTROLS, GAMEPLAY,
  ACHIEVEMENTS, ABOUT), drawn with the existing tab style.
- **Front screen:** an **ACHIEVEMENTS** button joins PLAY and SETTINGS under the box, the same button style, the three
  centred together. It opens that game's page on the ACHIEVEMENTS tab with **All** chosen.

## The ACHIEVEMENTS tab

Inside the page's existing scrolling rows area, top to bottom:

1. Heading **ACHIEVEMENT POPUPS** (the launcher's heading: red bar, capitals, line under it, as on ABOUT), then the
   five popup settings as ordinary setting rows (the existing option row code: label left, control right, tooltip),
   labelled: Popup style, Popup colour (Xbox style), Popup position, Popup sound, Hide achievement popups.
   - They are the part 2 options, moved from the Display tab: same ids (`AchievementStyle`, `AchievementColour`,
     `AchievementPosition`, `AchievementSound`, `HideAchievementPopups`), Shared scope, same `settings.ini [General]`
     keys, same choices and defaults, same `--cogbox_ach_*` game arguments and capability check. Saved with Save.
2. Heading **ACHIEVEMENTS**, then:
   - **Game buttons** (the launcher's small button style, as "Replace your copy..." on ABOUT): this page's game first,
     the others in series order, then All - part 3's `BuildSidebar` order with the page's game as the running title.
     Labels: Gears of War, Gears of War 2, Gears of War 3, Judgment, All. The chosen one uses the red button style; a
     game that is not loaded is drawn disabled-looking but can be chosen (its message shows). The page's game is chosen
     when the tab opens (All when opened from the front button).
   - **Status line** with the launcher's lamp: `"<u> / <t> achievements | <e> / <g> G | <p>%"`; lamp green when at
     least one is unlocked, grey otherwise (none yet, or not played).
   - **Sort** and **Show** as ordinary setting-style rows (combos with part 3's names and order).
   - **The list**: one row per achievement from part 3's `BuildRows`: icon (greyed when locked, a "?" box for a hidden
     secret), name, text (dim), GP and status right-aligned, the game label in All, the "Secret" tag, the rarity text
     when known. Texts cut with "..." to one line (part 2's `FitText`); the launcher's fonts (they include the Chinese,
     Japanese and Korean fonts already). The list's empty messages are part 3's `EmptyMessage`.
   - Choosing a hidden secret (mouse or A) asks through the launcher's own message box (`ask()`): title
     "Reveal this secret achievement?", text "Its name, description and icon will be shown in every game and in the
     launcher.", buttons **No, keep it secret** (the default) and **Yes, reveal it**. Yes: part 1's `AddRevealed`, then
     the data is read again. A failed save shows the launcher's red message line: "Couldn't save - is the COGBox folder
     read-only?"

## Controls

The launcher's own scheme, nothing new: LB / RB change tabs; D-pad / stick move through the rows, game buttons, combos
and list; A presses or opens; B goes to the bottom bar (as on the other tabs). No X / Y shortcuts on this tab. On the
front screen LB / RB step through PLAY, SETTINGS and ACHIEVEMENTS (today two choices), A presses the chosen one.

## Data

- The COGBox folder is the launcher's own folder (the one it passes as `--cogbox_dir`).
- Part 1's `LoadAllGames` is read when the tab is opened (each time it becomes the shown tab, or the page opens on it)
  and again when a game the launcher started has closed. Sort / Show / the chosen game are kept while the launcher
  runs.
- Icons: the cached PNGs (part 1's `Achievement::icon`), decoded with the launcher's `stb_image` into SDL textures
  (as its box art), loaded only for rows drawn, kept until the launcher closes; a failed load draws the "?" box.
- Times: the PC's local time, as in game (`BuildRows`' offset function from `localtime_s` / `_mkgmtime`).

## Code

1. **Shared, unchanged:** `achievements_data.cpp` (already in `build.sh`), `achievements_browser_model.cpp` and
   `achievements_popup_logic.cpp` (both added to `build.sh`'s source list). Standard C++17, already unit-tested.
2. **`cogbox/src/gears_settings.{h,cpp}`:** `enum class Tab` gains `Achievements` (after `Gameplay`); the five options'
   tab changes from `Tab::Display` to `Tab::Achievements`.
3. **`cogbox/src/page.cpp`:** `k_tabs` gains "ACHIEVEMENTS" before "ABOUT" (`k_tabCount` 6). The option-row loop already
   draws the tab's options; on the Achievements tab `achievements_tab(a, g)` draws below them in the same rows child.
   The tab becoming shown, and a game closing, mark the tab's data for reloading.
4. **`cogbox/src/achievements_tab.cpp`** (new; declared in `app.h`): headings, game buttons, status line, Sort / Show,
   the list with icons, the reveal question. Uses only the launcher's theme functions (`th_heading`, `th_button`,
   `th_button_red`, `th_lamp`), fonts (`F_BODY`, `F_SMALL`, `F_BOLD`) and colours (`col::`).
5. **`cogbox/src/front.cpp`:** the third button and the three-way LB / RB choice; it opens the page on the
   ACHIEVEMENTS tab with All chosen.

## Building and testing

- Unit tests: part 3's `gn_tests [ach]` (unchanged logic); the launcher's `settings_test` (built natively with clang,
  as in part 2) checks the five options are on `Tab::Achievements` with unchanged ids, defaults and arguments.
- A launcher build on this PC: a Windows MinGW compiler (llvm-mingw) and SDL3's MinGW development files, installed
  outside the repo (each download confirmed by the player first, with its name, source and size), and `build.sh` run
  with `COGBOX_NO_FFMPEG=1` (no menu music - a test build only, never shipped).
- The player's check: the test launcher as `CogBox-achievements-test.exe` beside their own `CogBox.exe` (untouched),
  with `settings.ini` and `CogBox.ini` backed up first. On two games' pages: the popup settings change and save and
  are no longer on Display; every game button and All; every Sort and Show; a secret reveal (No, then Yes) and the
  same secret revealed in game; the front button with the mouse and with LB / RB; a game played from the test launcher
  shows its new unlocks after it closes; one window resize.
