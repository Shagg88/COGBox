# CogBox: build the "normal game code" exes the launcher falls back to after repeated crashes of the faster build (cg050).
# Run on the PC from a clean checkout, before package_cogbox.ps1:
#   powershell -File Gears2-Native\cogbox\tools\build_normal_code.ps1 [-Games gears1,gears2,gears3,judgment]
# Each game's title-update exe ships twice: <exe> (the normal release build, made with its manifest's faster recompiler
# option) and <stem>-normal.exe (this script: the same commit with that one option set to false). For each game it
#   1. sets the option to false in the game's manifest (the file in the checkout, for the length of that build only),
#   2. runs the game's own tools\build.ps1 -Config Release (-Tu6 for Gears 2 / 3), as the release build does,
#   3. leaves the exe in build\<folder>-normal\ beside the release build folder, with normal-code.txt (commit + option),
#   4. puts the manifest back (git checkout) and touches it, so the next normal build makes the faster code again.
# Gears 2's build.ps1 has no -Name: its normal exe is built in build\Release-tu6, moved to build\Release-tu6-normal, and
# the faster exe is built again at the end so build\Release-tu6 never holds the normal code.
# Nothing is deleted: an older -normal folder is moved to build\_old-files\. Stops on a manifest with uncommitted changes.
param([string]$Games = 'gears1,gears2,gears3,judgment')
$ErrorActionPreference = 'Continue'
$docs = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSCommandPath)))   # the project folder
# The faster option each game's manifest turns on (cg049 94629a5): Gears 3 TU6 cr_as_local, the others non_volatile_as_local.
$all = [ordered]@{
  'gears1'   = @{ 'proj' = 'Gears1-Native'; 'man' = 'gow1_manifest.toml';     'args' = @('-Config', 'Release');         'bdir' = 'Release';     'exe' = 'gow1.exe'; 'opt' = 'non_volatile_as_local'; 'name' = $true }
  'gears2'   = @{ 'proj' = 'Gears2-Native'; 'man' = 'gow2_manifest_tu6.toml'; 'args' = @('-Config', 'Release', '-Tu6'); 'bdir' = 'Release-tu6'; 'exe' = 'gow2.exe'; 'opt' = 'non_volatile_as_local'; 'name' = $false }
  'gears3'   = @{ 'proj' = 'Gears3-Native'; 'man' = 'gow3_manifest_tu6.toml'; 'args' = @('-Config', 'Release', '-Tu6'); 'bdir' = 'Release-tu6'; 'exe' = 'gow3.exe'; 'opt' = 'cr_as_local';           'name' = $true }
  'judgment' = @{ 'proj' = 'GearsJ-Native'; 'man' = 'gowj_manifest.toml';     'args' = @('-Config', 'Release');         'bdir' = 'Release';     'exe' = 'gowj.exe'; 'opt' = 'non_volatile_as_local'; 'name' = $true }
}
$head = (git -C $docs rev-parse HEAD 2>$null | Select-Object -First 1)
if (-not $head) { Write-Host 'STOP: not a git checkout'; Write-Host 'RESULT: FAIL'; exit 1 }
$head = $head.Trim()
$fail = @()
function Restore-Manifest([string]$rel) {
  git -C $docs checkout -q -- $rel 2>&1 | ForEach-Object { Write-Host "    $_" }
  (Get-Item (Join-Path $docs $rel)).LastWriteTime = Get-Date   # codegen re-runs when the manifest is newer than its stamp
}
function Run-Build([hashtable]$gd, [string[]]$extra) {
  $bs = Join-Path $docs "$($gd.proj)\tools\build.ps1"
  $o = @(& powershell -NoProfile -ExecutionPolicy Bypass -File $bs @($gd.args + $extra) 2>&1); $code = $LASTEXITCODE
  $o | Select-String -Pattern 'BUILD RESULT|BUILD STOP|PGO' | ForEach-Object { Write-Host "    $($_.Line.Trim())" }
  if ($code -ne 0) { $o | Select-String -Pattern 'error' | Select-Object -First 15 | ForEach-Object { Write-Host "    $_" } }
  return $code
}
foreach ($key in @($Games.Split(',') | ForEach-Object { $_.Trim().ToLower() } | Where-Object { $_ })) {
  if (-not $all.Contains($key)) { Write-Host "STOP: unknown game '$key' (gears1, gears2, gears3, judgment)"; $fail += $key; continue }
  $gd = $all[$key]; $rel = "$($gd.proj)/$($gd.man)"; $manPath = Join-Path $docs $rel
  Write-Host "== $key ($($gd.exe)): $($gd.opt) = false"
  if (@(git -C $docs status --porcelain -- $rel).Count) { Write-Host "  STOP: $rel has uncommitted changes"; $fail += $key; continue }
  $lines = @(Get-Content -LiteralPath $manPath)
  $hits = @($lines | Where-Object { $_ -match "^\s*$($gd.opt)\s*=\s*true\b" })
  if ($hits.Count -ne 1) { Write-Host "  STOP: $rel has $($hits.Count) '$($gd.opt) = true' lines (want 1) - nothing to switch off"; $fail += $key; continue }
  $lines | ForEach-Object { if ($_ -match "^\s*$($gd.opt)\s*=\s*true\b") { "$($gd.opt) = false   # build_normal_code.ps1 (temporary)" } else { $_ } } | Set-Content -LiteralPath $manPath -Encoding utf8
  $bdRoot = Join-Path $docs "$($gd.proj)\build"; $nd = Join-Path $bdRoot "$($gd.bdir)-normal"
  if (Test-Path -LiteralPath $nd) { $old = Join-Path $bdRoot "_old-files\$($gd.bdir)-normal-$(Get-Date -Format yyyyMMdd-HHmmss)"; New-Item -ItemType Directory -Force (Split-Path $old) | Out-Null; Move-Item -LiteralPath $nd $old; Write-Host "  moved the old $nd -> $old" }
  $t0 = Get-Date
  $code = Run-Build $gd $(if ($gd.name) { @('-Name', "$($gd.bdir)-normal") } else { @() })
  $built = Join-Path $bdRoot "$(if ($gd.name) { "$($gd.bdir)-normal" } else { $gd.bdir })\$($gd.exe)"
  $bi = Get-Item -LiteralPath $built -ErrorAction SilentlyContinue
  Restore-Manifest $rel
  if ($code -ne 0 -or -not $bi -or $bi.LastWriteTime -lt $t0) { Write-Host "  FAIL: build exit $code, exe $(if ($bi) { 'not rebuilt' } else { 'missing' })"; $fail += $key; continue }
  if (-not $gd.name) {   # Gears 2: out of build\Release-tu6 into its -normal folder, then the faster exe again
    New-Item -ItemType Directory -Force $nd | Out-Null
    Move-Item -LiteralPath $built (Join-Path $nd $gd.exe)
    Write-Host '  normal exe moved out; building the faster exe again in its own folder'
    $t1 = Get-Date; $code2 = Run-Build $gd @()
    $fi = Get-Item -LiteralPath $built -ErrorAction SilentlyContinue
    if ($code2 -ne 0 -or -not $fi -or $fi.LastWriteTime -lt $t1) { Write-Host "  FAIL: the faster $($gd.exe) did not build again (exit $code2) - build it before packing"; $fail += "$key (faster exe)" }
  }
  $ne = Join-Path $nd $gd.exe
  $sha = (Get-FileHash -Algorithm SHA256 -LiteralPath $ne).Hash.ToLower()
  @("commit=$head", "option=$($gd.opt)=false", "exe=$($gd.exe)", "sha256=$sha", "built=$(Get-Date -Format s)") | Set-Content -LiteralPath (Join-Path $nd 'normal-code.txt')
  Write-Host ("  {0}: {1:N1} min, {2:N0} MB, sha256 {3}" -f $ne, ((Get-Date) - $t0).TotalMinutes, ((Get-Item $ne).Length / 1MB), $sha)
}
if ($fail.Count) { Write-Host "RESULT: FAIL ($($fail -join ', '))"; exit 1 }
Write-Host 'RESULT: PASS'
