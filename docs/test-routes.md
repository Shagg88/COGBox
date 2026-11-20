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
| Gears 1 | chapter launch | none needed (does not pause) | job 1589 notes |

## Known facts, not yet a proven route
- Judgment: a film is skipped with the BACK button pressed TWICE (Shagg, 20 Nov). `SP_00_Museum_P` opens on a
  30 fps intro film. First scripted use: job 2145 (Back at 25 / 27 s and 45 / 47 s) - move to "Proven" once its
  screenshots show gameplay.
- Gears 2 `SP_Assault_P` / `SP_Hospital_P` with no pad: pause menu over the level - used ON PURPOSE as a still scene
  for like-for-like cost and picture comparisons; always label those numbers "paused scene".
- Gears 2 title screen: attract mode starts about 2 min 15 s after launch.
- Loading a given checkpoint slot needs the front end (Campaign > Continue / Load), not `?loadcheckpoint=1`.

## Not mapped (no scripted route exists)
Front-end menus of all four games (title > main menu > Campaign / Stats & Awards / Options ...). Mapping needs one
short run per game that presses one button at a time with a screenshot after each press - only with Shagg's OK.

## Mistakes this page exists to stop
- Job 1450, jobs 1671-1688, 1834 / 1836, 2143, 2153: measured the pause menu and reported it as gameplay.
- Never judge "reached gameplay" from a frame count or a frame time; use the state line and look at a screenshot.

## Every performance question: measure the whole frame in ONE job (Shagg, 20 Nov)
Never queue a job that answers one question. A job that has to run carries, beside its clean timing runs, a
diagnostics run of the same scene with everything on (its frame times do not count):
`--gow2_renderer_cpu_split=2 --gow2_renderer_gpu_by_target=true --gow2_frame_log=true --gow2_draw_stats=300`
`--gow2_resolve_log_frames=<two frames> --gow2_renderer_log_aliases=200 --gow2_renderer_draw_report_frame=<frame>`
`--gow2_renderer_draw_report_frames=2 --gow2_profile=30 --gow2_profile_start=3600 --gow2_profile_top=20`
`--gow2_hitch_report_ms=30 --gow2_texture_phase_log_ms=15 --gow2_ao_gpu_log=<N> (with AO on) --cogbox_test_state=true`
Read from it: CPU split per part, GPU time per category and per target, the ownership slow-path counters (slow
claims, tiles walked, tiles that changed owner, book changes), per-draw GPU time, copies per frame, alias pairs,
hitches, texture phases, the profile (build with symbols so it names our functions).
