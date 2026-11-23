# Logging levels and the bug-report zip

Shagg, 23 Nov 2026: players on setups we can't test (AMD, Linux / Proton, Mac / CrossOver) can turn on more logging
when they hit an interesting bug, then hand us one zip.

Where: launcher, **Gameplay tab, BUG REPORTS** (right column, under SAVES). One value for all four games (Shared).
Code: one table, `log_switches()` in `Gears2-Native/cogbox/src/gears_settings.cpp`; `log_level_args()` turns it into
each game's switches and `build_args()` adds them just before `extra-args.txt` (so a player's own line still wins).

## The three levels

Each switch is sent only when the game's exe (or the SDK runtime DLL) contains the switch's description text, so an
older build just gets fewer switches; `args.txt` says which were skipped (`launcher: logging switches this build
doesn't have: ...`). The option itself is greyed in a build without the shared frame log.

| Level | Turns on | Extra size | Slowdown |
|---|---|---|---|
| Normal | nothing (as before) | - | - |
| Detailed | `--gow2_hitch_report_ms=50` (first 200 slow frames, where the time went), `--gow2_texture_phase_log_ms=20` (slow texture loads by phase), `--gow2_renderer_log_every=300` (renderer summary, already the default), `--gow2_frame_log=true` (summary every 600 frames + spike lines, no CSV), `--gow2_frame_times=600`, `--gow2_draw_stats=600`, `--<game>_log_weapon_state=true`, `--cogbox_test_state=true` (once a second: in gameplay / paused / cutscene), `--ue3_mouse_log=true` (once a second), `--log_flush_interval=1` (the log survives a hard crash) | ~2-5 MB per hour [G] | none visible [G] |
| Everything | all of Detailed, with tighter thresholds (`hitch 33 ms`, `texture phase 8 ms`, `frame_times / draw_stats every 60`), plus `--log_level=debug`, `--gow2_frame_log_csv=true` (~45 MB per hour), `--gow2_frame_timeline=true`, `--gow2_d3d_watch=600` (Gears 2 only), `--gow2_renderer_camera_log=300` (max 2000 lines), `--gow2_renderer_bc_gpu_page_log=200`, `--gow2_renderer_texture_stale_log=600` (first 400 events), `--gow2_profile=20` (20 s sampling profile from frame 600), `--<game>_profile_log=true`, `--xam_stats_log=true`, screenshots `--gow2_frame_shots=900 --gow2_frame_shots_last=36000` into `<play>\shots` (one every 15 s at 60 fps, none after ~10 min: at most 40 PNGs), a fuller crash dump, `options.txt` | ~1-6 MB per minute of log/CSV plus 2-8 MB per screenshot at 1080p-4K; capped at roughly 300-400 MB [G] | some: debug log lines, the profiler's 20 s, PNG encoding every 15 s [G] |

Both Detailed and Everything write `options.txt` (every launcher setting of that play by id) into the play's folder.

Limits on Everything:
- **One play only**: when the game closes, the level goes back to Detailed (for every game) and the launcher says so.
- **Disk**: with under 4 GB free where the logs go, it starts as Detailed (`args.txt` says so).
- The heavy switches cap themselves (first 200 hitch frames, 2000 camera lines, 400 stale-texture events, 40 screenshots,
  one 20 s profile). The per-frame CSV and debug log lines are not capped; the one-play rule bounds them.
- Crash dump: the launcher adds DataSegs, HandleData, UnloadedModules, ProcessThreadData and FullMemoryInfo to the
  game's own flags (tens of MB). Not a full-memory dump: that would be several GB and holds the player's data.
- A "Prepare all shaders" run always logs at Normal.

Left out on purpose: `--d3d12_debug` / `--vulkan_validation_enabled` (need the debug layers installed, very slow, and
can stop the device from starting), `--log_noisy` and `--log_level=trace` (unbounded), `--cogbox_net_trace` (other
players' addresses), `--<game>_memory_log_seconds` (off for players since an outside review: the gauge thread could read
the kernel during shutdown), draw-by-draw and shader/texture dumps (GB-sized, frame lists needed).

