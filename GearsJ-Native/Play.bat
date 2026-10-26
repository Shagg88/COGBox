@echo off
rem GearsJ-Native: play the current developer build (adapted from Gears3-Native's Play.bat, D5).
rem Our renderer (layer 0), Title Update 4 applied at start-up, memexport on (Judgment's default, job 011).
rem Extra options go after the file name, e.g.  Play.bat --fullscreen=true   or   Play.bat --gow2_fps_bar=top
rem Stock emulator renderer instead (for comparison): Play.bat --gow2_gpu=sdk
rem Straight into the first chapter: Play.bat --ue3_launch="SP_00_Museum_P -nostartupmovies"
rem Shader / pipeline cache: out\userdata\cache (job 021 fills it for every chapter; a clean build folder no longer loses it).
rem Saves: out\userdata (shared with the test runs). Saving is not checked yet - a checkpoint may not stick.
rem Log: out\play-logs\<date-time>\gowj.log - send that folder if something looks wrong.
rem Please don't start this while a test job is running (check jobs\queue\heartbeat.txt or the job window).
set "ROOT=%~dp0"
set "EXE=%ROOT%build\RelWithDebInfo\gowj.exe"
if not exist "%EXE%" ( echo Not built yet. & pause & exit /b 1 )
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "TS=%%i"
set "LOGDIR=%ROOT%out\play-logs\%TS%"
mkdir "%LOGDIR%" 2>nul
start "" /D "%ROOT%build\RelWithDebInfo" "%EXE%" --game_data_root="%ROOT%game\extracted" --update_data_root="%ROOT%game\tu4\package" --user_data_root="%ROOT%out\userdata" --gow2_shader_cache_dir="%ROOT%out\userdata\cache" --gpu_plugin=xenos --log_file="%LOGDIR%\gowj.log" --gow2_crash_dir="%LOGDIR%" %*
