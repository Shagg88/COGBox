@echo off
rem Gears3-Native: play the Title Update 6 build (notes/24) until the launcher has a "Game version" choice for Gears 3.
rem Same game folder and saves as Play.bat; the update in game\tu6\package is mounted and applied at load.
rem Plays with the game's default options (the launcher's settings are not applied). Extra options go after %%*.
rem Log: out\play-logs\<date-time>-tu6\gow3.log
set "ROOT=%~dp0"
set "EXE=%ROOT%build\RelWithDebInfo-tu6\gow3.exe"
if not exist "%EXE%" ( echo TU6 build not made yet. & pause & exit /b 1 )
if not exist "%ROOT%game\tu6\package\default.xexp" ( echo game\tu6\package\default.xexp missing. & pause & exit /b 1 )
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "TS=%%i"
set "LOGDIR=%ROOT%out\play-logs\%TS%-tu6"
mkdir "%LOGDIR%" 2>nul
start "" /D "%ROOT%build\RelWithDebInfo-tu6" "%EXE%" --game_data_root="%ROOT%game\extracted" --update_data_root="%ROOT%game\tu6\package" --user_data_root="%ROOT%out\userdata" --log_file="%LOGDIR%\gow3.log" --gow2_crash_dir="%LOGDIR%" %*
