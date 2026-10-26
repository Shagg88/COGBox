@echo off
rem Gears3-Native: Play-Scout.bat (best settings + scout logging) WITH the 2026-10-23 renderer fixes switched on
rem (patches 0004-0007 and 0009, Shagg's go 01:06 / 01:35): page-watch narrow widening (0009: black / flickering
rem characters?), export pending mirror (NPC vertex explosions), shader warm-up + an 8 ms
rem per-frame shader wait (pop-in), and the graphics-card crash report (on by default). 0005 (32/64-bit transfers) is
rem OFF: it caused the yellow brick pattern (01:31). Needs the build from job 070.
rem Everything goes to out\play-logs\<date-time>\:
rem   gow3.log      - the game log: renderer summary every 300 frames (draws, skipped draws, shaders still compiling,
rem                   copies), frame-time stats every 600 frames, errors, crash reports
rem   options.txt   - this session's settings and start time
rem   frames\       - a full-size screenshot every 1200 frames (about every 20 s at 60 fps; ~6 MB each), named by
rem                   frame number so a moment can be matched to the log lines around it
rem   crash-*.txt   - only if the game crashes
rem When something looks wrong: note the time on your clock and roughly what happened; Win+Shift+S for a screenshot
rem helps too. The log lines carry the time, so the moment can be found afterwards.
rem Please don't start this while a test job is running (check jobs\queue\heartbeat.txt).
set "ROOT=%~dp0"
set "EXE=%ROOT%build\RelWithDebInfo\gow3.exe"
if not exist "%EXE%" ( echo Not built yet. & pause & exit /b 1 )
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "TS=%%i"
set "LOGDIR=%ROOT%out\play-logs\%TS%"
mkdir "%LOGDIR%\frames" 2>nul
set "OPTS=--gow2_present_aa=smaa --fullscreen=true --gow2_renderer_memexport=true --gow2_gpu_vsync=false --gow2_fps_bar=top"
set "OPTS=%OPTS% --gow2_renderer_resolution_scale=2 --gow2_renderer_resolution_scale_threshold=1120"
set "OPTS=%OPTS% --gow2_renderer_watch_narrow=true --gow2_renderer_export_pending_mirror=true"
set "OPTS=%OPTS% --gow2_renderer_shader_warmup=true --gow2_renderer_shader_wait_ms=8"
set "OPTS=%OPTS% --gow2_frame_shots=1200 --gow2_frame_dir=%LOGDIR%\frames --gow2_frame_times=600"
> "%LOGDIR%\options.txt" echo Play-Patches started %DATE% %TIME%
>> "%LOGDIR%\options.txt" echo %OPTS% %*
echo Logging to %LOGDIR%
start "" /D "%ROOT%build\RelWithDebInfo" "%EXE%" --game_data_root="%ROOT%game\extracted" --user_data_root="%ROOT%out\userdata" --log_file="%LOGDIR%\gow3.log" --gow2_crash_dir="%LOGDIR%" %OPTS% %*
