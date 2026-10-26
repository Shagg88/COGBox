@echo off
rem Runs jobs Claude queues in jobs\. Leave this window open while we work; close it to stop.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\job_runner.ps1"
pause
