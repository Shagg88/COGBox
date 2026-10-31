# Call it in-process (& tools\run.ps1 ... -Extra @('a','b')): 'powershell -File' passes only the first -Extra item.
# Gears1-Native unattended run, copied from Gears3-Native's tools\run.ps1 (D5): start gow1.exe at Normal priority, let it run for -Seconds, close it, and collect
# the log into out\runs\<stamp>\. Only from a job that has waited for a gap in the Gears 2 queue (D4, D14).
# -Yield: a script block checked every second; when it returns true the game is closed at once (Gears 2 wants the PC).
param([int]$Seconds = 90, [string]$Config = 'RelWithDebInfo', [string]$Gpu = 'xenos', [int]$ShotEvery = 0, [string[]]$Extra = @(), [scriptblock]$Yield = $null, [string]$UserData = '')
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot

# Screenshot of the game window only (its rectangle on screen), saved as PNG. Used when the user is away (D4).
Add-Type -AssemblyName System.Drawing
if (-not ('Gow1Win2' -as [type])) {
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class Gow1Win2 {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
"@
}
[void][Gow1Win2]::SetProcessDPIAware()
function Save-Shot($proc, $file) {
  try {
    $proc.Refresh(); $h = $proc.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { return $false }
    [void][Gow1Win2]::SetForegroundWindow($h)
    $r = New-Object Gow1Win2+RECT; [void][Gow1Win2]::GetClientRect($h, [ref]$r)
    $pt = New-Object Gow1Win2+POINT; [void][Gow1Win2]::ClientToScreen($h, [ref]$pt)
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    if ($w -le 0 -or $hh -le 0) { return $false }
    $bmp = New-Object Drawing.Bitmap $w, $hh
    # The window itself (PW_CLIENTONLY | PW_RENDERFULLCONTENT), so console windows or the taskbar on top of the game
    # don't end up in the shot (job 012). Falls back to copying the screen when that comes back empty (all black).
    $g = [Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc(); $ok = [Gow1Win2]::PrintWindow($h, $hdc, 3); $g.ReleaseHdc($hdc); $g.Dispose()
    $lit = $false
    if ($ok) { foreach ($fx in 0.25, 0.5, 0.75) { foreach ($fy in 0.25, 0.5, 0.75) { $c = $bmp.GetPixel([int]($w * $fx), [int]($hh * $fy)); if ($c.R + $c.G + $c.B -gt 0) { $lit = $true } } } }
    if (-not $lit) { $g = [Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size); $g.Dispose() }
    $bmp.Save($file, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose(); return $true
  } catch { return $false }
}
$exe = Join-Path $root "build\$Config\gow1.exe"
if (-not (Test-Path $exe)) { Write-Host "RUN STOP: $exe missing"; exit 2 }
# A crashed run can leave its gow3.exe alive for a few seconds (crash reporting). Wait up to 30 s for it, then end it,
# but only if it is our own build's gow3.exe.
for ($w = 0; $w -lt 15 -and (Get-Process gow1 -ErrorAction SilentlyContinue); $w++) { Start-Sleep 2 }
Get-Process gow1 -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | ForEach-Object { Write-Host "  ending leftover gow1 pid $($_.Id)"; Stop-Process -Id $_.Id -Force; Start-Sleep 2 }
if (Get-Process gow1 -ErrorAction SilentlyContinue) { Write-Host "RUN STOP: a gow1 (not ours) is running"; exit 3 }
if (Get-Process gow2 -ErrorAction SilentlyContinue) { Write-Host "RUN STOP: Gears 2 (gow2) is running"; exit 3 }
if (Get-Process gow3 -ErrorAction SilentlyContinue) { Write-Host "RUN STOP: Gears 3 (gow3) is running"; exit 3 }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runDir = Join-Path $root "out\runs\$stamp"; New-Item -ItemType Directory -Force -Path $runDir | Out-Null
$user = if ($UserData) { $UserData } else { Join-Path $root 'out\userdata' }; New-Item -ItemType Directory -Force -Path $user | Out-Null
$logFile = Join-Path $runDir 'gow1.log'
# Diagnostics on every run (Shagg 09:21: "as many fixes and diagnostics as possible to each run"). Unknown switches are
# ignored by the SDK (cvar.cpp stashes them), so a switch a renderer version lacks does no harm.
$diag = @(
  '--gow2_frame_times=600',                              # frame-time summary every 600 swaps
  '--gow2_hitch_report_ms=40',                           # every frame over 40 ms, with the part that took the time
  '--gow2_draw_stats=300',                               # draw / copy counts every 300 frames
  '--gow2_frame_shots=60', "--gow2_frame_dir=$(Join-Path $runDir 'frames')",   # our renderer's own frame every 60 frames (job 005 showed the Epic intro before any 300th frame)
  "--gow2_dump_image=$(Join-Path $runDir 'image.bin')"   # the loaded TU5 image, to compare with notes/03 (game data: out\ only)
)
$runArgs = @("--game_data_root=$(Join-Path $root 'game\extracted')", "--update_data_root=$(Join-Path $root 'game\tu5\package')", "--user_data_root=$user", "--log_file=$logFile", '--log_level=info', "--gow2_crash_dir=$runDir") + $diag + $(if ($Gpu) { @("--gpu_plugin=$Gpu") } else { @() }) + $Extra
# One value per switch, the last one wins (jobs 018 / 020, Gears 2 mail 17:58): the SDK's command-line parser (CLI11)
# rejects a single-value switch given twice, prints that only to stderr and drops every switch after it - Gears 2's
# requested --gow2_frame_times / --gow2_frame_shots repeated ours and silently cancelled --gow2_renderer_api=vulkan.
$seen = @{}; $dedup = New-Object System.Collections.Generic.List[string]
for ($ai = $runArgs.Count - 1; $ai -ge 0; $ai--) {
  $a = [string]$runArgs[$ai]; $k = if ($a -match '^(--[^=]+)') { $Matches[1] } else { $a }
  if ($seen.ContainsKey($k)) { Write-Host "  (switch $k given twice - keeping the later value)"; continue }
  $seen[$k] = $true; $dedup.Insert(0, $a)
}
$runArgs = @($dedup)
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
  if ($yielded) { Write-Host "  YIELD: Gears 2 needs the PC - closing gow1 now"; break }
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
