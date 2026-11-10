@echo off
rem Gears3-Native: play with the best settings verified so far (see notes\09-how-to-play.md).
rem Same as Play.bat (same saves: out\userdata; log: out\play-logs\<date-time>\gow3.log) plus:
rem   SMAA anti-aliasing (verified in job 033), borderless fullscreen (the SDK's option),
rem   60 fps (SIXTY=1 below; 0 = the original 30): our side stops forcing 30 fps and the game's own engine cap (62)
rem   applies. Job 035: 62 fps in 4 chapters, game speed correct, 20 minutes at 62.1 fps with flat memory, no errors.
rem   memexport (job 042): fixes the blue/rainbow skin (ch17 cutscenes) and the dark ch1 room - always on here.
rem   1440p (RES2=1 below; 0 = 720p): the picture at 2x; render targets up to 1120 wide (post-process chains, shadow
rem   maps, half-size buffers) stay at the console's 1x (job 042: no green tint; job 044: 92 fps ch8 / 141 fps ch22
rem   uncapped, up from 76 / 105 at threshold 512). Set RES2=0 if anything looks off.
rem Frame-rate bar at the top (job 052): remove --gow2_fps_bar=top from the BEST line below to hide it.
rem Extra options can still be added after the file name, e.g.  Play-Best.bat --fullscreen=false
rem Please don't start this while a test job is running (check jobs\queue\heartbeat.txt).
set "SIXTY=1"
set "RES2=1"
set "BEST=--gow2_present_aa=smaa --fullscreen=true --gow2_renderer_memexport=true --gow2_fps_bar=top"
if "%SIXTY%"=="1" set "BEST=%BEST% --gow2_gpu_vsync=false"
if "%RES2%"=="1" set "BEST=%BEST% --gow2_renderer_resolution_scale=2 --gow2_renderer_resolution_scale_threshold=1120"
call "%~dp0Play.bat" %BEST% %*
