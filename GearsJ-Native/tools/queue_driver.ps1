# GearsJ-Native (Judgment) job queue, adapted from Gears3-Native's queue_driver.ps1 (same project, MIT).
# Runs jobs\queue\*.ps1 one after another, oldest name first. A job is picked only when complete (its last line is
# '# END-OF-JOB') and at least 3 s old. Output: jobs\queue\logs\<name>.log; finished files move to jobs\queue\done\.
# Stops after -IdleMinutes with an empty queue or when jobs\queue\stop.flag appears. Heartbeat: jobs\queue\heartbeat.txt.
# 14:09 (Shagg away): idle limit 120 -> 1440 min; at 120 it quit at 13:10 and two jobs sat unrun until it was restarted.
# Rules agreed with the Gears 2 chat (2026-10-21 21:01): this queue runs at Below Normal priority (child processes
# inherit it), never writes into Documents\Gears2-Native or Gears3-Native (except the shared PC lock files), and never starts the game unless a gap has been agreed.
param([int]$IdleMinutes = 1440, [string]$QueueDir = 'jobs\queue')
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$q = Join-Path $root $QueueDir
foreach ($d in 'logs', 'done') { New-Item -ItemType Directory -Force -Path (Join-Path $q $d) | Out-Null }
try { (Get-Process -Id $PID).PriorityClass = 'BelowNormal' } catch {}
$idleSince = Get-Date
$dlog = Join-Path $q 'driver.log'
function Note($m) { $l = "$(Get-Date -Format s) $m"; Write-Host $l; Add-Content -LiteralPath $dlog -Value $l -Encoding utf8 }
Note "Judgment queue driver started, pid $PID, priority $((Get-Process -Id $PID).PriorityClass)"
trap { Note "DRIVER ERROR: $_"; continue }
while ($true) {
  if (Test-Path (Join-Path $q 'stop.flag')) { Remove-Item (Join-Path $q 'stop.flag') -Force; Note "stop.flag - exiting"; break }
  $next = Get-ChildItem $q -Filter '*.ps1' -File | Where-Object { $_.LastWriteTime -lt (Get-Date).AddSeconds(-3) } |
    Where-Object { (Get-Content $_.FullName -Tail 1) -match '^# END-OF-JOB' } | Sort-Object Name | Select-Object -First 1
  Set-Content (Join-Path $q 'heartbeat.txt') "alive=$(Get-Date -Format s) pid=$PID running=$(if ($next) { $next.Name } else { '-' })" -Encoding UTF8
  if (-not $next) {
    if ((Get-Date) - $idleSince -gt [TimeSpan]::FromMinutes($IdleMinutes)) { Note "idle $IdleMinutes min - exiting"; break }
    Start-Sleep 2; continue
  }
  $log = Join-Path $q ("logs\" + $next.BaseName + '.log')
  $t = Get-Date
  Note "start $($next.Name)"
  & powershell -NoProfile -ExecutionPolicy Bypass -File $next.FullName *>&1 | Out-File -LiteralPath $log -Encoding utf8
  $secs = [int]((Get-Date) - $t).TotalSeconds
  Add-Content -LiteralPath $log -Value "== QUEUE: finished $(Get-Date -Format s) after $secs s, exit $LASTEXITCODE" -Encoding utf8
  Move-Item -LiteralPath $next.FullName -Destination (Join-Path $q ("done\" + $next.Name)) -Force
  Note "done  $($next.Name) ($secs s)"
  $idleSince = Get-Date
}
