@echo off
rem Gears 3 DLSS test B (Shagg 09:53: research the DLSS / FSR black triangles): chapter 8's opening cutscene,
rem DLSS on, 2x resolution, with the Xbox 360's own MSAA sample positions (Gears 2 patch 0089).
rem Lead (Canary cc4981a, draw_util.h): UE3 nudges its viewport by the Xbox's MSAA sample 0 when it restores depth into
rem an MSAA target; with PC sample positions the restored depth can fail later depth tests and leave parts unshaded.
rem Gears 3's ch8 draws a half-size 4x MSAA target. A and B differ ONLY in --gow2_renderer_msaa_sample_positions.
rem The chapter opens paused: press A / Start. Watch the close-ups of faces for black triangles. Quit with Alt+F4.
rem Needs the DLSS file next to gow3.exe (the launcher's DLSS choice uses the same one).
call "%~dp0Play.bat" --ue3_launch="GearGame_P?loadcheckpoint=0?chapter=8" --gow2_upscaler=dlss --gow2_renderer_msaa_sample_positions=xenos --gow2_renderer_resolution_scale=2 --gow2_renderer_resolution_scale_threshold=1120 --gow2_renderer_memexport=true --gow2_gpu_vsync=false --fullscreen=true %*
