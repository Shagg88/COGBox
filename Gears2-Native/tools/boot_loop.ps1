# Gears2-Native boot loop: run the game; if it stops on "[FATAL] Call to invalid or unregistered function at guest
# address X", add `X = {}` to the manifest (with the run stamp and the containing function as the comment), rebuild,
# run again. Only this one, mechanical kind of hint is added automatically; anything else stops the loop for a look.
param([int]$MaxRounds = 10, [int]$Seconds = 120)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$manifest = Join-Path $root 'gow2_manifest.toml'
for ($i = 1; $i -le $MaxRounds; $i++) {
  Write-Host "=== round $i  $(Get-Date -Format s)"
  $run = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools\run.ps1') -Seconds $Seconds 2>&1
  $run | Select-Object -First 1 | ForEach-Object { Write-Host "  $_" }
  $last = Get-ChildItem (Join-Path $root 'out\runs') -Directory | Sort-Object Name | Select-Object -Last 1
  $log = Join-Path $last.FullName 'gow2.log'
  $text = Get-Content $log -Raw -ErrorAction SilentlyContinue
  $m = [regex]::Matches($text, 'unregistered function at guest address 0x([0-9A-Fa-f]{8})')
  if ($m.Count -eq 0) {
    Write-Host "  no unregistered-function stop; loop ends. Last 15 log lines:"
    Get-Content $log -Tail 15 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  | $_" }
    $run | Select-String '^\s+!' | Select-Object -First 15 | ForEach-Object { Write-Host "  $($_.Line)" }
    break
  }
  $addr = $m[0].Groups[1].Value.ToUpper()
  if (Select-String -Path $manifest -Pattern "^0x$addr\s*=" -Quiet) { Write-Host "  0x$addr is already hinted - the hint didn't take; loop stops for a look"; break }
  $info = (& python (Join-Path $root 'tools\find_hints.py') $root 2>&1 | Select-String "^0x$addr = \{\}" | Select-Object -First 1)
  $comment = if ($info) { ($info.Line -split '#', 2)[1].Trim() } else { $last.Name }
  Add-Content -LiteralPath $manifest -Value "0x$addr = {}   # $comment"
  Write-Host "  added 0x$addr = {}   # $comment"
  $b = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools\build.ps1') 2>&1
  $b | Select-String 'BUILD RESULT' | ForEach-Object { Write-Host "  $($_.Line)" }
  if (-not ($b | Select-String 'BUILD RESULT ok=True')) { $b | Select-Object -Last 20 | ForEach-Object { Write-Host "  | $_" }; break }
}
