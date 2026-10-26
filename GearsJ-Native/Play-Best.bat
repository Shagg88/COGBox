@echo off
rem GearsJ-Native: play with Gears 3's best settings (Gears3-Native\Play-Best.bat, notes\09-how-to-play.md there).
rem Same as Play.bat (same saves: out\userdata; log: out\play-logs\<date-time>\gowj.log) plus:
rem   SMAA anti-aliasing, borderless fullscreen, 60 fps (SIXTY=1: our side stops forcing 30 fps and the game's own
rem   engine cap applies - no patch), 1440p (RES2=1: the picture at 2x; render targets up to 1120 wide stay 1x, which
rem   is Judgment's default threshold), and the frame-rate bar at the top.
rem Not tested on Judgment yet (D9): these work on Gears 2 and 3 with the same renderer. Set SIXTY=0 or RES2=0 below
rem if anything looks off, and tell Claude what you saw.
rem Extra options can still be added after the file name, e.g.  Play-Best.bat --fullscreen=false
rem Please don't start this while a test job is running (check jobs\queue\heartbeat.txt).
set "SIXTY=1"
set "RES2=1"
set "BEST=--gow2_present_aa=smaa --fullscreen=true --gow2_fps_bar=top"
if "%SIXTY%"=="1" set "BEST=%BEST% --gow2_gpu_vsync=false"
if "%RES2%"=="1" set "BEST=%BEST% --gow2_renderer_resolution_scale=2"
call "%~dp0Play.bat" %BEST% %*
