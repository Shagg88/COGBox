@echo off
rem Gears3-Native: play with the best settings (as Play-Best.bat: SMAA, fullscreen, 60 fps, 1440p, memexport) and
rem "scout" logging for bug hunting while playing any chapter. Everything goes to out\play-logs\<date-time>\:
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
set "OPTS=%OPTS% --gow2_frame_shots=1200 --gow2_frame_dir=%LOGDIR%\frames --gow2_frame_times=600"
> "%LOGDIR%\options.txt" echo Play-Scout started %DATE% %TIME%
>> "%LOGDIR%\options.txt" echo %OPTS% %*
echo Logging to %LOGDIR%
start "" /D "%ROOT%build\RelWithDebInfo" "%EXE%" --game_data_root="%ROOT%game\extracted" --user_data_root="%ROOT%out\userdata" --log_file="%LOGDIR%\gow3.log" --gow2_crash_dir="%LOGDIR%" %OPTS% %*
