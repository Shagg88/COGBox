@echo off
rem GearsJ-Native (Judgment) job queue. Runs the jobs Claude puts in jobs\queue, one after another, at Below Normal
rem priority (so Gears 2 and Gears 3 keep their speed). Leave this window open; close it to stop.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\queue_driver.ps1"
pause
