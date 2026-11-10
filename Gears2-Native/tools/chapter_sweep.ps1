# Gears2-Native chapter sweep: boot each campaign map straight in (--gow2_launch), run it, screenshot, and record whether
# it stopped (unregistered function, crash) or survived. Writes out\sweeps\<stamp>\summary.md. One game at a time (D4).
param([int]$Seconds = 75, [string[]]$Maps = @(), [string[]]$Extra = @(), [switch]$Cpu, [switch]$Tu6)   # -Tu6: the Title Update 6 build with the update mounted (notes/63)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
if ($Cpu) { . (Join-Path $PSScriptRoot 'cpu-report.ps1') }   # every run is also a CPU sample (notes/60)
if (-not $Maps) {
  $Maps = Get-ChildItem (Join-Path $root 'game\extracted\GearGame\CookedXenon') -Filter 'SP_*_P.xxx' | ForEach-Object { $_.BaseName } | Sort-Object
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $root "out\sweeps\$stamp"; New-Item -ItemType Directory -Force -Path $out | Out-Null
$rows = @()
foreach ($m in $Maps) {
  $t = Get-Date
  $ex = @("--gow2_launch=$m", '--input_backend=none') + $Extra
  if ($Cpu) { $ex += @('--gow2_frame_times=600') + (Get-CpuProfileArgs -Start 600 -Top 5); $w = Start-CpuWatch }
  $r = & (Join-Path $root 'tools\run.ps1') -Tu6:$Tu6 -Seconds $Seconds -ShotEvery 25 -Extra $ex *>&1
  $runLine = ($r | Select-String '^RUN ' | Select-Object -First 1).Line
  $last = Get-ChildItem (Join-Path $root 'out\runs') -Directory | Sort-Object Name | Select-Object -Last 1
  $log = Get-Content (Join-Path $last.FullName 'gow2.log') -Raw -ErrorAction SilentlyContinue
  $unreg = [regex]::Matches($log, '(?:unregistered function at guest address|No function registered at) (?:0x)?([0-9A-Fa-f]{8})') | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique
  $crash = Get-ChildItem $last.FullName -Filter 'crash-*.txt' | Select-Object -First 1
  $res = Get-Content (Join-Path $last.FullName 'result.txt') -ErrorAction SilentlyContinue
  $status = if ($res -match 'result=limit') { 'ran to the limit' } elseif ($unreg) { "stopped: unregistered $($unreg -join ',')" } elseif ($crash) { 'crashed (report)' } else { "exited ($res)" }
  if ($Cpu) {
    $lp = Join-Path $last.FullName 'gow2.log'
    $ft = Select-String -Path $lp -Pattern 'gow2_frame_times: swaps' -CaseSensitive -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($ft) { Write-Host "    $($ft.Line -replace '^.*?(gow2_frame_times: )', '' -replace ' \| observer.*$', '')" }
    Write-CpuReport -Log $lp -Name "sweep-$stamp-$m" -Watch $w -Brief
  }
  $rows += [pscustomobject]@{ Map = $m; Status = $status; Run = $last.Name; Secs = [int]((Get-Date) - $t).TotalSeconds }
  Write-Host ("{0,-28} {1}  ({2})" -f $m, $status, $last.Name)
}
$md = @("# Chapter sweep $stamp$(if ($Tu6) { ' (Title Update 6 build)' })", '', "Each map booted with --gow2_launch, $Seconds s, scripted pad only.", '', '| Map | Result | Run folder |', '|---|---|---|')
$md += $rows | ForEach-Object { "| $($_.Map) | $($_.Status) | out\runs\$($_.Run) |" }
Set-Content (Join-Path $out 'summary.md') $md -Encoding UTF8
Write-Host "summary: $out\summary.md"
