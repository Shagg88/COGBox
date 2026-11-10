@echo off
rem Unpacks the Judgment disc image into GearsJ-Native\game\extracted (read-only on the image).
rem Runs at Below Normal priority so the other Gears chats keep their speed. Takes about 5-15 minutes.
start "" /belownormal /wait /b powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\unpack_disc.ps1"
echo.
echo Done. You can close this window.
pause
