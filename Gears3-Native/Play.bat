@echo off
rem Gears3-Native: play the current developer build (adapted from Gears2-Native Play.bat).
rem Uses our renderer (layer 0's default). Extra options go after %%* - e.g. Play.bat --fullscreen=true
rem Stock emulator renderer instead (for comparison): Play.bat --gow2_gpu=sdk
rem Log: out\play-logs\<date-time>\gow3.log
rem Please don't start this while a test job is running (check jobs\queue\heartbeat.txt or the job window).
set "ROOT=%~dp0"
set "EXE=%ROOT%build\RelWithDebInfo\gow3.exe"
if not exist "%EXE%" ( echo Not built yet. & pause & exit /b 1 )
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "TS=%%i"
set "LOGDIR=%ROOT%out\play-logs\%TS%"
mkdir "%LOGDIR%" 2>nul
start "" /D "%ROOT%build\RelWithDebInfo" "%EXE%" --game_data_root="%ROOT%game\extracted" --user_data_root="%ROOT%out\userdata" --log_file="%LOGDIR%\gow3.log" --gow2_crash_dir="%LOGDIR%" %*
