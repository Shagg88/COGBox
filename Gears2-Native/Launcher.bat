@echo off
rem Gears2-Native: the tiny launcher - pick a few settings and press Play (tools\launcher.ps1).
start "" powershell -NoProfile -ExecutionPolicy Bypass -STA -WindowStyle Hidden -File "%~dp0tools\launcher.ps1"
