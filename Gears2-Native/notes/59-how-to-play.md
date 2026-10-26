# 59. How to play - the switches

Start the game with `Launcher.bat` (a small window: render resolution, frame-rate limit, SMAA, 16x anisotropic,
ultrawide, HDR, borderless fullscreen, FPS counter, keyboard and mouse; choices kept in `out\launcher-settings.json`),
or `Play.bat` (or `build\RelWithDebInfo\gow2.exe` with the options below). Every option is
`--name=value` on the command line. Defaults are what you get with no options.

Game version (notes/63): the launcher's first choice is **Title Update 6** (the latest patch, 0.0.6.5; the default
once it is built) or **Disc** (0.0.0.5). Title Update 6 is `build\RelWithDebInfo-tu6\gow2.exe` with
`--update_data_root=game\tu6\package` (the update's files, extracted by `tools\stfs_extract.ps1`); `Play.bat` picks it
when it is there. Same game folder and same saves for both.

Gears of War 3 (user 14:52): the launcher's **Game** choice also starts the Gears3-Native build next door
(`..\Gears3-Native\build\RelWithDebInfo\gow3.exe`, its `game\extracted`), with the same resolution / SMAA / HDR /
fullscreen / keyboard-and-mouse switches; its own 16x-aniso and FPS-counter patches (`--gow3_patch_*`) are opt-in and
kept apart from Gears 2's boxes (off by default). From its chat's lists (20:08, 20:31; jobs 265 / 269): render
resolution 1x-4x (above 1x with --gow2_renderer_resolution_scale_threshold=1120), frame rate 30 / 60 / 120 / unlimited
(vsync off above 30; 120 and unlimited add --gow3_max_fps; game speed checked in its job 049), the motion blur / depth
of field / lens flares / field of view boxes (its port of our config edits: Xbox360-GearEngine.ini, GearCamera.ini
FOVAngle; it also rewrites the disc's table of contents), players, and --gow2_renderer_memexport=true always (fixes the
blue / rainbow skin and the black chapter-1 room). Not yet for Gears 3: ultrawide; DLSS / FSR fall back to SMAA, the FPS
bar to the game's own counter. HDR and keyboard/mouse are wired but not yet run on Gears 3; its FPS counter showed in job 263 [V]. Its saves
and logs go to this project's `out\gears3-userdata` and `out\play-logs\gears3-*` - a different save folder from
Gears3-Native's own Play.bat (`Gears3-Native\out\userdata`); nothing is written into the Gears 3 folder.
The launcher warns before starting either game while a test job holds the shared PC lock.
Also in the launcher (user 15:53): HDR paper white / peak (when HDR is on), exclusive fullscreen, monitor
(`--monitor`: 0 = default, 1..N = the SDK's N-th display), a 4x (5K) supersampling resolution (Gears 2), and Players
1-4 for split-screen (`--local_players`, Gears 2 - a controller per player).

## Picture
| What | Option | Default | Notes |
|---|---|---|---|
| Resolution | `--gow2_renderer_resolution_scale=N` | 1 (1280x720) | 2 = 2560x1440, 3 = 3840x2160 (4K), 4 = 5120x2880. Real detail, not an upscale (notes/53). |
| Anti-aliasing | `--gow2_present_aa=smaa` | off | SMAA 1x on the finished picture (notes/54). |
| Anti-aliasing: FSR / DLSS (experimental) | `--gow2_upscaler=fsr` / `dlss` | off | Temporal AA of the 3D scene before post-processing, at the render resolution (FSR native AA / DLAA; notes/64). Launcher: 'Anti-aliasing' list. **Experimental:** camera movement is in the motion vectors since job 255 (moving characters use the camera's motion only); the launcher falls back to SMAA when the build lacks the SDK or for Gears 3. |
| HDR | `--present_hdr=true` | off | HDR10 output; needs Windows HDR on. Tuning: `--gow2_hdr_paper_white` (203), `--gow2_hdr_peak` (1000), `--gow2_hdr_knee` (0.75), `--gow2_hdr_expand` (1.0) (notes/55). |
| Motion blur / Depth of field / Lens flares / Field of view | `--ue3_ini_edits="Xe-GearEngine.ini\|SystemSettings\|MotionBlur=False;..."`, `GearCamera.ini\|*\|FOVAngle*=1.2` | game's own | The game's own settings, changed in an edited copy of its config bundle (the game's files untouched; notes/64). Job 258 [V]: effects off 21 -> 17 passes per frame; FOV x1.2 widens the camera (80 -> 96 degrees); MaxSmoothedFrameRate 62 -> 45 fps honoured. Launcher: three boxes and 'Field of view (%)', Gears 2 only. |
| Ultrawide 21:9 | `--gow2_ultrawide=true` | off | A wider 2.33:1 view; fills a 21:9 screen, bars in other window shapes (notes/57; Canary patch by boma and TudorHorse). |
| 16x anisotropic filtering | `--gow2_patch_aniso16=true` | off | Community patch (boma). |

## Window
| What | Option | Default | Notes |
|---|---|---|---|
| Fullscreen | `--fullscreen=true` | off | Borderless fullscreen; `--fullscreen_exclusive=true` for exclusive. Can change while running (SDK). |
| Window size | `--window_width=W --window_height=H` | SDK default | Logical size. |
| Monitor | `--monitor=N` | 0 | Which monitor fullscreen uses. |

## Frame rate
| What | Option | Default | Notes |
|---|---|---|---|
| Unlock (with the next line) | `--gow2_gpu_vsync=false` | on | The game itself asks for 30 fps. |
| Engine limit | `--gow2_max_fps=N` | 0 (the game's 62) | N = cap, -1 = none. Ch02 ran ~100-115 fps uncapped (job 199) (notes/56). |
| FPS counter | `--gow2_fps_bar=top` / `bottom`, or `--gow2_patch_show_fps=true` | off | Bar: one line (FPS, frame time, slowest frame), drawn by the port over the game, nothing in the game changed (job 264; Gears 2 for now). Game's own: the engine's FPS / frame-time column (a small data patch; on Gears 3 too, job 263 [V]). Launcher: 'FPS counter' list. |

## Players
| What | Option | Default | Notes |
|---|---|---|---|
| Local profiles | `--local_players=N` | 1 | Signs in players 2-4 as offline profiles "Player 2".. (SDK patch 25). Split-screen works (job 228, notes/58). |

## Keyboard and mouse
| What | Option | Default | Notes |
|---|---|---|---|
| On | `--mnk_mode=true --mnk_mouse=true` | off | The SDK's keyboard/mouse -> controller emulation; a controller still works. |
| Sensitivity | `--mnk_sensitivity=N` | 1.0 | 0.01-10. |
| Keys | `--keybind_<button>=Key[,Key]` | SDK layout | Launcher.bat sets a Gears layout: WASD, mouse look, LMB fire, RMB aim, Space cover, F melee, E use, Q look-at, R reload, Tab Tac/Com, 1-4 weapons, MMB/Z zoom, C left stick, Esc/Enter pause, Backspace back. |
- In game: F4 opens the SDK's settings overlay. If you save there, it writes `build\RelWithDebInfo\gow2.toml`, which
  every later run loads (the test runs warn when that file exists).

## Test-only (leave off)
`--gow2_patch_horde_guard` (no longer needed since SDK patch 27), `--gow2_input_script`,
`--gow2_input_script2`, `--gow2_frame_shots`, `--gow2_frame_times`, `--gow2_gpu=sdk` (the emulator reference).

## Example: 4K, SMAA, HDR, uncapped
`gow2.exe --gow2_renderer_resolution_scale=3 --gow2_present_aa=smaa --present_hdr=true --gow2_gpu_vsync=false --gow2_max_fps=-1`

## Known limits (October 2026)
- Horde: the wave-1 crash is fixed (SDK patch 27, job 216); longer Horde play not tested yet.
- Split-screen co-op works (job 228): `--local_players=2`, a second controller, Co-op Campaign -> Play Split-Screen
  from the main menu. Only the menu route and one chapter seen so far.
- The disc image has 208 KB of zeros inside GearGame\CookedXenon\SP_Maria_1A.xxx (bytes 0x824000-0x858000, 104
  sectors, found by the Gears 3 chat; job 220: the same in the ISO and the original, so not the extraction). That
  level's data is damaged until the disc is checked against Redump and re-dumped if needed.
- Movie sound and real-controller feel not checked by the tests (no audio capture, scripted pads only).
