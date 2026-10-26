# Gears2-Native harness disk limit (decision D18). Prunes ONLY inside out\harness:
#  - keeps every summary/report text file, anything under a folder containing a 'keep' file, and failing runs
#    (folders containing a 'failed' file) until that file is removed;
#  - keeps the newest 5 run folders of each route (out\harness\<route>\<run>);
#  - deletes RenderDoc (.rdc) and Aftermath (.nv-gpudmp) captures older than 14 days unless kept;
#  - then, if out\harness is still over 50 GB, deletes the oldest unprotected run folders until it fits.
# Prints everything it deletes. -WhatIf lists without deleting.
param([double]$CapGB = 50, [int]$KeepRuns = 5, [int]$CaptureDays = 14, [switch]$WhatIf)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$h = Join-Path $root 'out\harness'
if (-not (Test-Path $h)) { Write-Host "no out\harness yet"; return }
function Protected($dir) { (Test-Path (Join-Path $dir 'keep')) -or (Test-Path (Join-Path $dir 'failed')) }
function Remove-Run($dir, $why) {
  $mb = (Get-ChildItem $dir -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum / 1MB
  Write-Host ("  prune {0} ({1:N0} MB): {2}" -f $dir.Substring($root.Length), $mb, $why)
  if (-not $WhatIf) { Remove-Item -LiteralPath $dir -Recurse -Force }
}
foreach ($route in Get-ChildItem $h -Directory) {
  $runs = Get-ChildItem $route.FullName -Directory | Sort-Object Name -Descending
  $runs | Select-Object -Skip $KeepRuns | Where-Object { -not (Protected $_.FullName) } | ForEach-Object { Remove-Run $_.FullName "older than the newest $KeepRuns runs of $($route.Name)" }
}
Get-ChildItem $h -Recurse -File -Include *.rdc, *.nv-gpudmp -ErrorAction SilentlyContinue |
  Where-Object { $_.LastWriteTime -lt (Get-Date).AddDays(-$CaptureDays) -and -not (Protected $_.DirectoryName) } |
  ForEach-Object { Write-Host "  prune capture $($_.FullName.Substring($root.Length)) (older than $CaptureDays days)"; if (-not $WhatIf) { Remove-Item -LiteralPath $_.FullName -Force } }
$size = { (Get-ChildItem $h -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum / 1GB }
$oldest = Get-ChildItem $h -Directory | ForEach-Object { Get-ChildItem $_.FullName -Directory } | Sort-Object Name | Where-Object { -not (Protected $_.FullName) }
foreach ($d in $oldest) { if ((& $size) -le $CapGB) { break }; Remove-Run $d.FullName "out\harness over $CapGB GB" }
Write-Host ("out\harness: {0:N1} GB (cap {1} GB)" -f (& $size), $CapGB)
