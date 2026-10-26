# Gears3-Native chapter sweep (adapted from Gears2-Native tools/chapter_sweep.ps1, MIT, with thanks): start each
# campaign chapter straight in, run it, take window screenshots, and record whether it stopped (unregistered function,
# crash) or survived. Writes out\sweeps\<stamp>\summary.md. Call in-process from a job that HOLDS the shared PC lock:
#   & tools\chapter_sweep.ps1 -Chapters 0..10 -Seconds 75
# Chapters are EChapterPoint numbers (notes/06: 0 = CHAP_One_Nightmare ... 30 = CHAP_Five_Tower_2). Gears 3 starts a
# chapter with GearGame_P?loadcheckpoint=0?chapter=N (the string is in GearGame.xxx); like Gears 2 it is expected to
# open paused, so A is pressed at swaps 300 and 450 (Resume). Keep one call to ~20 minutes (~12 chapters at 75 s).
param([int[]]$Chapters = @(), [int]$Seconds = 75, [string[]]$Extra = @(), [int]$ShotEvery = 25)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$names = @{}
$tbl = Join-Path $root 'notes\06-chapters.md'
if (Test-Path $tbl) { Get-Content $tbl | ForEach-Object { if ($_ -match '^\| (\d+) \| (CHAP_\S+) \|') { $names[[int]$Matches[1]] = $Matches[2] } } }
if (-not $Chapters) { $Chapters = 0..30 }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $root "out\sweeps\$stamp"; New-Item -ItemType Directory -Force -Path $out | Out-Null
$rows = @()
foreach ($n in $Chapters) {
  $t = Get-Date
  $ex = @("--ue3_launch=`"GearGame_P?loadcheckpoint=0?chapter=$n -nostartupmovies -FIXEDSEED`"", '--gow2_input_script=300:A,450:A',
          '--gow2_input_frames=true', '--input_backend=none') + $Extra
  $r = & (Join-Path $root 'tools\run.ps1') -Seconds $Seconds -ShotEvery $ShotEvery -Extra $ex *>&1
  $last = Get-ChildItem (Join-Path $root 'out\runs') -Directory | Sort-Object Name | Select-Object -Last 1
  $log = Get-Content (Join-Path $last.FullName 'gow3.log') -Raw -ErrorAction SilentlyContinue
  $unreg = [regex]::Matches("$log", 'unregistered function at guest address (0x[0-9A-Fa-f]{8})') | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique
  $crash = Get-ChildItem $last.FullName -Filter 'crash-*.txt' | Select-Object -First 1
  $res = Get-Content (Join-Path $last.FullName 'result.txt') -ErrorAction SilentlyContinue
  $status = if ($res -match 'result=limit') { 'ran to the limit' } elseif ($unreg) { "stopped: unregistered $($unreg -join ',')" } elseif ($crash) { "crashed ($((Get-Content $crash.FullName -TotalCount 1)))" } else { "exited ($res)" }
  $name = if ($names.ContainsKey($n)) { $names[$n] } else { '?' }
  $rows += [pscustomobject]@{ N = $n; Name = $name; Status = $status; Run = $last.Name; Secs = [int]((Get-Date) - $t).TotalSeconds }
  Write-Host ("{0,2} {1,-26} {2}  ({3})" -f $n, $name, $status, $last.Name)
}
$md = @("# Gears 3 chapter sweep $stamp", '', "Each chapter started with --ue3_launch (loadcheckpoint=0?chapter=N), $Seconds s, A at swaps 300/450.", '',
        '| N | Chapter | Result | Run folder |', '|---|---|---|---|')
$md += $rows | ForEach-Object { "| $($_.N) | $($_.Name) | $($_.Status) | out\runs\$($_.Run) |" }
Set-Content (Join-Path $out 'summary.md') $md -Encoding UTF8
Write-Host "summary: $out\summary.md"
