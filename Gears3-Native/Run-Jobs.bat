@echo off
rem Gears3-Native job queue. Runs the jobs Claude puts in jobs\queue, one after another, at Below Normal priority
rem (so Gears 2's builds and test runs keep their speed). Leave this window open; close it to stop.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\queue_driver.ps1"
pause
