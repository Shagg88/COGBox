# Gears2-Native job runner. Waits for jobs\go.flag, then runs jobs\job.ps1 once.
# - Runs the job at high priority (R13), never while the game (gow2.exe) is running.
# - Writes jobs\last.log (UTF-8) and jobs\last.done, and keeps jobs\heartbeat.txt fresh.
# - Touches nothing outside this project folder.
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$jobs = Join-Path $root 'jobs'
$flag = Join-Path $jobs 'go.flag'
$job  = Join-Path $jobs 'job.ps1'
$log  = Join-Path $jobs 'last.log'
$done = Join-Path $jobs 'last.done'
$beat = Join-Path $jobs 'heartbeat.txt'
New-Item -ItemType Directory -Force -Path $jobs | Out-Null
Write-Host "Gears2-Native job runner started $(Get-Date -Format s). Waiting for jobs; close this window to stop."
while ($true) {
  Set-Content -LiteralPath $beat -Value "alive=$(Get-Date -Format s) pid=$PID" -Encoding UTF8
  if (Test-Path -LiteralPath $flag) {
    Remove-Item -LiteralPath $flag -Force
    $start = Get-Date
    if (Get-Process gow2 -ErrorAction SilentlyContinue) {
      Set-Content -LiteralPath $log -Value "REFUSED: the game is running. Close it and Claude will requeue." -Encoding UTF8
      $code = 'refused'
    } elseif (-not (Test-Path -LiteralPath $job)) {
      Set-Content -LiteralPath $log -Value "REFUSED: jobs\job.ps1 is missing." -Encoding UTF8
      $code = 'nojob'
    } else {
      Write-Host "$(Get-Date -Format s) running job.ps1"
      $p = Start-Process powershell -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-Command',
             "& '$job' *>&1 | Out-File -LiteralPath '$log' -Encoding utf8") -WorkingDirectory $root -PassThru -WindowStyle Hidden
      $null = $p.Handle  # keeps the exit code readable after exit
      try { $p.PriorityClass = 'High' } catch {}
      while (-not $p.HasExited) {
        Set-Content -LiteralPath $beat -Value "alive=$(Get-Date -Format s) pid=$PID job=$($p.Id)" -Encoding UTF8
        Start-Sleep -Seconds 5
      }
      $code = $p.ExitCode
    }
    $secs = [int]((Get-Date) - $start).TotalSeconds
    Set-Content -LiteralPath $done -Value "done=$(Get-Date -Format s) exit=$code seconds=$secs" -Encoding UTF8
    Write-Host "$(Get-Date -Format s) finished: exit=$code in $secs s"
  }
  Start-Sleep -Seconds 3
}
