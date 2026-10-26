# CogBox - the PS1's own command line for the cases in a cases file (see settings_test.cpp), printed in the same form.
# The PS1 (ref/launcher.ps1) is not changed: its data tables, Can / Exe-For / Runtime-For, the per-game part of
# Update-ForGame, Save-Per / Save-Settings and the Play button's argument code are cut out of it by text markers and
# run here with the WinForms controls replaced by plain objects (.Checked / .SelectedIndex / .Enabled / .Value) and
# the file system, exe scans and display queries replaced by the case's answers.
#   pwsh ps1_args.ps1 <cases file> [path to launcher.ps1]
param([Parameter(Mandatory)][string]$CasesFile, [string]$Ps1 = '')
$ErrorActionPreference = 'Stop'
if (-not $Ps1) { $Ps1 = [IO.Path]::Combine($PSScriptRoot, '..', 'ref', 'launcher.ps1') }
$src = [IO.File]::ReadAllLines($Ps1)

# ---- Cutting the PS1 up -----------------------------------------------------------------------------------------------
function Find-Line([string]$pattern, [int]$from = 0) {
  for ($i = $from; $i -lt $src.Count; $i++) { if ($src[$i] -match $pattern) { return $i } }
  throw "launcher.ps1: no line matches $pattern"
}
function Cut([string]$from, [string]$to, [int]$after = 0, [switch]$Exclusive) {
  $a = Find-Line $from $after; $b = Find-Line $to $a
  if ($Exclusive) { $b-- }
  return ($src[$a..$b] -join "`n")
}
$parts = [ordered]@{
  names     = Cut '^\$gameNames = ' '^\$engineIni = '
  defaults  = Cut '^\$defaults = \[ordered\]' '^\$s = \[ordered\]@\{\}'
  perkeys   = Cut '^\$perKeys = ' '^\$per = @\{\}'
  lists     = Cut '^\$verNames = ' '^\}' (Find-Line '^\$verNames = ')
  aanames   = Cut '^\$aaNames = ' '^\$aaNames = '
  gpu       = Cut '^\$gpuMain = ' '^\$fpsCaps = '
  tables    = Cut '^\$presetNames = ' '^\$deadValue = '
  runtime   = Cut '^function Runtime-For' '^\}'
  g2edits   = Cut '^\$g2Edits0127 = ' '^\$g2Edits0127 = '
  can       = Cut '^function Not-Yet' '^\}' (Find-Line '^function Not-Yet')
  exefor    = Cut '^function Exe-For' '^\}'
  saveper   = Cut '^function Save-Per' '^\}'
  savesets  = Cut '^function Save-Settings' '^\}'
}
$uf = Find-Line '^function Update-ForGame'
$parts.forgame = Cut '^\s+\$names = @\(\$startNamesG2' '^\s+\$cShow\.SelectedIndex = @' $uf
$parts.setcan = Cut '^\s+Set-Can \$cStart' '^\s+\$tip\.SetToolTip\(\$cPlayers' $uf
$pl = Find-Line '^\$play\.Add_Click'
$parts.play = Cut '^\s+\$g = \$cGame\.SelectedIndex$' '^\s+Set-Content \(Join-Path \$logDir' $pl -Exclusive
$blocks = @{}; foreach ($k in $parts.Keys) { $blocks[$k] = [scriptblock]::Create($parts[$k]) }

