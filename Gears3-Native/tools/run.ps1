# Call it in-process (& tools\run.ps1 ... -Extra @('a','b')): 'powershell -File' passes only the first -Extra item.
# Gears3-Native unattended run (adapted from Gears2-Native tools\run.ps1): start gow3.exe at Normal priority, let it run for -Seconds, close it, and collect
# the log into out\runs\<stamp>\. Only from a job that has waited for a gap in the Gears 2 queue (D4, D14).
# -Yield: a script block checked every second; when it returns true the game is closed at once (Gears 2 wants the PC).
# -Tu6: the Title Update 6 build (build\<Config>-tu6, notes/24) with the extracted update mounted as update:
#   (--update_data_root=game\tu6\package; the SDK applies its default.xexp to the disc's default.xex at load).
param([int]$Seconds = 90, [string]$Config = 'RelWithDebInfo', [string]$Gpu = 'xenos', [int]$ShotEvery = 0, [string[]]$Extra = @(), [scriptblock]$Yield = $null, [switch]$Tu6)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot

# Screenshot of the game window only (its rectangle on screen), saved as PNG. Used when the user is away (D4).
Add-Type -AssemblyName System.Drawing
if (-not ('Gow3Win' -as [type])) {
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class Gow3Win {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
}
[void][Gow3Win]::SetProcessDPIAware()
function Save-Shot($proc, $file) {
  try {
    $proc.Refresh(); $h = $proc.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { return $false }
    [void][Gow3Win]::SetForegroundWindow($h)
    $r = New-Object Gow3Win+RECT; [void][Gow3Win]::GetClientRect($h, [ref]$r)
    $pt = New-Object Gow3Win+POINT; [void][Gow3Win]::ClientToScreen($h, [ref]$pt)
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -le 0 -or $hh -le 0) { return $false }
    $bmp = New-Object Drawing.Bitmap $w, $hh
    $g = [Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size); $g.Dispose()
    $bmp.Save($file, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose(); return $true
  } catch { return $false }
}
$exe = Join-Path $root "build\$Config$(if ($Tu6 -and $Config -notmatch 'tu6') { '-tu6' })\gow3.exe"  # -Config RelWithDebInfo-tu6x stays as given
$tu6Dir = Join-Path $root 'game\tu6\package'
if ($Tu6 -and -not (Test-Path (Join-Path $tu6Dir 'default.xexp'))) { Write-Host "RUN STOP: $tu6Dir\default.xexp missing"; exit 2 }
if (-not (Test-Path $exe)) { Write-Host "RUN STOP: $exe missing"; exit 2 }
# A crashed run can leave its gow3.exe alive for a few seconds (crash reporting). Wait up to 30 s for it, then end it,
# but only if it is our own build's gow3.exe.
for ($w = 0; $w -lt 15 -and (Get-Process gow3 -ErrorAction SilentlyContinue); $w++) { Start-Sleep 2 }
Get-Process gow3 -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | ForEach-Object { Write-Host "  ending leftover gow3 pid $($_.Id)"; Stop-Process -Id $_.Id -Force; Start-Sleep 2 }
if (Get-Process gow3 -ErrorAction SilentlyContinue) { Write-Host "RUN STOP: a gow3 (not ours) is running"; exit 3 }
if (Get-Process gow2 -ErrorAction SilentlyContinue) { Write-Host "RUN STOP: Gears 2 (gow2) is running"; exit 3 }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runDir = Join-Path $root "out\runs\$stamp"; New-Item -ItemType Directory -Force -Path $runDir | Out-Null
$user = Join-Path $root 'out\userdata'; New-Item -ItemType Directory -Force -Path $user | Out-Null
$logFile = Join-Path $runDir 'gow3.log'
$runArgs = @("--game_data_root=$(Join-Path $root 'game\extracted')", "--user_data_root=$user", "--log_file=$logFile", '--log_level=info', "--gow2_crash_dir=$runDir") + $(if ($Tu6) { @("--update_data_root=$tu6Dir") } else { @() }) + $(if ($Gpu) { @("--gpu_plugin=$Gpu") } else { @() }) + $Extra
Set-Content (Join-Path $runDir 'args.txt') ($runArgs -join "`r`n")
$t = Get-Date
$p = Start-Process -FilePath $exe -ArgumentList $runArgs -WorkingDirectory (Split-Path $exe) -PassThru
$null = $p.Handle
try { $p.PriorityClass = 'Normal' } catch {}  # the job runs Below Normal; the game itself runs Normal
$yielded = $false
$peak = 0
$nextShot = $ShotEvery
while (-not $p.HasExited -and ((Get-Date) - $t).TotalSeconds -lt $Seconds) {
  for ($k = 0; $k -lt 2 -and -not $p.HasExited; $k++) { Start-Sleep 1; if ($Yield -and (& $Yield)) { $yielded = $true; break } }
  if ($yielded) { Write-Host "  YIELD: Gears 2 needs the PC - closing gow3 now"; break }
  try { $p.Refresh(); $peak = [Math]::Max($peak, $p.WorkingSet64) } catch {}
  $el = ((Get-Date) - $t).TotalSeconds
  if ($ShotEvery -gt 0 -and $el -ge $nextShot) {
    $f = Join-Path $runDir ('shot-{0:000}s.png' -f [int]$el)
    if (Save-Shot $p $f) { Write-Host "  shot $(Split-Path $f -Leaf)" }
    $nextShot += $ShotEvery
  }
}
$alive = -not $p.HasExited
if ($alive) { $null = $p.CloseMainWindow(); Start-Sleep $(if ($yielded) { 1 } else { 5 }); if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
$secs = [int]((Get-Date) - $t).TotalSeconds
$code = if ($yielded) { 'YIELDED to Gears 2 (closed early)' } elseif ($alive) { 'still running at the time limit (closed)' } else { "exited by itself, code $($p.ExitCode)" }
Write-Host ("RUN {0}: {1} after {2} s, peak working set {3:N0} MB, log {4}" -f $stamp, $code, $secs, ($peak/1MB), $logFile)
Set-Content (Join-Path $runDir 'result.txt') ("result=" + $(if ($yielded) { 'yielded' } elseif ($alive) { 'limit' } else { 'exit' }) + " code=" + $(if ($alive) { '' } else { $p.ExitCode }) + " seconds=$secs") -Encoding UTF8
if (-not (Test-Path $logFile)) { $l = Get-ChildItem (Join-Path (Split-Path $exe) 'logs') -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1; if ($l) { Copy-Item $l.FullName $logFile } }
Get-ChildItem $runDir -Filter 'crash-*.txt' -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  CRASH REPORT $($_.Name)"; Get-Content $_.FullName | Select-Object -First 8 | ForEach-Object { Write-Host "  C $_" } }
if (Test-Path $logFile) {
  $lines = Get-Content $logFile
  Write-Host "  log lines: $($lines.Count)"
  $lines | Select-String -Pattern 'FATAL|fatal|unregistered|Unregistered|exception|Exception|crash|assert|not implemented|Unimplemented' | Select-Object -First 25 | ForEach-Object { Write-Host "  ! $($_.Line)" }
  $lines | Select-Object -Last 25 | ForEach-Object { Write-Host "  | $_" }
}