## Collect logs for a bug report

Button under the level. Makes `<COGBox>\reports\CogBox-logs-<game>-<time>.zip` from the game's newest play folder:
the game log, `args.txt`, `options.txt`, `menu.txt`, `vkd3d.log` (Proton), the frame CSV, crash `.txt` / `.dmp`, the
newest 40 screenshots in `shots\`, `settings.ini`, the launcher's `padlog.txt` when `COGBOX_PADLOG=1` made one, and
`report.txt` (Windows, CPU, RAM, GPUs and drivers, displays, Proton / Steam Deck details, which experimental settings
were on, game file fingerprints). Nothing is uploaded. It reuses the problem report's code (`make_report(..., collect)`,
`report_write`), so it is masked the same way:

- Windows user name, PC name, profile folder, Linux / Mac account name (`/home/<name>`, `/Users/<name>`) -> `%USERNAME%`
  / `%COMPUTERNAME%` / `%USERPROFILE%`; other drive paths keep only their last part (`<path>\default.xex`).
- Public IPv4 addresses -> `a.x.x.d`; friends' IP lists -> a count; the online account token -> `<token>`.
- New: the profile's gamertags (main + split-screen 2-4) -> `<gamertag>` (whole words, 3+ letters, not generic names).
- Crash dumps and screenshots: user and PC names overwritten with x's of the same length.

## Inventory of logging / diagnostic switches (23 Nov 2026)

`gow2_` names in layer 0 (`Gears2-Native/src/layer0_xenos`) are built into all four games (shared renderer). Cost:
CPU and disk with the switch on. Privacy: what it can write that identifies a person. Paths are masked by the zip.

| Switch (file:line) | Games | Cost | Privacy | Level |
|---|---|---|---|---|
| `log_level` (SDK core/logging.cpp:40) | all | debug: several MB/min [G] | paths | Everything (debug) |
| `log_flush_interval` (SDK core/logging.cpp:51) | all | ~0 | - | Detailed |
| `log_noisy` (SDK core/logging.cpp:49) | all | high, unbounded | paths | no |
| `d3d12_debug` (SDK ui/d3d12/d3d12_provider.cpp:23) + `gow2_renderer_debug` (layer0 renderer/shadow_renderer_cvars.cpp:27) | all | very slow, needs debug layer | - | no |
| `vulkan_validation_enabled` (SDK ui/vulkan/vulkan_provider.cpp:21) | all | very slow, needs layers | - | no |
| `trace_gpu_frames` (SDK graphics/command_processor.cpp:48) | all | GB | - | no |
| `cogbox_content_log` (patches/sdk-patch48-content-log.diff:10) | all | small, default on | content names | default |
| `cogbox_save_trace` (sdk-patch56:74) | all | small | save names | no [G: could join Detailed] |
| `cogbox_net_trace` (sdk-patch79-online.diff:2395) | all | small | other players' IPs | no |
| `xam_stats_log` (sdk-patch123-local-stats-store.diff:36) | all | small | - | Everything |
| `gow2_hitch_report_ms` (shadow_renderer_cvars.cpp:270) | all | ~0, 200 lines max | - | Detailed / Everything |
| `gow2_renderer_log_every` (shadow_renderer_cvars.cpp:295) | all | default 300 | - | Detailed |
| `gow2_texture_phase_log_ms` (renderer/texture_cache.cpp:54) | all | small | - | Detailed / Everything |
| `gow2_renderer_bc_gpu_page_log` (texture_cache.cpp:102) | all | 500 lines max | - | Everything |
| `gow2_texture_dump_dir` / `gow2_texture_pack_dump_dir` (texture_cache.cpp:146 / 161) | all | GB | - | no |
| `gow2_frame_times` (observer_hub.cpp:28) | all | ~0 | - | Detailed / Everything |
| `gow2_frame_log` (+ `_every`, `_spike_ms`, `_spike_lines`, `_csv`) (devtools/frame_log.cpp:51-69) | all | CSV ~45 MB/h | - | Detailed (+CSV Everything) |
| `gow2_frame_timeline` (frame_log.cpp:424) | all | small | - | Everything |
| `gow2_draw_stats` (devtools/draw_stats.cpp:19) | all | small | - | Detailed / Everything |
| `gow2_draw_log_frames` / `_full` / `_textures` (devtools/draw_log.cpp:22-25) | all | MB per frame | - | no (needs frame numbers) |
| `gow2_resolve_log_frames` (devtools/resolve_log.cpp:25) | all | MB per frame | - | no |
| `gow2_renderer_draw_report_frame(s)` (shadow_renderer_cvars.cpp:437-459) | all | large | - | no |
| `gow2_renderer_tile_trace`, `gow2_renderer_log_vs` (shadow_renderer_cvars.cpp:657-672) | all | large | - | no |
| `gow2_renderer_camera_log` (shadow_renderer_cvars.cpp:797) | all | 2000 lines max | - | Everything |
| `gow2_renderer_texture_stale_log` (shadow_renderer_cvars.cpp:817) | all | 400 events max | - | Everything |
| `gow2_renderer_log_aliases` (shadow_renderer_cvars.cpp:1344) | all | default 20 | - | default |
| `gow2_renderer_vulkan_record_thread_log` (shadow_renderer_cvars.cpp:433) | all (Vulkan) | default 60 | - | default |
| `gow2_ao_log`, `gow2_ao_gpu_log`, `gow2_ssil_log`, `gow2_ao_trace_*`, `gow2_pcss_trace_frames` (shadow_renderer_cvars.cpp:898-1017) | all | small to large | - | no (feature studies) |
| `gow2_fg_debug` (renderer/frame_generation.cpp:43) | all | draws on screen | - | no |
| `gow2_profile` (+ `_start`, `_top`) (devtools/profiler.cpp:25-27) | all | 20 s of sampling | - | Everything |
| `gow2_frame_shots` (+ `_dir`, `_first`, `_last`) (devtools/frame_capture.cpp:56-60) | all | 2-8 MB per PNG | what's on screen (gamertags in menus) | Everything |
| `gow2_screenshot_presented` (shadow_renderer_cvars.cpp:801), screenshot key `\` | all | per press | screen | no |
| `gow2_upscale_capture_key` / `_frames` (shadow_renderer_cvars.cpp:1077-1080) | all | large | - | no |
| `gow2_dump_image` (devtools/image_dump.cpp:12), `gow2_shader_dump_dir` (devtools/shader_dump.cpp:23), `gow2_renderer_hlsl_dump_*` (renderer/shader_cache.cpp:48-50), `ue3_shader_dump_dir` (layer2_ue3/shader_prebuild.cpp:106) | all | GB | - | no |
| `gow2_crash_dir`, `gow2_crash_minidump`, `gow2_crash_test` (devtools/crash_report.cpp:42-52) | all | on crash | names in dump (x'd) | launcher adds dump flags at Everything |
| `cogbox_save_log` (devtools/save_watch.cpp:34) | all | default on | relative paths | default |
| `gow2_d3d_watch` (layer1_xdk7645/d3d_watch.cpp:21) | G2 | small | - | Everything (G2) |
| `ue3_voice_language_log` (layer2_ue3/config_install.cpp:768) | all | default 40 lines | - | default |
| `<game>_log_weapon_state` (G1 adaptive_triggers.cpp:425, G2 :121, G3 :113, J :154) | all | event-based | - | Detailed (G1 already on by default) |
| `cogbox_test_state` (G1 adaptive_triggers.cpp:419, G2 :410, G3 :377, J :461) | all | 1 line/s | - | Detailed |
| `ue3_mouse_log` (each game's layer3 ue3_mouse_look.h: G1 :103, G2 :115, G3 :123, J :100) | all | 1 line/s | - | Detailed |
| `<game>_profile_log` (G1 unlocks.cpp:42, G2 unlocks.cpp:44, G3 unlocks.cpp:53, J profile_log.cpp:45) | all | once per change | - | Everything |
| `<game>_tick_log` (G2 game_patches.cpp:130, G3 :487, J :127) | G2 G3 J | default 600 | - | default |
| `<game>_memory_log_seconds` (G1 game_patches.cpp:41, G2 :145, G3 :1055, J :717) | all | small | - | no (shutdown risk) |
| `gow3_log_file_opens` (G3 game_patches.cpp:552), `gow3_squad_slot_log` (checkpoint_stream.cpp:55), `gow3_log_bot_classes` (horde_bot_characters.cpp:44), `gow3_mod_file_log` (mod_chapters.cpp:64), `mod_squad_log`, `mod_kismet_trace`, `mod_log_props` | G3 | small to large | file names | no (G3 studies) |
| `gow2_log_texture_defrag` (G2 game_patches.cpp:108), `gow2_loc_log` (G2 :1008) | G2 | small | - | no |
| `<game>_first_person_diag` (G2 first_person.cpp:101, G3 :91, J :73) | G2 G3 J | small | - | no (feature off) |
| `ue3_dump_settings` (G3 extras.cpp:44) | G3 | once | - | no |
| `COGBOX_PADLOG=1` (launcher front.cpp:726) | launcher | 1 line/frame while input changes | - | not a game switch; zipped when present |
| `VKD3D_LOG_FILE` (launcher play.cpp, Proton only) | all under Proton | small | Linux paths (masked) | always under Proton |
| `extra-args.txt` beside CogBox.exe | all | - | - | any switch by hand |

## Parity gaps found

- `<game>_tick_log`: not in Gears 1 (G2 / G3 / J have it, on by default). Not used by the levels.
- `gow2_d3d_watch` (layer 1): Gears 2 only - the others have no layer 1. Everything sends it only where the exe has it.
- No game has an in-game controller input log; only the launcher's `COGBOX_PADLOG`. A shared one (layer 0 / SDK input)
  would be the right place - not done here.
- `gow1_log_weapon_state` is on by default in Gears 1 and off in the others (the levels turn it on everywhere).
- G2-only `log_texture_defrag` / `loc_log` and the G3-only studies are already listed in docs/parity.md.

## Test plan (not run)

For each game (Gears 1, 2, 3, Judgment) and each level (Normal, Detailed, Everything): set the level, Play, reach
gameplay by docs/test-routes.md, play 60 s, quit, then press "Collect logs for a bug report". Check:

1. `args.txt`: Normal has no `launcher: logging level` line; Detailed says `logging level Detailed (10 switches`;
   Everything `(22 switches` in Gears 2, 21 in the others, and names `gow2_d3d_watch` as skipped there.
2. The game log: Detailed has `cogbox_test_state: GAMEPLAY` once a second, frame log summary lines, renderer
   summaries every 300 frames, weapon-state lines after a weapon switch; Everything also has debug lines, the frame
   timeline line per 600 frames and the profiler's report after ~30 s.
3. Everything: `<play>\shots` has ~4 PNGs after 60 s; `<log name>-frames.csv` exists; `options.txt` in the play folder;
   after the game closes the launcher says it is back to Detailed and the Gameplay tab shows Detailed in all games.
4. The zip in `<COGBox>\reports`: has report.txt, session/*.log, args.txt, options.txt, shots/*.png (Everything),
   settings.ini; search it for your Windows user name, PC name, gamertag and public IP: none found.
5. Crash: with Everything, `--gow2_crash_test=<mode>` in extra-args.txt; the .dmp is bigger than at Normal and opens in
   WinDbg / Visual Studio.
6. An older exe: the option is greyed ("Not yet in ... needs its next build").
