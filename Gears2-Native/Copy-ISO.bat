@echo off
rem Copies your Gears of War 2 disc image into this project. The source folder is only read.
set "SRC=C:\Emulation\Games\Xbox 360\Games"
set "DST=%~dp0game"
robocopy "%SRC%" "%DST%" "Gears of War 2.iso" /J /R:1 /W:1
powershell -NoProfile -Command "$f=Get-Item -LiteralPath '%DST%\Gears of War 2.iso' -ErrorAction SilentlyContinue; if ($f -and $f.Length -eq 7835492352) { Write-Host 'OK: copied, 7,835,492,352 bytes.' } else { Write-Host 'PROBLEM: the copy is missing or the wrong size.' }"
pause
