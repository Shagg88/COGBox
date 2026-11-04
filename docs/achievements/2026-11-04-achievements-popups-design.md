# COGBox achievements, part 2: the unlock popups

Date: 2026-11-04. Status: design approved in chat, awaiting spec review. Builds on part 1
(`2026-11-04-achievements-data-layer-design.md`).

## Goal

When an achievement unlocks, the game shows a popup in the style the player picked: **Xbox 360**, **modern Xbox**
(with colour presets and a "Rare" variant) or **PlayStation 5 trophy**. Popups animate like the real ones (stretch,
slide, pop, fade), play a replaceable sound, scale with the window, and never change what is recorded: GP is always
stored, whichever style shows.

## Not in this part

- The achievement browser (parts 3 and 4) and the launcher's Achievements page (part 4). Until part 4 the settings
  are changed in the F1 in-game menu (or `settings.ini`).
- Cloud rarity: "Rare" depends on the part 1 `RarityProvider`; with `NoRarity` it never shows.
- No Microsoft or Sony logo, font, artwork or sound is shipped. Every shape is our own drawing; fonts come from the
  player's PC or a bundled free font; the sound is our own placeholder.

## The three styles (approved as mockups in the brainstorm; kept locally, not in the repo)

| | Xbox 360 | Modern Xbox | PS5 trophy |
|---|---|---|---|
| Default position | bottom centre | bottom centre (centred; opens from the middle) | top right |
| Shape | flat grey (#4B4B4B) pill | pill in the chosen colour, round badge on the left | dark (#1F2023, slightly see-through) rounded box |
| Icon | our orb: dark ring cut in four by a cross, top-left quarter lit green, glossy grey sphere, symmetric green X of two curved bands; the X turns into the achievement's own icon (game data, clipped to the circle) | round badge: the achievement's own icon (clipped to the circle), which flips to a trophy (normal) / diamond with laurels (rare) | the achievement's own icon + a grade cup in gold / silver / bronze |
| Line 1 | "Achievement unlocked" | "Achievement unlocked!" / "Rare achievement unlocked" (bold) | grade cup + achievement name |
| Line 2 | "<GP>G - <name>" | white "G" coin, "<GP> - <name>" | "Trophy earned!" (grey) |
| Font | Segoe UI | Segoe UI, line 1 Semibold/Bold | Segoe UI |

Platinum (PS5 style only): when an unlock makes its game 100 % (every achievement in the game's list, DLC included),
a second popup follows it: a platinum cup and the game's name on line 1, "Platinum trophy earned!" on line 2.
It is a popup only: nothing is added to totals or GP.

### Colour presets (modern Xbox)

Dark `#24262B` (default, thin `#3C3F46` border), Red `#9E1B22`, Green `#107C10`, Blue `#0E5AA7`, Purple `#6B2FA3`,
Orange `#C25A0C`, Pink `#B4246E`, Teal `#0E7C7B`, Grey `#4B4F56`, Gold `#A07A12`. Text stays white; the badge is a
darker shade of the same colour.

### Animation (measured frame by frame from reference clips of the real popups; our own drawing)

Durations: Xbox 360 6.1 s, modern Xbox 7.3 s, PS5 5.7 s. Popups queue and show one after another.

- **Xbox 360:** a plain orb (no ring) fades and grows in (0-0.15 s); the bar slides out to the right from behind it
  with a small overshoot (0.15-0.6 s); the ring and the text appear (0.35 s); the X dims and the achievement icon
  fades in (0.75-0.95 s). Out, reversed: icon back to the X (5.2-5.5 s), ring off, text off, the orb shrinks to a dot
  (5.85-5.95 s), the empty bar fades (5.95-6.1 s). No achievement icon: the X stays.
- **Modern Xbox:** a small hollow ring grows (0-0.35 s) and fills (0.35-0.45 s); the achievement icon grows from a
  dot inside it (0.5-0.75 s); the badge flips like a coin to the trophy / diamond (1.2-1.4 s) while the pill opens
  outward from the centre (1.2-1.55 s); the text fades in (1.55-1.75 s). Page 1 (title, "G <GP> - name") until
  4.2 s, cross-fade to page 2 (the achievement's description) by 4.4 s. Out: text off (6.1 s), the pill closes to
  the centre (6.25-6.5 s), flips back (6.5-6.7 s), the icon shrinks (6.7-6.85 s), the ring empties and shrinks
  (6.85-7.3 s). The real one's "Hold Guide to open" page and console logo are not copied. Rare title: "Rare
  achievement unlocked - <percent>%". No achievement icon: the trophy shows from the start, no flip.
- **PS5:** fades in with a short 10 px slide from the right (0-0.15 s), holds, fades out with the same slide
  (5.5-5.7 s).

The sound starts with the intro.

## Trophy grade from GP (PS5 style)

Gears of War: Reloaded (PS5) trophy grades matched by name to Gears 1's Xbox 360 GP (from the game's own data, 25
achievements in both): 10 GP all Bronze; 20 and 30 GP Bronze except Soldier (Hardcore campaign) and Commando (Insane
campaign), which are Gold; 40 and 50 GP Silver. Reloaded: 1 Platinum, 2 Gold, 4 Silver, 50 Bronze
(source: PowerPyx trophy guide).

Rule:
- GP <= 30: Bronze; 31-74: Silver; >= 75: Gold.
- Exceptions (Reloaded's grade): Gears 1 (`4D5307D5`) achievement 13 (Soldier) and 19 (Commando): Gold. Stored as IDs
  only.

Result: Gears 1 2 Gold / 2 Silver; Gears 2 4 Gold / 6 Silver; Gears 3 5 Gold / 11 Silver; the rest Bronze.

## Rare (modern Xbox)

Rare when the rarity provider knows the achievement and fewer than 10 % of players have it (`known && percent <
10`). With `NoRarity` never.

## Settings

All four are global (`settings.ini [General]`), passed by the launcher to whichever game starts, shown in the F1 menu
(changes there are written back to `[General]`):

| Setting | `[General]` key | Values (first = default) | Game option |
|---|---|---|---|
| Achievement popups | `AchievementStyle` | Xbox 360, Xbox, PlayStation | `--cogbox_ach_style=x360\|xbox\|ps5` |
| Popup colour | `AchievementColour` | the 10 presets above | `--cogbox_ach_colour=<name>` |
| Popup position | `AchievementPosition` | Default, Top left, Top centre, Top right, Bottom left, Bottom centre, Bottom right | `--cogbox_ach_position=<name>` |
| Achievement sound | `AchievementSound` | On, Off | `--cogbox_ach_sound=true\|false` |
| Hide achievement popups | `HideAchievementPopups` | Off, On | `--cogbox_ach_hide=true\|false` |

"Hide achievement popups" on: no popup and no sound; achievements still unlock and save. The existing per-game "Hide
on-screen notes" then covers only the port's own notes (shader building / loading); its description loses the
achievement line, and the games' `CreateAchievementNotificationDialog` checks the new setting instead of
`XenosOnScreenNotesHidden()`.

Unknown or damaged values fall back to the defaults.

## Sound

`COGBox\assets\sounds\achievement.wav`, or `achievement-x360.wav` / `achievement-xbox.wav` / `achievement-ps5.wav`
when present (the style's own wins). Played once per popup at the intro with Windows' `PlaySound` (asynchronous).
The shipped file is a short chime we generate ourselves, as a placeholder: replacing the file changes the sound, no
rebuild. Missing or unplayable: silent, one log line.

## Design

### Pure logic (part 1's `achievements_data`, standard C++17, unit-tested)

- `TrophyGrade GradeFor(uint32_t title_id, uint32_t achievement_id, uint32_t gamerscore)` - Bronze / Silver / Gold.
- `bool IsRare(const Rarity& r)`.
- `bool CompletesGame(const std::vector<std::pair<uint32_t, bool>>& unlocked_by_id, uint32_t just_unlocked)` - true
  when every other achievement is unlocked and this one was the last.
- `PopupFrame PopupTimeline(PopupStyle style, float seconds)` - the state of each moving part at that time: bar
  width (0-1), slide offset (0-1), scale, text and overall alpha, finished. The styles draw only from these numbers.
- Settings parsing: style / colour / position names to values, unknown -> default.
- `std::string FitText(const std::string& text, float max_width, const std::function<float(const std::string&)>& measure)`
  - shrinks once to 90 %, then cuts with "…" (UTF-8 safe).

### Drawing (Gears 2 `layer0_xenos`, built into all four games)

- `AchievementPopup` replaces `LazyAchievementToast`: same "attached to the ImGui drawer only while a popup shows"
  behaviour and `WantsContinuousPaint() == false` (drawn with the game's frames), a queue, the sound.
- `popup_x360.cpp`, `popup_xbox.cpp`, `popup_ps5.cpp`: each draws one frame from `PopupFrame` with ImGui draw lists
  (rounded rects, circles, paths). Icons from the SDK's achievement icon cache.
- Size: designed for 1080p, scaled by window height / 1080, clamped to 0.6-2.0; the screen-edge margin scales too.
- Fonts in each game's `OnConfigureFonts`: Segoe UI and Segoe UI Semibold from `C:\Windows\Fonts`, merged with Malgun
  Gothic (Korean) and Microsoft JhengHei / YaHei (Chinese) when present; when Segoe UI is missing (Proton), the bundled
  Noto Sans (OFL, in `assets/fonts`). A character in no loaded font shows as a box, never as nothing.

### Data flow

1. The game unlocks an achievement; the SDK saves it and raises its notification (unchanged).
2. `AchievementPopup::Push` reads the settings at that moment and queues one entry: style, colour, position, name, GP,
   grade, rare, completes-game (from the SDK's list and unlock states). PS5 + completes-game queues the Platinum entry
   behind it.
3. Popups draw one at a time through `PopupTimeline`; the sound plays when each intro starts.

### When things go wrong

- No icon: Xbox shows its badge (needs no game icon); PS5 shows a plain dark square; 360 never uses the game icon.
- Long names: `FitText`, never overflowing the popup.
- Settings: unknown values -> defaults (Xbox 360, Dark, Default, sound on, not hidden).
- Sound: missing or unplayable -> silent, one log line.

## Testing

Unit tests (`gn_tests`, `[ach]`):
- `GradeFor`: 10/20/30 -> Bronze; 40/50/74 -> Silver; 75/100/150 -> Gold; Gears 1 13 and 19 -> Gold; ids 13 / 19 of
  another title -> by GP.
- `IsRare`: `NoRarity` false; 9.9 % true; exactly 10 % and 40 % false.
- `CompletesGame`: last locked one -> true; already unlocked, unknown id, or another still locked -> false.
- `PopupTimeline`, each style at 0, mid-intro, hold, mid-outro, end: values in range; alpha starts and ends at 0;
  finished after the end.
- Settings parsing: every preset name, every position, unknown -> default.
- `FitText` with a fake measure function: fits unchanged; shrinks; cuts with "…" on a UTF-8 boundary.

In the visual companion: the three animations rebuilt from the same timeline numbers, tuned against reference clips
(if the player has them) before they go into the games.

In the games (the player runs them): a test build with a debug key (off by default, on only in the test `.bat`) that
shows a sample popup; each style, colour and position at 1080p and another size; the Platinum sample; "Hide
achievement popups" shows nothing and plays nothing.
