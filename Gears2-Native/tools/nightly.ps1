# Gears2-Native nightly check (decision D21 item 3): build, unit tests, and the Stage 1 comparison on the live
# regression routes (pass = 0 unexplained changes; the same check as job 064). Run it as a queue job - for example a
# job file containing only:  & (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'tools\nightly.ps1')
# - not as a scheduled task (R5). Appends one line per run to out\harness\nightly\history.txt and writes the detail to
# out\harness\nightly\<date>.txt. Routes are human chapter numbers (0 = menu); the live set comes from job 048 (D21).
param([int[]]$Routes = @(23, 20, 5, 14, 11, 24, 6, 10, 9, 0), [int]$Seconds = 85)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
try { (Get-Process -Id $PID).PriorityClass = 'High' } catch {}
$base = Join-Path $root 'out\harness\nightly'
New-Item -ItemType Directory -Force -Path $base | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmm'
$detail = Join-Path $base "$stamp.txt"
$rev = (git rev-parse --short HEAD 2>$null)
$lines = @("nightly $stamp at $rev")
$b = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools\build.ps1') 2>&1
$buildOk = [bool]($b | Select-String 'BUILD RESULT ok=True')
$lines += "build: $(if ($buildOk) { 'ok' } else { 'FAILED' })"
$testsOk = $false
if ($buildOk) {
  $t = Get-ChildItem (Join-Path $root 'build') -Recurse -Filter 'gn_tests.exe' | Sort-Object LastWriteTime | Select-Object -Last 1
  if ($t) { $o = & $t.FullName 2>&1; $testsOk = ($LASTEXITCODE -eq 0); $lines += "unit tests: $(if ($testsOk) { 'ok' } else { 'FAILED' }) | $(($o | Select-String 'assertions|test cases' | Select-Object -Last 1).Line)" }
}
$pad = '--gow2_input_script=300:A,450:A,600:A,750:A,900:A,1200:A'
$pass = 0; $fail = @()
if ($buildOk) {
  foreach ($ch in $Routes) {
    $extra = @('--input_backend=none', $pad, '--gow2_input_frames=true', '--gow2_res_check=60', '--gow2_draw_stats=600')
    if ($ch -gt 0) { $extra = @("--gow2_launch=`"GearGame_P?loadcheckpoint=0?chapter=$($ch - 1) -nostartupmovies -BENCHMARK -FIXEDSEED`"") + $extra; $name = 'ch{0:D2}' -f $ch } else { $name = 'menu' }
    & (Join-Path $root 'tools\run.ps1') -Seconds $Seconds -ShotEvery 0 -Extra $extra *>&1 | Out-Null
    $last = Get-ChildItem (Join-Path $root 'out\runs') -Directory | Sort-Object Name | Select-Object -Last 1
    $log = Join-Path $last.FullName 'gow2.log'
    $n = 0; $unexpl = 0
    Select-String -Path $log -Pattern 'gow2_res_check: frame \d+ .* UNEXPLAINED (\d+)' | ForEach-Object { $n++; $unexpl += [int]$_.Matches[0].Groups[1].Value }
    $crash = Test-Path (Join-Path $last.FullName 'crash*')
    $ok = ($n -gt 0) -and ($unexpl -eq 0) -and (-not $crash)
    if ($ok) { $pass++ } else { $fail += $name }
    $lines += "{0}: {1} | checks {2}, unexplained {3}{4} | run {5}" -f $name, $(if ($ok) { 'PASS' } else { 'FAIL' }), $n, $unexpl, $(if ($crash) { ', CRASH REPORT' } else { '' }), $last.Name
    Select-String -Path $log -Pattern 'gow2_res_check:   unexplained' | Select-Object -First 6 | ForEach-Object { $lines += "    $($_.Line.Substring([Math]::Min(40, $_.Line.Length)))" }
  }
}
$lines | Set-Content $detail
$lines | ForEach-Object { Write-Host "  $_" }
$verdict = if ($buildOk -and $testsOk -and $fail.Count -eq 0) { 'PASS' } else { 'FAIL' }
"{0} {1} {2} | build {3}, tests {4}, routes {5}/{6} pass{7}" -f $stamp, $rev, $verdict, $(if ($buildOk) { 'ok' } else { 'FAILED' }), $(if ($testsOk) { 'ok' } else { 'FAILED' }), $pass, $Routes.Count, $(if ($fail) { " (failed: $($fail -join ', '))" } else { '' }) | Add-Content (Join-Path $base 'history.txt')
Write-Host "NIGHTLY $verdict"
