# Gears2-Native job queue (R14): runs jobs\queue\*.ps1 one after another, oldest name first, with no gap.
# A job file is picked only when complete (its last line is '# END-OF-JOB') and at least 3 s old.
# Output: jobs\queue\logs\<name>.log; finished files move to jobs\queue\done\. Stops after -IdleMinutes with an empty
# queue or when jobs\queue\stop.flag appears. Heartbeat: jobs\queue\heartbeat.txt.
# -QueueDir jobs\queue-b runs a second lane for compute-only jobs (shader checks, oracle, analysis) next to the main
# lane: lane-B jobs never build the game or the SDK and never start the game, so they can't collide with lane A.
param([int]$IdleMinutes = 90, [string]$QueueDir = 'jobs\queue')
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$q = Join-Path $root $QueueDir
foreach ($d in 'logs', 'done') { New-Item -ItemType Directory -Force -Path (Join-Path $q $d) | Out-Null }
try { (Get-Process -Id $PID).PriorityClass = 'High' } catch {}
$idleSince = Get-Date
Write-Host "queue driver started $(Get-Date -Format s), pid $PID"
while ($true) {
  if (Test-Path (Join-Path $q 'stop.flag')) { Remove-Item (Join-Path $q 'stop.flag') -Force; Write-Host "stop.flag - exiting"; break }
  $next = Get-ChildItem $q -Filter '*.ps1' -File | Where-Object { $_.LastWriteTime -lt (Get-Date).AddSeconds(-3) } |
    Where-Object { (Get-Content $_.FullName -Tail 1) -match '^# END-OF-JOB' } | Sort-Object Name | Select-Object -First 1
  Set-Content (Join-Path $q 'heartbeat.txt') "alive=$(Get-Date -Format s) pid=$PID running=$(if ($next) { $next.Name } else { '-' })" -Encoding UTF8
  if (-not $next) {
    if ((Get-Date) - $idleSince -gt [TimeSpan]::FromMinutes($IdleMinutes)) { Write-Host "idle $IdleMinutes min - exiting"; break }
    Start-Sleep 2; continue
  }
  $log = Join-Path $q ("logs\" + $next.BaseName + '.log')
  $t = Get-Date
  Write-Host "$(Get-Date -Format s) start $($next.Name)"
  & powershell -NoProfile -ExecutionPolicy Bypass -File $next.FullName *>&1 | Out-File -LiteralPath $log -Encoding utf8
  $secs = [int]((Get-Date) - $t).TotalSeconds
  # 0.2.1: a job that hit PowerShell errors is not a clean PASS, whatever it printed
  $errs = @(Select-String -LiteralPath $log -Pattern 'FullyQualifiedErrorId' -SimpleMatch -ErrorAction SilentlyContinue).Count
  if ($errs) { Add-Content -LiteralPath $log -Value "== QUEUE: WARNING - $errs PowerShell error(s) above; treat this job as FAILED until checked" -Encoding utf8; Write-Host "  $($next.Name): $errs PowerShell error(s)" }
  Add-Content -LiteralPath $log -Value "== QUEUE: finished $(Get-Date -Format s) after $secs s, exit $LASTEXITCODE" -Encoding utf8
  Move-Item -LiteralPath $next.FullName -Destination (Join-Path $q ("done\" + $next.Name)) -Force
  Write-Host "$(Get-Date -Format s) done  $($next.Name) ($secs s)"
  $idleSince = Get-Date
}
