# Gears2-Native: CPU report helpers for queue jobs (notes/60). Every game run in a job is also a CPU sample:
#   . (Join-Path $root 'tools\cpu-report.ps1')
#   $extra += Get-CpuProfileArgs            # the sampling profiler, 10 s from swap 600
#   $w = Start-CpuWatch                     # before tools\run.ps1
#   ... run ...
#   Write-CpuReport -Log $log -Name "$job-$tag" -Watch $w
# Writes the full profile to jobs\queue\logs\<Name>-profile.log and a short summary to the job log.

function Get-CpuProfileArgs([int]$Start = 600, [int]$Seconds = 10, [int]$Top = 20) {
  return @("--gow2_profile=$Seconds", "--gow2_profile_start=$Start", "--gow2_profile_top=$Top")
}

function Start-CpuWatch {
  $w = @{}
  Get-Process | ForEach-Object { try { $w[$_.Id] = $_.TotalProcessorTime.TotalMilliseconds } catch {} }
  return @{ Cpu = $w; Start = Get-Date }
}

function Write-CpuReport([string]$Log, [string]$Name, $Watch, [switch]$Brief) {
  $root = Split-Path -Parent $PSScriptRoot
  $secs = ((Get-Date) - $Watch.Start).TotalSeconds
  # Everything else that used the CPU during the run (Defender, Search, the other project's build ...).
  $others = Get-Process | Where-Object { $_.ProcessName -ne 'gow2' } | ForEach-Object {
    $b0 = $Watch.Cpu[$_.Id]
    if ($b0 -ne $null) { try { [pscustomobject]@{ N = $_.ProcessName; Ms = $_.TotalProcessorTime.TotalMilliseconds - $b0 } } catch {} }
  } | Where-Object { $_.Ms -gt 0 } | Sort-Object Ms -Descending
  $sum = ($others | Measure-Object Ms -Sum).Sum
  Write-Host ("    cpu: other processes used {0:N1} cores on average over {1:N0} s; top: {2}" -f ($sum / 1000 / [Math]::Max(1, $secs)), $secs,
    (($others | Select-Object -First 5 | ForEach-Object { "{0} {1:N1} s" -f $_.N, ($_.Ms / 1000) }) -join ', '))
  if (-not (Test-Path $Log)) { Write-Host "    cpu: no game log"; return }
  $p = @(Select-String -Path $Log -Pattern 'gow2_profile:' -CaseSensitive | ForEach-Object { $_.Line -replace '^.*?(gow2_profile:)', '$1' })
  if ($p.Count) {
    $p | Set-Content -LiteralPath (Join-Path $root "jobs\queue\logs\$Name-profile.log") -Encoding utf8
    $p | Where-Object { $_ -match 'window \|' } | ForEach-Object { Write-Host "    cpu: $($_.Substring(0, [Math]::Min(200, $_.Length)))" }
    $threads = @($p | Where-Object { $_ -match '^gow2_profile: thread +\d+ .* CPU +([\d.]+)%' } | Select-Object -First $(if ($Brief) { 3 } else { 6 }))
    $threads | ForEach-Object { Write-Host "    cpu: $($_.Substring(0, [Math]::Min(160, $_.Length)))" }
    foreach ($t in ($threads | Select-Object -First $(if ($Brief) { 1 } else { 4 }))) {
      if ($t -match 'thread +(\d+) ') {
        $id = $Matches[1]
        $p | Where-Object { $_ -match "\[$id self\]" } | Select-Object -First $(if ($Brief) { 3 } else { 5 }) | ForEach-Object { Write-Host "    cpu:   $($_.Substring(0, [Math]::Min(170, $_.Length)))" }
      }
    }
  } else { Write-Host "    cpu: no profile in this run (ended before the profiling window?)" }
  if ($Brief) { return }
  Select-String -Path $Log -Pattern 'gow2_renderer: frames \d+-\d+: CPU split' -CaseSensitive | Select-Object -Last 1 | ForEach-Object {
    $l = $_.Line -replace '^.*?(gow2_renderer)', '$1'; Write-Host "    cpu: $($l.Substring(0, [Math]::Min(400, $l.Length)))" }
}
