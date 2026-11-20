# Unattended test routes (proven only)

Every job that starts a game takes its route from this page. A route not listed under "Proven" is a guess: do not
queue a gameplay test on it. Every gameplay run passes `--cogbox_test_state=true` and must find
`cogbox_test_state: GAMEPLAY` in its log (after the load) - otherwise the run measured a menu, a cutscene or a film
and its numbers are thrown away.

## Why runs end up on the pause menu
Gears 2 (and Gears 3 / Judgment) started straight into a chapter with no controller connected open on the PAUSE menu
(controller-disconnected pause). `--input_backend=none` alone gives no controller. A script pad
(`--gow2_input_script`, Gears 1 / 3: `--cogbox_input_script`) is a connected controller and can press Resume.

## Proven
| Game | Launch | Pad script | Proof |
|---|---|---|---|
| Gears 2 | `--gow2_launch="GearGame_P?loadcheckpoint=0?chapter=9 -nostartupmovies -FIXEDSEED"` | `30:A:0.3, 36:LUP:3, 40:LT:3, 41.5:RT:0.3, 44:RLEFT:1, 46:RUP:0.4, 47:RDOWN:0.8, 49:LUP:3` (route 1857; ONE A = Resume, a second A rolls) | jobs 1857, 2092 |
| Gears 3 | `SP_Coast_P`, no `--gow3_cheats` (keeps the game paused), `--cogbox_test_skip_movies`, ini `bSkipAllMatinees=TRUE` | A at 5 / 10 / 15 / 20 s, CancelCine at 30 / 35 / 40 s | job 1840 V3 |
| Gears 1 | (none proven: `SP_Embry_P` alone sits in its opening cutscene, job 2187) | see below | - |
| Judgment | `--ue3_launch="SP_00_Museum_P -nostartupmovies"` | `25:BACK:0.3, 27:BACK:0.3, 45:BACK:0.3, 47:BACK:0.3` (`--gow2_input_script`; BACK TWICE skips the intro film) | job 2145: gameplay (Old Town, "Meet up with the convoy") by frame 1200 at 60 fps, still there at frame 6000 |

## Known facts, not yet a proven route
- Gears 1: a cutscene is skipped with the X button (Shagg, 20 Nov). `SP_Embry_P` opens on a real-time cutscene that
  lasts longer than 115 s (job 2187: `cutscene yes` on every state line - it does not pause, but it is NOT gameplay).
  First scripted use: job 2194 (X at 30 / 36 / 42 s through `--cogbox_input_script`) - move to "Proven" once its state
  line says GAMEPLAY and a screenshot shows it.
- Judgment: a film is skipped with the BACK button pressed TWICE (Shagg, 20 Nov). `SP_00_Museum_P` opens on a
  30 fps intro film. First scripted use: job 2145 (Back at 25 / 27 s and 45 / 47 s) - move to "Proven" once its
  screenshots show gameplay.
- Gears 2 `SP_Assault_P` / `SP_Hospital_P` with no pad: pause menu over the level - used ON PURPOSE as a still scene
  for like-for-like cost and picture comparisons; always label those numbers "paused scene".
- Gears 3 and Judgment TITLE SCREEN (job 2174, 20 Nov: one frame shot a second from start-up, 3440x1440, test
  profile): both games leave "Press Start" BY THEMSELVES by about 27 s - no press is needed, and a START sent later
  lands on whatever came next. (Job 2145's "START does nothing" was this, not a loading problem.)
  - Judgment: title at ~20 s; by 27 s a first-run box "NEW TO GEARS OF WAR?" is up - A = "I'm new", B = "I know the
    basics". START and the D-pad do nothing on it. Press B (or A) at 30 s or later; the main menu behind it has not
    been mapped yet.
  - Gears 3: the MAIN MENU is up by 39 s. Items top to bottom: Campaign, Versus, Horde, Beast, Stats & Awards, Event
    Calendar, Exit Game (A accept, B back, START options). Stats & Awards and Event Calendar carry an orange "!"
    icon (meaning not confirmed). The highlight moved with no pad press sent (guess: the mouse pointer over the
    menu) - move the pointer out of the window before trusting D-pad counts.
- Gears 2 title screen: attract mode starts about 2 min 15 s after launch.
- Loading a given checkpoint slot needs the front end (Campaign > Continue / Load), not `?loadcheckpoint=1`.

## Not mapped (no scripted route exists)
Front-end menus of all four games (title > main menu > Campaign / Stats & Awards / Options ...). Mapping needs one
short run per game that presses one button at a time with a screenshot after each press - only with Shagg's OK.

## Mistakes this page exists to stop
- Job 1450, jobs 1671-1688, 1834 / 1836, 2143, 2153: measured the pause menu and reported it as gameplay.
- Never judge "reached gameplay" from a frame count or a frame time; use the state line and look at a screenshot.

## One launch, many settings (Shagg, 20 Nov: "3 hours is way too long. For anything.")
A launch costs ~2.5 minutes of which ~30 s is measured. Never queue one launch per setting when the setting can change
while the game runs: step through the settings inside ONE launch and cut the log into segments.
- XeGTAO: `--gow2_ao_test_script="<swap>:<key>=<value>,..."` (quality, denoise, algo, blur, show, half, single, mip,
  view); each step logs `gow2_ao_test:`; job 2169 is the template (segments of 3000 frames; still scene: one frame
  shot per setting).
- Not switchable yet (one launch each, so keep the list short): upscaler / DLSS model, SSIL, PCSS, resolution scale,
  renderer API. Make an option switchable before testing many values of it.
- A whole queue should fit in about 30 minutes. If it does not, cut the variations to the ones that would change a
  default, or make them switchable first.
- A speed test is made FROM job 2146's script (template minus diagnostics, `--gow2_renderer_queries=strict` re-added,
  speed switches), never re-typed (job 2157 dropped strict queries: twice the draws, invalid).
- Never put a new job in front of ones already queued; add it at the end.
