@echo off
rem Gears2-Native: play the current developer build with our renderer (gpu_plugin=xenos is our graphics system).
rem Launcher.bat does the same with a few settings to pick; extra options here go after %*.
rem Log: out\play-logs\<date-time>\gow2.log
set "ROOT=%~dp0"
rem Title Update 6 (notes/63) when it is built and extracted, else the disc build.
set "BUILD=%ROOT%build\RelWithDebInfo"
set "UPD="
if exist "%ROOT%build\RelWithDebInfo-tu6\gow2.exe" if exist "%ROOT%game\tu6\package\default.xexp" (
  set "BUILD=%ROOT%build\RelWithDebInfo-tu6"
  set "UPD=--update_data_root=%ROOT%game\tu6\package"
)
set "EXE=%BUILD%\gow2.exe"
if not exist "%EXE%" ( echo Not built yet. & pause & exit /b 1 )
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "TS=%%i"
set "LOGDIR=%ROOT%out\play-logs\%TS%"
mkdir "%LOGDIR%" 2>nul
start "" /D "%BUILD%" "%EXE%" --game_data_root="%ROOT%game\extracted" --user_data_root="%ROOT%out\userdata" --log_file="%LOGDIR%\gow2.log" --gpu_plugin=xenos %UPD% %*
