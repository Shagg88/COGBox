# Call it in-process (& tools\run.ps1 ... -Extra @('a','b')): 'powershell -File' passes only the first -Extra item.
# Gears2-Native unattended run: start gow2.exe at high priority, let it run for -Seconds, close it, and collect
# the log into out\runs\<stamp>\. Unattended only when the user is away or says so (D4); one game at a time; high priority (R13).
param([int]$Seconds = 90, [string]$Config = 'RelWithDebInfo', [string]$Gpu = 'xenos', [int]$ShotEvery = 0, [string[]]$Extra = @(), [switch]$Tu6, [string]$LogLevel = 'info')
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot

# Screenshot of the game window only (its rectangle on screen), saved as PNG. Used when the user is away (D4).
Add-Type -AssemblyName System.Drawing
if (-not ('Gow2Win' -as [type])) {
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class Gow2Win {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
}
[void][Gow2Win]::SetProcessDPIAware()
function Save-Shot($proc, $file) {
  try {
    $proc.Refresh(); $h = $proc.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { return $false }
    [void][Gow2Win]::SetForegroundWindow($h)
    $r = New-Object Gow2Win+RECT; [void][Gow2Win]::GetClientRect($h, [ref]$r)
    $pt = New-Object Gow2Win+POINT; [void][Gow2Win]::ClientToScreen($h, [ref]$pt)
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -le 0 -or $hh -le 0) { return $false }
    $bmp = New-Object Drawing.Bitmap $w, $hh
    $g = [Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size); $g.Dispose()
    $bmp.Save($file, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose(); return $true
  } catch { return $false }
}
# -Tu6 (notes/63): the Title Update 6 build, with the extracted update mounted as update: (default.xexp + TU files).
$exe = Join-Path $root "build\$Config$(if ($Tu6) { '-tu6' })\gow2.exe"
if (-not (Test-Path $exe)) { Write-Host "RUN STOP: $exe missing"; exit 2 }
# A crashed run can leave its gow2.exe alive for a few seconds (crash reporting). Wait up to 30 s for it, then end it,
# but only if it is our own build's gow2.exe.
for ($w = 0; $w -lt 15 -and (Get-Process gow2 -ErrorAction SilentlyContinue); $w++) { Start-Sleep 2 }
Get-Process gow2 -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | ForEach-Object { Write-Host "  ending leftover gow2 pid $($_.Id)"; Stop-Process -Id $_.Id -Force; Start-Sleep 2 }
if (Get-Process gow2 -ErrorAction SilentlyContinue) { Write-Host "RUN STOP: a gow2 (not ours) is running"; exit 3 }
# The SDK loads gow2.toml next to the exe (the F4 settings overlay saves it): its settings would change every test run.
$toml = Join-Path (Split-Path $exe) 'gow2.toml'
if (Test-Path $toml) { Write-Host "  WARNING: $toml exists - its settings apply to this run too: $((Get-Content $toml | Where-Object { $_ -match '=' }) -join '; ')" }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runDir = Join-Path $root "out\runs\$stamp"; New-Item -ItemType Directory -Force -Path $runDir | Out-Null
$user = Join-Path $root 'out\userdata'; New-Item -ItemType Directory -Force -Path $user | Out-Null
$logFile = Join-Path $runDir 'gow2.log'
$runArgs = @("--game_data_root=$(Join-Path $root 'game\extracted')", "--user_data_root=$user", "--log_file=$logFile", "--log_level=$LogLevel", "--gow2_crash_dir=$runDir") + $(if ($Gpu) { @("--gpu_plugin=$Gpu") } else { @() }) + $(if ($Tu6) { @("--update_data_root=$(Join-Path $root 'game\tu6\package')") } else { @() }) + $Extra
Set-Content (Join-Path $runDir 'args.txt') ($runArgs -join "`r`n")
$t = Get-Date
$p = Start-Process -FilePath $exe -ArgumentList $runArgs -WorkingDirectory (Split-Path $exe) -PassThru
$null = $p.Handle
try { $p.PriorityClass = 'High' } catch {}
$peak = 0
$nextShot = $ShotEvery
while (-not $p.HasExited -and ((Get-Date) - $t).TotalSeconds -lt $Seconds) {
  Start-Sleep 2
  try { $p.Refresh(); $peak = [Math]::Max($peak, $p.WorkingSet64) } catch {}
  $el = ((Get-Date) - $t).TotalSeconds
  if ($ShotEvery -gt 0 -and $el -ge $nextShot) {
    $f = Join-Path $runDir ('shot-{0:000}s.png' -f [int]$el)
    if (Save-Shot $p $f) { Write-Host "  shot $(Split-Path $f -Leaf)" }
    $nextShot += $ShotEvery
  }
}
$alive = -not $p.HasExited
if ($alive) { $null = $p.CloseMainWindow(); Start-Sleep 5; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
$secs = [int]((Get-Date) - $t).TotalSeconds
$code = if ($alive) { 'still running at the time limit (closed)' } else { "exited by itself, code $($p.ExitCode)" }
Write-Host ("RUN {0}: {1} after {2} s, peak working set {3:N0} MB, log {4}" -f $stamp, $code, $secs, ($peak/1MB), $logFile)
Set-Content (Join-Path $runDir 'result.txt') ("result=" + $(if ($alive) { 'limit' } else { 'exit' }) + " code=" + $(if ($alive) { '' } else { $p.ExitCode }) + " seconds=$secs") -Encoding UTF8
if (-not (Test-Path $logFile)) { $l = Get-ChildItem (Join-Path (Split-Path $exe) 'logs') -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1; if ($l) { Copy-Item $l.FullName $logFile } }
Get-ChildItem $runDir -Filter 'crash-*.txt' -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  CRASH REPORT $($_.Name)"; Get-Content $_.FullName | Select-Object -First 8 | ForEach-Object { Write-Host "  C $_" } }
if (Test-Path $logFile) {
  $lines = Get-Content $logFile
  Write-Host "  log lines: $($lines.Count)"
  $lines | Select-String -Pattern 'FATAL|fatal|unregistered|Unregistered|exception|Exception|crash|assert|not implemented|Unimplemented' | Select-Object -First 25 | ForEach-Object { Write-Host "  ! $($_.Line)" }
  $lines | Select-Object -Last 25 | ForEach-Object { Write-Host "  | $_" }
}
