# COGBox achievements: known follow-ups

Small issues the final reviews of parts 2-4 found and left for later (none affects normal play). Collected 2026-11-05.

## Popups (part 2)

- Two unlocks that each complete the game, on two threads at the same moment, can queue two Platinum popups (a
  `platinum_shown_` flag under the queue lock would stop it).
- The popup style / colour / position cvars are set on the UI thread by the in-game menu while an unlock on a guest
  thread may read them; at worst one popup shows in the default style (a lock, or reading them on the UI thread).
- The PS5 popup runs off screen in windows narrower than about 440 px (clamp the scale by width too).
- The font atlas is large (about 32 MB with two sizes of two fonts); the Korean / Chinese glyphs could be merged into
  only the fonts that draw them.
- The PS5 Platinum title is blank for a title that is not one of the four games (fall back to "Platinum").
- The "Hide on-screen notes" tooltip says popups are separate, which is not true with an older exe.
- No automated test of the queue / Platinum ordering (the dialog needs the SDK).

## In-game browser (part 3)

- Sorted by name, a hidden secret sits at its real name's place, which hints at it (sort it as "Secret achievement").
- A failed icon load stays a "?" until the game closes.
- A corrupt, huge unlock time prints a far-future date (no upper bound check).
- F7 does nothing when the in-game menu is turned off (`cogbox_menu_enabled=false`).
- With the mouse on one row and the controller on another, the description line follows the last one drawn.
- A reveal could be written relative to the working folder if the COGBox folder can't be found at all.
- `WrapText` measures every prefix when it cuts the second line, every frame (cache the lines per row).
- A test constant is misnamed (`kNov4_2010Utc` is 2026-11-04 20:10).

## Launcher (part 4)

- In game, the F1 menu's Achievements tab is second (the menu orders tabs by their first option); in the launcher it
  is fifth. Moving the five option declarations after the Gameplay ones would match them.
- The tab's status line and the "Secret" tag do not go through `tr()` (they show in English).
- A failed icon load stays a "?" until the launcher restarts.
- `settings_test`'s achievements check covers Gears 2, Gears 3 and Judgment, not Gears 1.
