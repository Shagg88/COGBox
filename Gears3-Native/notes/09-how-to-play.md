# 09 - How to play (Gears 3, October 2026)

## Start it
- `Play-Best.bat` (double-click): our renderer, SMAA anti-aliasing, borderless fullscreen, 60 fps (job 035; set
  `SIXTY=0` inside the file for the original 30), memexport (fixes blue skin + dark ch1 room, job 042) and 1440p
  (`RES2=1`; `RES2=0` for 720p). `Play-Best.bat --fullscreen=false` for a window.
- `Play.bat`: the plain build (30 fps, no extras).
- Gears 2's `Launcher.bat` can also start Gears 3 (checkboxes for the options). It keeps Gears 3 saves in
  `Gears2-Native\out\gears3-userdata`, while Play.bat / Play-Best.bat use `Gears3-Native\out\userdata` - pick one way
  and stay with it, or the campaign is split between two save folders.
- Don't start the game while a test job runs (`jobs\queue\heartbeat.txt` says `running=...`): both use the GPU and the
  job's results would be wrong.

## Options (`--name=value` after the .bat name or in the launcher)
| What | Option | Default | Status for Gears 3 |
|---|---|---|---|
| Anti-aliasing | `--gow2_present_aa=smaa` | off | Verified (job 033). |
| Fullscreen | `--fullscreen=true` | off | SDK option. |
| 60 fps | `--gow2_gpu_vsync=false` | off (30 fps) | **Verified (job 035)**: 62 fps in ch0/8/17, 60.1 average in ch22 (one dip to ~50); cutscene timing matches 30 fps at the same real time; 20 minutes at 62.1 fps every minute, memory flat (~1.6-1.8 GB), no errors. No game patch: our side stops forcing every second refresh; the game's own engine then caps at **62 fps** (`MaxSmoothedFrameRate=62`, `bSmoothFrameRate=TRUE` in `Xbox360-GearEngine.ini` inside `Coalesced_INT.bin` [V]). Above 62: `--gow3_max_fps=N` (or -1 = no limit) overrides that one value - in the next build, being measured (job 038). |
| Resolution x2 | `--gow2_renderer_resolution_scale=2 --gow2_renderer_resolution_scale_threshold=1120` | 1 | **Works (jobs 042, 044)**: targets up to 1120 wide (post-process, shadow maps, half-size buffers) stay 1x as on the console; no green tint; uncapped 92 fps ch8, 141 fps ch22 (512: 76 / 105). Without a threshold: green tint. |
| Memexport | `--gow2_renderer_memexport=true` | off | **Works (job 042)**: fixes the blue/rainbow skin (ch17) and the black ch1 room. Copies exported data back to memory (`--gow2_renderer_memexport_readback`, on). |
| 16x anisotropic | `--gow3_patch_aniso16=true` | off | Works (job 033), but it is a game patch - off unless wanted (Shagg 14:47). |
| HDR | `--present_hdr=true` | off | Not tested on Gears 3 yet. |
| FPS counter | `--gow3_patch_show_fps=true` | off | Game patch, not tested yet. |

## Known picture problems (being worked on)
- FIXED with memexport (job 042): ch17 blue/rainbow skin (the game moves textures with GPU copies) and the black
  ch1 room. Still open: ground textures flicker a little (Shagg 18:17); a light-function shader (ps FC367470036D7CFE)
  slightly brighter in parts of ch1; above 62 fps game speed not yet checked.
- Everything else seen so far (prologue, chapters 1, 8, 14, 17, 19, 22, 27) runs clean at 30 fps, no crashes.

## If something looks wrong
Note the chapter and roughly what you were doing, and send the newest folder in `out\play-logs` (its `gow3.log`).
A screenshot (Win+Shift+S) helps.