# ---- Stubs ------------------------------------------------------------------------------------------------------------
Add-Type -TypeDefinition @'
public static class GearsDisplay { public static int Hz = 0; public static int RefreshHz(string deviceName) { return Hz; } }
namespace Windows.Forms {
  public class Screen {
    public string DeviceName;
    public Screen(string n) { DeviceName = n; }
    public static Screen PrimaryScreen { get { return new Screen("DISPLAY1"); } }
  }
}
'@
function Join-Path([string]$Path, [string]$ChildPath) { "$Path\$ChildPath" }   # Windows paths on any OS
function Split-Path([Parameter(Position = 0)][string]$Path, [switch]$Leaf) {
  $i = $Path.LastIndexOf('\'); if ($Leaf) { $Path.Substring($i + 1) } else { $Path.Substring(0, $i) }
}
function Test-Path([Parameter(Position = 0)][string]$Path, [string]$LiteralPath) { if ($LiteralPath) { $Path = $LiteralPath }; $script:exist.Contains($Path) }
function Exe-Has([string]$exe, [string]$text) {
  if ($exe -like '*.dll') { return [bool]($script:rtNeedles -contains $text) }
  return [bool]($script:exeNeedles -contains $text)
}
function Get-Content([Parameter(Position = 0)][string]$Path) { $script:extraLines }
function Get-Date([string]$Format) { '20260101-000000' }
function New-Item { }
function Set-Content { }
function Add-Content { }
function Get-Process { }
function Check-GameFiles([int]$g, [bool]$withUpdate) { return @{ ok = $true; lines = @() } }
function Set-Can($c, [string]$why) { $c.Enabled = -not $why }
function New-Ctl { [pscustomobject]@{ Enabled = $true; Checked = $false; SelectedIndex = 0; Value = [decimal]0; Items = [System.Collections.ArrayList]::new() } }
$tip = [pscustomobject]@{}; $tip | Add-Member ScriptMethod SetToolTip { param($c, $t) }
$script:tips = @{}

$alias = @{ fif = 'gow2_renderer_frames_in_flight'; ups = 'gow2_upscale_mode'; fg = 'gow2_frame_generation'; reflex = 'gow2_reflex'
            al2 = 'gow2_antilag2'; aspect = 'gow3_aspect'; edits = "'file|section|~=old>new'"; avx = 'gn_cpu_level=x86-64-v'
            vs = 'd3d12_present_vsync'; lat = 'd3d12_max_frame_latency'
            fsr = 'amd_fidelityfx_loader_dx12.dll'; dlss = 'nvngx_dlss.dll'; xess = 'libxess.dll'; fgdll = 'amd_fidelityfx_framegeneration_dx12.dll' }
$envKeys = 'game', 'upd', 'hz', 'screens', 'gpus', 'extra', 'exe', 'rt', 'files', 'rtfile'
# Case ids of the per-game boxes -> the PS1's $s keys
$ownKeys = @{ Start = @('StartG2', 'StartG3', 'StartGJ'); Version = @('Version', 'G3Version', $null); Aniso = @('Aniso', 'G3Aniso', 'GJAniso')
              FpsMode = @('FpsMode', 'G3FpsMode', 'GJFpsMode') }
$out = [System.Text.StringBuilder]::new()
$n = 0
foreach ($line in [IO.File]::ReadAllLines($CasesFile)) {
  if (-not $line) { continue }
  $n++
  $kv = [ordered]@{}
  foreach ($p in $line.Split(';')) { $i = $p.IndexOf('='); if ($i -gt 0) { $kv[$p.Substring(0, $i)] = $p.Substring($i + 1) } }
  $gIdx = [int]$kv['game']; $upd = $kv['upd'] -eq '1'
  $split = { param($v, $c) if ($v) { @($v.Split($c) | ForEach-Object { if ($alias.ContainsKey($_)) { $alias[$_] } else { $_ } }) } else { @() } }
  $script:exeNeedles = @(& $split $kv['exe'] ',' | ForEach-Object { if ($_ -eq 'mute') { "$(@('gow2', 'gow3', 'gowj')[$gIdx])_mute_unfocused" } else { $_ } })
  $script:rtNeedles = @(& $split $kv['rt'] ',')
  $fileNames = @(& $split $kv['files'] ',')
  $rtFile = if ($kv.Contains('rtfile')) { $kv['rtfile'] -eq '1' } else { $script:rtNeedles.Count -gt 0 }
  $script:extraLines = if ($kv.Contains('extra')) { @($kv['extra'].Split('|')) } else { @() }

  # The PS1's paths (its lines 7-41)
  $root = 'C:\P\Gears2-Native'; $docs = 'C:\P'
  $exeDisc = "$root\build\RelWithDebInfo\gow2.exe"; $exeTu6 = "$root\build\RelWithDebInfo-tu6\gow2.exe"; $tu6Dir = "$root\game\tu6\package"
  $g3Root = "$docs\Gears3-Native"; $exeG3 = "$g3Root\build\RelWithDebInfo\gow3.exe"; $exeG3Tu6 = "$g3Root\build\RelWithDebInfo-tu6\gow3.exe"
  $g3Tu6Dir = "$g3Root\game\tu6\package"; $gjRoot = "$docs\GearsJ-Native"; $exeGJ = "$gjRoot\build\RelWithDebInfo\gowj.exe"; $gjTu4Dir = "$gjRoot\game\tu4\package"
  $script:exist = [System.Collections.Generic.HashSet[string]]::new()
  foreach ($e in $exeDisc, $exeG3, $exeGJ) { [void]$script:exist.Add($e) }
  if ($upd) { foreach ($e in $exeTu6, $exeG3Tu6, "$tu6Dir\default.xexp", "$g3Tu6Dir\default.xexp", "$gjTu4Dir\default.xexp") { [void]$script:exist.Add($e) } }
  foreach ($e in $exeDisc, $exeTu6, $exeG3, $exeG3Tu6, $exeGJ) {
    $d = Split-Path $e
    foreach ($f in $fileNames) { [void]$script:exist.Add("$d\$f") }
    if ($rtFile) { [void]$script:exist.Add("$d\rexruntime.dll") }
  }
  if ($kv.Contains('extra')) { [void]$script:exist.Add("$root\out\extra-args.txt") }
  $tu6Ready = (Test-Path $exeTu6) -and (Test-Path "$tu6Dir\default.xexp")
  $g3Ready = $true; $g3Tu6Ready = (Test-Path $exeG3Tu6) -and (Test-Path "$g3Tu6Dir\default.xexp"); $gjReady = $true

  # The PS1's tables and defaults (fresh for each case), the PC
  . $blocks.names; . $blocks.defaults; . $blocks.perkeys; . $blocks.lists; . $blocks.aanames
  $screenCount = if ($kv.Contains('screens')) { [int]$kv['screens'] } else { 1 }
  $screens = @(1..$screenCount | ForEach-Object { [Windows.Forms.Screen]::new("DISPLAY$_") })
  [GearsDisplay]::Hz = [int]$kv['hz']
  $gpus = if ($kv['gpus']) { @($kv['gpus'].Split('/')) } else { @() }
  . $blocks.gpu; . $blocks.tables; . $blocks.runtime; . $blocks.can; . $blocks.exefor; . $blocks.saveper; . $blocks.savesets
  . $blocks.g2edits

  # The case's settings into $s (as loaded from launcher-settings.json)
  foreach ($k in $kv.Keys) {
    if ($envKeys -contains $k) { continue }
    $key = if ($ownKeys.ContainsKey($k)) { $ownKeys[$k][$gIdx] } else { $k }
    if (-not $key) { continue }
    if ($defaults[$key] -is [bool]) { $s[$key] = [int]$kv[$k] -ne 0 } elseif ($key -eq 'Sens') { $s[$key] = [double][decimal]$kv[$k] } else { $s[$key] = [int]$kv[$k] }
  }
  # The window's boxes, made from $s as the PS1 makes them (its lines 315-386 / Load-Per)
  foreach ($c in 'cGame', 'cPreset', 'cStart', 'cVer', 'cPlayers', 'cSkip', 'cCheat', 'cMut', 'cLsel', 'cFull', 'cExcl', 'cWin', 'cMon', 'cShow',
                 'cRes', 'cFps', 'cVsync', 'cAA', 'cScene', 'cFG', 'cAniso', 'cTri', 'cMip', 'cPool', 'cDraw', 'cUW', 'cHDR', 'cPaper', 'cPeak',
                 'cBlur', 'cDoF', 'cFlare', 'cShake', 'cFov', 'cMnk', 'cSens', 'cDead', 'cVib', 'cLag', 'cMute', 'bPre') { Set-Variable $c (New-Ctl) }
  $cGame.SelectedIndex = $gIdx; $cPreset.SelectedIndex = $s.Preset; $cPlayers.SelectedIndex = $s.Players; $cSkip.Checked = $s.SkipLogos
  $cCheat.Checked = $s.Cheats; $cMut.Checked = $s.Mutators; $cLsel.Checked = $s.LevelSelect; $cFull.Checked = $s.Fullscreen; $cExcl.Checked = $s.Exclusive
  $cWin.SelectedIndex = $s.WindowSize; $cMon.SelectedIndex = $s.Monitor; $cRes.SelectedIndex = $s.Resolution; $cFps.SelectedIndex = $s.Fps
  $cVsync.Checked = $s.Vsync; $cAA.SelectedIndex = $s.AAMode; $cScene.SelectedIndex = $s.SceneMode; $cFG.SelectedIndex = $s.FrameGen
  $cTri.Checked = $s.Trilinear; $cMip.Checked = $s.MipFade; $cPool.SelectedIndex = $s.Pool; $cDraw.SelectedIndex = $s.DrawDist; $cUW.Checked = $s.Ultrawide
  $cHDR.Checked = $s.HDR; $cPaper.Value = [decimal]$s.PaperWhite; $cPeak.Value = [decimal]$s.Peak; $cBlur.Checked = $s.MotionBlur; $cDoF.Checked = $s.DoF
  $cFlare.Checked = $s.LensFlares; $cShake.SelectedIndex = $s.Shake; $cFov.Value = [decimal]$s.Fov; $cMnk.Checked = $s.Mnk; $cSens.Value = [decimal]$s.Sens
  $cDead.SelectedIndex = $s.DeadZone; $cVib.Checked = $s.Vibration; $cLag.Checked = $s.LowLag; $cMute.Checked = $s.Mute
  $cExcl.Enabled = $cFull.Checked; $cWin.Enabled = -not $cFull.Checked; $cPaper.Enabled = $cHDR.Checked; $cPeak.Enabled = $cHDR.Checked
  if ($screens.Count -lt 2) { $cMon.Enabled = $false }
  $g = $gIdx
  . $blocks.forgame   # start point, version, aniso, FPS counter of this game
  . $blocks.setcan    # what is greyed
  # Play
  $a = @(); $notes = @()
  . $blocks.play
  [void]$out.AppendLine("CASE $n $line")
  foreach ($x in $a) { [void]$out.AppendLine("ARG $x") }
  foreach ($x in $notes) { [void]$out.AppendLine("NOTE $x") }
  [void]$out.AppendLine('END')
}
[Console]::Out.Write($out.ToString())
