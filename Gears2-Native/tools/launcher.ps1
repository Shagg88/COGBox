# Gears2-Native: the launcher for all three ports (MIT). Pick the game and settings, press Play. Started by Launcher.bat.
# Settings are kept in out\launcher-settings.json and passed to the game on its command line - never in a gow2.toml
# next to the exe, which every test run would also pick up.
# Shagg's rules (Gears 3's mails 09:48 / 09:56): every option is offered for every game (Gears 2, Gears 3, Judgment);
# where a game can't do one yet, the option still shows, greyed, and its tooltip says "Not yet in <game>".
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$docs = Split-Path -Parent $root
# ---- The three games -----------------------------------------------------------------------------------------------
# Gears 2 (notes/63): Title Update 6 (0.0.6.5) runs build\RelWithDebInfo-tu6 with the extracted update mounted as
# update:; the disc (0.0.0.5) runs build\RelWithDebInfo. Both read the same game folder and saves.
$exeDisc = Join-Path $root 'build\RelWithDebInfo\gow2.exe'
$exeTu6 = Join-Path $root 'build\RelWithDebInfo-tu6\gow2.exe'
$tu6Dir = Join-Path $root 'game\tu6\package'
$tu6Ready = (Test-Path $exeTu6) -and (Test-Path (Join-Path $tu6Dir 'default.xexp'))
# Gears 3 (user 14:52 on 22 Oct): the Gears3-Native build next door, started read-only from its folder. Its saves and
# logs go into THIS project's out\ folder (out\gears3-userdata, out\play-logs), so nothing is written over there.
# Its Title Update 6 build (its mail 14:43, commit b149e10): build\RelWithDebInfo-tu6\gow3.exe + game\tu6\package.
$g3Root = Join-Path $docs 'Gears3-Native'
$exeG3 = Join-Path $g3Root 'build\RelWithDebInfo\gow3.exe'
$exeG3Tu6 = Join-Path $g3Root 'build\RelWithDebInfo-tu6\gow3.exe'
$g3Tu6Dir = Join-Path $g3Root 'game\tu6\package'
$g3Ready = Test-Path $exeG3
$g3Tu6Ready = (Test-Path $exeG3Tu6) -and (Test-Path (Join-Path $g3Tu6Dir 'default.xexp'))
# Judgment (its Play.bat): GearsJ-Native\build\RelWithDebInfo\gowj.exe, Title Update 4 always applied (update:), memexport
# on by default there. Saves / logs / shaders in THIS project's out\ (out\judgment-userdata ...), like Gears 3.
$gjRoot = Join-Path $docs 'GearsJ-Native'
$exeGJ = Join-Path $gjRoot 'build\RelWithDebInfo\gowj.exe'
# Patch 0139 (frame-rate item 3, a test switch): out\exe-variant.txt holding a build type (e.g. Release) runs each game from
# build\<that>[-tu6] instead of RelWithDebInfo when that exe exists - the faster-build A/B. Delete the file to go back.
# Patch 0140 (Shagg 22:57: everything for all games): Gears 3 and Judgment too.
$exeVariant = ''
try { $vf = Join-Path $root 'out\exe-variant.txt'; if (Test-Path $vf) { $exeVariant = ((Get-Content $vf -TotalCount 1) -as [string]).Trim() } } catch {}
if ($exeVariant -match '^[A-Za-z]+$' -and $exeVariant -ne 'RelWithDebInfo') {
  foreach ($n in 'exeDisc', 'exeTu6', 'exeG3', 'exeG3Tu6', 'exeGJ') {
    $alt = (Get-Variable $n -ValueOnly) -replace '\\RelWithDebInfo', "\$exeVariant"
    if (Test-Path $alt) { Set-Variable $n $alt }
  }
}
$gjTu4Dir = Join-Path $gjRoot 'game\tu4\package'
$gjReady = Test-Path $exeGJ
$lockFile = Join-Path $g3Root 'tools\sharedpc\status.txt'   # the shared test-job lock (read only)
$store = Join-Path $root 'out\launcher-settings.json'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()
# Patch 0131 (Athelas ideas 9 / 10 / 15): Xbox-type controllers through XInput (xinput1_4.dll, Windows 8+; the older
# xinput9_1_0.dll otherwise), whether the window in front is this launcher's, and blanking a word in a file's bytes.
if (-not ('GearsPad' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class GearsPad {
  [StructLayout(LayoutKind.Sequential)]
  public struct State { public uint Packet; public ushort Buttons; public byte LT; public byte RT; public short LX; public short LY; public short RX; public short RY; }
  [DllImport("xinput1_4.dll", EntryPoint = "XInputGetState")] static extern uint Get14(uint i, out State s);
  [DllImport("xinput9_1_0.dll", EntryPoint = "XInputGetState")] static extern uint Get91(uint i, out State s);
  static int dll = 0;
  public static bool Read(uint i, out State s) {
    s = new State();
    try {
      if (dll == 0) { try { return Get14(i, out s) == 0; } catch (DllNotFoundException) { dll = 1; } catch (EntryPointNotFoundException) { dll = 1; } }
      if (dll == 1) return Get91(i, out s) == 0;
    } catch (Exception) { dll = 2; }
    return false;
  }
  [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  public static bool OursInFront() {
    uint pid; GetWindowThreadProcessId(GetForegroundWindow(), out pid);
    return pid == (uint)System.Diagnostics.Process.GetCurrentProcess().Id;
  }
  // WM_CHANGEUISTATE / UIS_CLEAR / UISF_HIDEFOCUS: draw the focus box, which Windows hides until a key is pressed.
  public static void ShowFocus(IntPtr h) { SendMessage(h, 0x0127, (IntPtr)((1 << 16) | 2), IntPtr.Zero); }
  // Same-length blanking ('x') of every case-insensitive match of find in data; zero bytes of find (UTF-16) are kept.
  public static int Blank(byte[] data, byte[] find) {
    int n = 0;
    if (find.Length == 0) return 0;
    for (int i = 0; i + find.Length <= data.Length; i++) {
      int j = 0;
      for (; j < find.Length; j++) {
        int a = data[i + j], b = find[j];
        if (a >= 65 && a <= 90) a += 32;
        if (b >= 65 && b <= 90) b += 32;
        if (a != b) break;
      }
      if (j == find.Length) { for (int k = 0; k < find.Length; k++) if (find[k] != 0) data[i + k] = 120; n++; i += find.Length - 1; }
    }
    return n;
  }
}
'@
}

$gameNames = @('Gears of War 2', 'Gears of War 3', 'Gears of War: Judgment')
$gameShort = @('Gears 2', 'Gears 3', 'Judgment')
$gameReady = @(((Test-Path $exeDisc) -or $tu6Ready), $g3Ready, $gjReady)
$procNames = @('gow2', 'gow3', 'gowj')
$userDirs = @((Join-Path $root 'out\userdata'), (Join-Path $root 'out\gears3-userdata'), (Join-Path $root 'out\judgment-userdata'))
$logPrefix = @('', 'gears3-', 'judgment-')
$engineIni = @('Xe-GearEngine.ini', 'Xbox360-GearEngine.ini', 'Xbox360-GearEngine.ini')

$defaults = [ordered]@{
  Game = 0; Version = 0; Resolution = 0; AA = $false; AAMode = -1; SceneMode = 0; FrameGen = 0; HDR = $false; Ultrawide = $false; Fps = 0; Fullscreen = $false
  Aniso = $true; ShowFps = $false; FpsMode = -1; Mnk = $false; Sens = 1.0; G3Aniso = $false; G3ShowFps = $false; G3FpsMode = -1
  GJFpsMode = 0; PaperWhite = 203; Peak = 1000; Monitor = 0; Exclusive = $false; Players = 0; MotionBlur = $true; DoF = $true
  LensFlares = $true; Fov = 100; StartG2 = 0; StartG3 = 0; G3Version = 1
  # Shagg's options 1-10 and the "every option for every game" rows (launcher patch 0127)
  Preset = 3; SkipLogos = $false; DrawDist = 0; Shake = 0; Trilinear = $true; MipFade = $true; Pool = 1
  # Launcher patch 0128: Shagg's yes to all six extra ideas, Judgment's and Gears 3's options (their mails 20:40 / 20:45)
  Vibration = $true; DeadZone = 0; WindowSize = 0; Cheats = $false; Mutators = $false; LevelSelect = $false; StartGJ = 0; GJAniso = $false; PerGame = $null
  Mute = $false   # launcher patch 0130
  LowLag = $true  # launcher patch 0133 (Shagg 22:10)
  Vsync = $false  # launcher patch 0155 (Shagg 10:05 "yes we can add vsync")
}
$s = [ordered]@{}; foreach ($k in $defaults.Keys) { $s[$k] = $defaults[$k] }
try { if (Test-Path $store) { (Get-Content $store -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { if ($s.Contains($_.Name)) { $s[$_.Name] = $_.Value } } } } catch {}
# Settings per game (idea 1): the graphics / effects / window boxes are remembered for each game separately. PerGame holds
# one set per game ("0", "1", "2"); the top-level values are the last game's (and the start values for a new game).
$perKeys = @('Preset', 'Resolution', 'Fps', 'AAMode', 'Trilinear', 'MipFade', 'Pool', 'DrawDist', 'MotionBlur', 'DoF', 'LensFlares',
             'Shake', 'Fov', 'Ultrawide', 'HDR', 'PaperWhite', 'Peak', 'SkipLogos', 'Players', 'Fullscreen', 'Exclusive', 'WindowSize')
$per = @{}
if ($s.PerGame) { foreach ($gp in $s.PerGame.PSObject.Properties) { $h = @{}; foreach ($q in $gp.Value.PSObject.Properties) { $h[$q.Name] = $q.Value }; $per[$gp.Name] = $h } }

$verNames = @('Title Update (latest)', 'Disc (original release)')
$resNames = @('1x - 720p (original)', '2x - 1440p', '3x - 4K', '4x - 5K (supersampling, heavy)', 'Match my monitor (native)')
$playerNames = @('1', '2 (split-screen)', '3 (split-screen)', '4 (split-screen)')
# Start at (Shagg 10:34): the main menu, or straight into a chapter from its beginning (the level select every test run
# uses: GearGame_P?loadcheckpoint=0?chapter=<index>). Gears 2: 29 chapters (notes/32). Gears 3: EChapterPoint 0-30 (its
# chat 10:48). The chapter opens paused: press A / Start. Saves are not changed.
$startNamesG2 = @('Main menu (normal)') + @(1..29 | ForEach-Object { "Chapter $_" })
$startNamesG3 = @('Main menu (normal)') + @('Act I - Nightmare (prologue)', 'Act I - Raven''s Nest 1', 'Act I - Raven''s Nest 2', 'Act II - Hanover 1', 'Act II - Hanover 2', 'Act II - Hanover 3', 'Act II - Bridge', 'Act II - Wreckage', 'Act III - Badlands 1', 'Act III - Badlands 2', 'Act III - Badlands 3', 'Act III - Badlands 4', 'Act III - Blimp 1', 'Act III - Blimp 2', 'Act III - Fort 1', 'Act III - Fort 2', 'Act IV - Convoy', 'Act IV - Mercy 1', 'Act IV - Mercy 2', 'Act IV - Char 1', 'Act IV - Char 2', 'Act IV - Char 3', 'Act IV - Shipyard 1', 'Act IV - Shipyard 2', 'Act V - Maelstrom', 'Act V - Coast 1', 'Act V - Coast 2', 'Act V - Azura 1', 'Act V - Azura 2', 'Act V - Tower 1', 'Act V - Tower 2')
# Judgment (its chat 20:52; maps in play order, all reached gameplay in its jobs 013/014; names by place, not the game's
# own chapter titles): --ue3_launch="<map> -nostartupmovies".
$mapsGJ = @('SP_00_Museum_P', 'SP_01_University_P', 'SP_02_Mansion_P', 'SP_02_Mansion_3_P', 'SP_03_Island_P', 'SP_03_to_04_P',
            'SP_04_Rooftops_P', 'SP_04_Rooftops2_P', 'SP_05_Courthouse_P', 'SP_05_CourthouseTrans_P', 'SP_05_CourthouseBoss_P',
            'SPE_E1_P', 'SPE_E2_P', 'SPE_E2oilrig_P', 'SPE_E3_P', 'SPE_E3_Outro_P', 'SPE_E4_P', 'SPE_E5_P')
$startNamesGJ = @('Main menu (normal)') + @('Prologue (museum / trial)', 'Sera University', 'Onyx Mansion', 'Onyx Mansion (part 3)',
  'Halvo Bay military base', 'Halvo Bay - to the rooftops', 'Rooftops', 'Rooftops (part 2)', 'Courthouse', 'Courthouse (transition)',
  'Courthouse - boss', 'Aftermath 1', 'Aftermath 2', 'Aftermath 2 - oil rig', 'Aftermath 3', 'Aftermath 3 - outro', 'Aftermath 4', 'Aftermath 5')
function Launch-Url([int]$g, [int]$chapter) {   # chapter = 0-based index into the game's list
  if ($g -eq 2) { return "$($mapsGJ[$chapter]) -nostartupmovies" }
  return "GearGame_P?loadcheckpoint=0?chapter=$chapter -nostartupmovies"
}
# Monitor: the SDK's --monitor is 0 = default (the main screen), 1..N = its N-th display (SDL's order).
$screens = @([System.Windows.Forms.Screen]::AllScreens)
$monNames = @('Default (main screen)') + @(for ($i = 0; $i -lt $screens.Count; $i++) { "{0}: {1}x{2}{3}" -f ($i + 1), $screens[$i].Bounds.Width, $screens[$i].Bounds.Height, $(if ($screens[$i].Primary) { ' (main)' }) })
# Anti-aliasing (notes/64): SMAA on the final image, or DLSS / FSR / XeSS on the 3D scene at the render resolution. Each
# is used only when its DLL is next to the game's exe; otherwise SMAA, and args.txt says why.
$aaNames = @('Off', 'SMAA', 'FSR', 'DLSS (NVIDIA RTX)', 'XeSS')   # Gears 3 (its 20:40 mail): clean in ch8 with all three
$gpus = @(try { Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object { [string]$_.Name } } catch { @() })
$gpuMain = @($gpus | Where-Object { $_ -match 'NVIDIA|GeForce|Quadro|AMD|Radeon|Intel.*Arc' } | Select-Object -First 1) + @($gpus | Select-Object -First 1) | Where-Object { $_ } | Select-Object -First 1
$hasRtx = @($gpus | Where-Object { $_ -match 'RTX' }).Count -gt 0
if ([int]$s.AAMode -lt 0) { $s.AAMode = $(if ($s.AA) { 1 } else { 0 }) }
$fpsNames = @('30 (original)', '60', '120', 'Unlimited')
$fpsShowNames = @('Off', 'Bar at the top', 'Bar at the bottom', "Game's own (column)")
if ([int]$s.FpsMode -lt 0) { $s.FpsMode = $(if ($s.ShowFps) { 3 } else { 0 }) }
if ([int]$s.G3FpsMode -lt 0) { $s.G3FpsMode = $(if ($s.G3ShowFps) { 3 } else { 0 }) }
$fpsCaps = @(0, 60, 120, -1)
$presetNames = @('Original (as on the console)', 'High', 'Ultra', 'Custom')
# Draw distance (Gears 3 / Judgment [SystemSettings] MaxDrawDistanceScale, game 1; Gears 3's job 051 tried 4).
$drawNames = @('Original', 'Far (1.5x)', 'Farthest (2x)')
$drawScale = @('1', '1.5', '2')
# Roadie-run camera shake: the game's RoadieRunShakyCamDampeningFactor (Gears 2 GearCamera.ini 0.4, Gears 3 / Judgment
# GearPawn*.ini 0.6). [G] A dampening factor of 1 should mean full damping (no shake); not yet checked by eye.
$shakeNames = @('Original', 'Less', 'Off')
$shakeValue = @('', '0.8', '1.0')
# Texture streaming pool (Gears 2 --gow2_patch_texture_pool_mb, Gears 3 --gow3_texture_pool_mb). Job 430: Gears 2 at
# 196 MB used 265-279 of the 512 MB, at 320 MB 389 MB in ch15 (no crash; no long play test yet).
$poolNames = @("Game's own (smallest)", 'Standard (196 MB / Gears 3 200 MB)', 'Large (256 MB)', 'Larger (320 MB, not long-tested)')
$poolMb = @(0, -1, 256, 320)   # -1 = the port's own default (Gears 2 196, Gears 3 200; Judgment's default is the game's 136, so 200 is sent)
# Window size when not fullscreen (idea 3; the SDK's --window_width / --window_height, logical pixels).
$winNames = @('Default', '1280 x 720', '1600 x 900', '1920 x 1080', '2560 x 1440', '3840 x 2160')
$winSizes = @($null, @(1280, 720), @(1600, 900), @(1920, 1080), @(2560, 1440), @(3840, 2160))
# Stick dead zone (the SDK's --left/right_stick_deadzone_percentage, a fraction; 0 = only the game's own dead zone).
$deadNames = @("Game's own", '5%', '10%', '15%', '20%', '25%')
$deadValue = @(0, 0.05, 0.10, 0.15, 0.20, 0.25)

# ---- What each game can do today (null = yes; text = the reason it is greyed) --------------------------------------
$checkStore = Join-Path $root 'out\launcher-exe-checks.json'
$script:checks = @{}
try { if (Test-Path $checkStore) { (Get-Content $checkStore -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $script:checks[$_.Name] = [bool]$_.Value } } } catch {}
# Patch 0134: does this CPU (and Windows) support AVX2? Same answer the game's own start-up check gives.
if (-not ('GearsDisplay' -as [type])) {   # launcher patch 0155: GearsDisplay is the newest type in this block
  Add-Type -TypeDefinition @'
public static class GearsDisplay {
  // Launcher patch 0155: the refresh rate a screen shows now (EnumDisplaySettings, ENUM_CURRENT_SETTINGS); 0 = unknown.
  [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential, CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
  struct DEVMODE {
    [System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
    public short dmSpecVersion; public short dmDriverVersion; public short dmSize; public short dmDriverExtra; public int dmFields;
    public int dmPositionX; public int dmPositionY; public int dmDisplayOrientation; public int dmDisplayFixedOutput;
    public short dmColor; public short dmDuplex; public short dmYResolution; public short dmTTOption; public short dmCollate;
    [System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
    public short dmLogPixels; public int dmBitsPerPel; public int dmPelsWidth; public int dmPelsHeight; public int dmDisplayFlags;
    public int dmDisplayFrequency; public int dmICMMethod; public int dmICMIntent; public int dmMediaType; public int dmDitherType;
    public int dmReserved1; public int dmReserved2; public int dmPanningWidth; public int dmPanningHeight;
  }
  [System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
  static extern bool EnumDisplaySettings(string deviceName, int modeNum, ref DEVMODE devMode);
  public static int RefreshHz(string deviceName) {
    try { DEVMODE d = new DEVMODE(); d.dmSize = (short)System.Runtime.InteropServices.Marshal.SizeOf(typeof(DEVMODE));
          if (EnumDisplaySettings(deviceName, -1, ref d) && d.dmDisplayFrequency > 1) return d.dmDisplayFrequency; } catch { }
    return 0;
  }
}
public static class GearsCpu {
  [System.Runtime.InteropServices.DllImport("kernel32.dll")] static extern bool IsProcessorFeaturePresent(uint feature);
  public static bool HasAvx2() { try { return IsProcessorFeaturePresent(40); } catch { return true; } }
}
'@
}
function Runtime-For([string]$exe) {   # launcher patch 0155: the SDK runtime DLL next to a game's exe (SDK switches live there)
  @('rexruntimerd.dll', 'rexruntime.dll' | ForEach-Object { Join-Path (Split-Path $exe) $_ } | Where-Object { Test-Path $_ }) | Select-Object -First 1
}
function Exe-Has([string]$exe, [string]$text) {
  # Cached per exe and its file time, so a rebuilt exe is looked at again and an unchanged one costs nothing.
  try { $key = "$exe|$((Get-Item $exe).LastWriteTimeUtc.Ticks)|$text" } catch { return $false }
  if ($script:checks.ContainsKey($key)) { return $script:checks[$key] }
  $r = $false; try { $r = [bool](Select-String -Path $exe -Pattern $text -SimpleMatch -Quiet) } catch {}
  $script:checks[$key] = $r
  try { New-Item -ItemType Directory -Force (Split-Path $checkStore) | Out-Null; $script:checks | ConvertTo-Json | Set-Content $checkStore -Encoding UTF8 } catch {}
  return $r
}
$g2Edits0127 = (Test-Path $exeDisc) -and (Exe-Has $exeDisc "'file|section|~=old>new'")   # Gears 2 layer 2 from patch 0127
# ---- Game file fingerprints (patch 0131, Athelas idea 2) ---------------------------------------------------------------
# SHA-1 of the copies these ports are made and tested with (job 440 on Shagg's PC). Another copy still starts if the player
# says so ("Start anyway"); the launcher only warns. Cached per file size and time in out\launcher-file-hashes.json.
$knownFiles = @(
  @{ xex = @{ size = 18595840; sha1 = 'b6860bf31e55013467b53eca963d12314c0d78da' }; tu = @{ size = 2074624; sha1 = '8689bf52b2a47908eb9281d05c5e86653db0cf11'; name = 'Title Update 6' } },
  @{ xex = @{ size = 21938176; sha1 = '2450fd8e4c3cf4d772cd471785d1e38cad3888ad' }; tu = @{ size = 1908736; sha1 = '5c335a50116064d2dc91dce1ec922ec9e1975b62'; name = 'Title Update 6' } },
  @{ xex = @{ size = 22855680; sha1 = '426c5d27ff73d3ff2af9d404adb49e9a3db18bf1' }; tu = @{ size = 1888256; sha1 = '88e876b10276535379d5524f2bef73ed9195870e'; name = 'Title Update 4' } }
)
$hashStore = Join-Path $root 'out\launcher-file-hashes.json'
$script:hashes = @{}
try { if (Test-Path $hashStore) { (Get-Content $hashStore -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $script:hashes[$_.Name] = [string]$_.Value } } } catch {}
function File-Sha1([string]$p) {
  $i = Get-Item -LiteralPath $p
  $key = "$p|$($i.Length)|$($i.LastWriteTimeUtc.Ticks)"
  if ($script:hashes.ContainsKey($key)) { return $script:hashes[$key] }
  $h = (Get-FileHash -Algorithm SHA1 -LiteralPath $p).Hash.ToLower()
  $script:hashes[$key] = $h
  try { New-Item -ItemType Directory -Force (Split-Path $hashStore) | Out-Null; $script:hashes | ConvertTo-Json | Set-Content $hashStore -Encoding UTF8 } catch {}
  return $h
}
# Returns @{ ok = $true/$false; lines = @(...) }: one line per file, "as expected" or what differs.
function Check-GameFiles([int]$g, [bool]$withUpdate) {
  $gameDir = @((Join-Path $root 'game'), (Join-Path $g3Root 'game'), (Join-Path $gjRoot 'game'))[$g]
  $tuDir = @($tu6Dir, $g3Tu6Dir, $gjTu4Dir)[$g]
  $want = $knownFiles[$g]
  $list = @(@{ p = (Join-Path $gameDir 'extracted\default.xex'); k = $want.xex; what = 'the game (default.xex)' })
  if ($withUpdate) { $list += @{ p = (Join-Path $tuDir 'default.xexp'); k = $want.tu; what = "$($want.tu.name) (default.xexp)" } }
  $ok = $true; $lines = @()
  foreach ($e in $list) {
    if (-not (Test-Path -LiteralPath $e.p)) { $ok = $false; $lines += "$($e.what): not found at $($e.p)"; continue }
    try { $h = File-Sha1 $e.p } catch { $ok = $false; $lines += "$($e.what): could not be read ($($_.Exception.Message))"; continue }
    if ($h -eq $e.k.sha1) { $lines += "$($e.what): the expected version (SHA-1 $h)" }
    else { $ok = $false; $lines += "$($e.what): a different version (SHA-1 $h, $((Get-Item -LiteralPath $e.p).Length) bytes; these ports are made for $($e.k.sha1), $($e.k.size) bytes)" }
  }
  return @{ ok = $ok; lines = $lines }
}
function Not-Yet([int]$g, [string]$what = '') { return "Not yet in $($gameShort[$g])$(if ($what) { " ($what)" })." }
function Can([int]$g, [string]$feature) {
  switch ($feature) {
    'version'   { if ($g -eq 0) { if (-not $tu6Ready) { return 'The Title Update 6 build is not there yet.' } } elseif ($g -eq 1) { if (-not $g3Tu6Ready) { return 'The Gears 3 Title Update 6 build is not there yet.' } } else { return 'Judgment always runs with its Title Update 4.' } }
    'start'     { if ($g -eq 2 -and $startNamesGJ.Count -lt 2) { return (Not-Yet 2 'its chapter list is coming') } }
    'gamefps'   { if ($g -eq 2) { return (Not-Yet 2 "game's own counter") } }
    'ultrawide' { if ($g -eq 2) { return (Not-Yet 2) } }
    'cheats'    { if ($g -ne 1) { return (Not-Yet $g) } elseif ($cVer.SelectedIndex -eq 0 -and $g3Tu6Ready) { return 'Gears 3: the Disc version only for now (Title Update 6 shows a dirty-disc error with it).' } }
    'mutators'  { if ($g -ne 1) { return (Not-Yet $g) } elseif ($cVer.SelectedIndex -eq 0 -and $g3Tu6Ready) { return 'Gears 3: the Disc version only for now (Title Update 6 shows a dirty-disc error with it).' } }
    'levelsel'  { if ($g -ne 2) { return (Not-Yet $g) } }
    'prebuild'  { if ($g -eq 2 -and $startNamesGJ.Count -lt 2) { return (Not-Yet 2 'needs its chapter list') } }
    'drawdist'  { if ($g -eq 0) { return (Not-Yet 0 'its config has no draw-distance setting') } }
    'skiplogos' { if ($g -eq 0 -and -not $g2Edits0127) { return (Not-Yet 0 'needs the next Gears 2 build') } }
    'trilinear' { if ($g -eq 0 -and -not $g2Edits0127) { return (Not-Yet 0 'needs the next Gears 2 build') } }
    'mipfade'   { if ($g -eq 0) { return (Not-Yet 0 'its config has no mip-fade setting') } }
    'lowlag'    { if (-not (Exe-Has (Exe-For $g) 'gow2_renderer_frames_in_flight')) { return (Not-Yet $g 'needs its next build') } }
    'scenescale' { if (-not (Exe-Has (Exe-For $g) 'gow2_upscale_mode')) { return (Not-Yet $g 'needs its next build') } }
    'framegen'  { if (-not (Exe-Has (Exe-For $g) 'gow2_frame_generation')) { return (Not-Yet $g 'needs its next build') }
                  if (-not (Test-Path (Join-Path (Split-Path (Exe-For $g)) 'amd_fidelityfx_framegeneration_dx12.dll'))) { return (Not-Yet $g 'this build has no AMD frame generation files') } }
    'mute'      { if (-not (Exe-Has (Exe-For $g) "$($procNames[$g])_mute_unfocused")) { return (Not-Yet $g 'needs its next build') } }
    'vsync'     { $rt = Runtime-For (Exe-For $g); if (-not $rt -or -not (Exe-Has $rt 'd3d12_present_vsync')) { return (Not-Yet $g 'needs its next build (SDK patch 37)') } }   # launcher patch 0155
  }
  return $null
}

# ---- The window -----------------------------------------------------------------------------------------------------
$f = New-Object Windows.Forms.Form
$f.Text = 'Gears of War - Native'; $f.FormBorderStyle = 'FixedDialog'; $f.MaximizeBox = $false; $f.StartPosition = 'CenterScreen'
$f.Font = New-Object Drawing.Font('Segoe UI', 9.5)
$tip = New-Object Windows.Forms.ToolTip; $tip.AutoPopDelay = 20000
$script:tips = @{}   # control -> its own tooltip text (the "Not yet" reason is put in front of it)
$y = 12
$script:panel = $f
function Add-Label([string]$t, [int]$x = 12) { $l = New-Object Windows.Forms.Label; $l.Text = $t; $l.Location = New-Object Drawing.Point $x, ($script:y + 3); $l.AutoSize = $true; $script:panel.Controls.Add($l); return $l }
function Set-Tip($c, [string]$t) { $script:tips[$c] = $t; $tip.SetToolTip($c, $t) }
function Add-Combo([string]$t, [string[]]$items, [int]$sel, [string]$tt = '') {
  $l = Add-Label $t
  $c = New-Object Windows.Forms.ComboBox; $c.DropDownStyle = 'DropDownList'; [void]$c.Items.AddRange($items)
  $c.SelectedIndex = [Math]::Max(0, [Math]::Min($items.Count - 1, $sel)); $c.Location = New-Object Drawing.Point 170, $script:y; $c.Width = 220
  $script:panel.Controls.Add($c); $script:y += 32; if ($tt) { Set-Tip $c $tt; $tip.SetToolTip($l, $tt) }; return $c
}
function Add-Check([string]$t, [bool]$on, [string]$tt = '') {
  $c = New-Object Windows.Forms.CheckBox; $c.Text = $t; $c.Checked = $on; $c.AutoSize = $true; $c.Location = New-Object Drawing.Point 12, $script:y
  $script:panel.Controls.Add($c); $script:y += 26; Set-Tip $c $tt; return $c
}
function Add-Number([string]$t, [double]$min, [double]$max, [double]$step, [double]$val, [string]$tt, [int]$dec = 0) {
  $l = Add-Label $t 30
  $n = New-Object Windows.Forms.NumericUpDown; $n.DecimalPlaces = $dec; $n.Minimum = [decimal]$min; $n.Maximum = [decimal]$max; $n.Increment = [decimal]$step
  $n.Value = [decimal][Math]::Max($min, [Math]::Min($max, $val)); $n.Location = New-Object Drawing.Point 310, $script:y; $n.Width = 80
  Set-Tip $n $tt; $tip.SetToolTip($l, $tt); $script:panel.Controls.Add($n); $script:y += 30; return $n
}

# Top: game and preset (outside the tabs)
$cGame = Add-Combo 'Game' $gameNames ([int]$s.Game)
$cPreset = Add-Combo 'Quality preset' $presetNames ([int]$s.Preset) 'Sets the graphics boxes in one go. Original = the game as on the console. Change any box afterwards and the preset shows Custom.'
$tabs = New-Object Windows.Forms.TabControl; $tabs.Location = New-Object Drawing.Point 8, $y; $tabs.Size = New-Object Drawing.Size 412, 470
$f.Controls.Add($tabs)
function New-Page([string]$t) { $p = New-Object Windows.Forms.TabPage; $p.Text = $t; $p.AutoScroll = $true; $tabs.TabPages.Add($p); $script:panel = $p; $script:y = 10; return $p }

# -- Game tab
[void](New-Page 'Game')
$cStart = Add-Combo 'Start at' $startNamesG2 ([int]$s.StartG2) 'Main menu, or straight into a chapter from its beginning (it opens paused: press A / Start). Your saves are not changed.'
$cVer = Add-Combo 'Game version' $verNames ([int]$s.Version) 'Title Update: the latest patch from Epic (Gears 2: TU6; Gears 3: TU6, new; Judgment: TU4). Disc: the original release.'
$cPlayers = Add-Combo 'Players' $playerNames ([int]$s.Players) 'Local split-screen co-op (one controller each).'
$cSkip = Add-Check 'Skip the intro logos' ([bool]$s.SkipLogos) 'Starts at the title screen without the publisher / studio logo films. The loading film and cutscenes still play.'
$cCheat = Add-Check 'Cheats (hotkeys F5-F11)' ([bool]$s.Cheats) 'The developers'' cheat keys (Gears 3 chat, its notes/25): F5-F11 in game. Achievements and saves are your own business.'
$cMut = Add-Check 'Unlock all mutators' ([bool]$s.Mutators) 'Every mutator (gameplay modifier) available from the start. Writes nothing to your profile.'
$cLsel = Add-Check 'Level select menu (experimental)' ([bool]$s.LevelSelect) 'The game''s own hidden level-select menu (Judgment). Its list has no text yet: Down moves, A loads (entry 1 Sera University ... 17 Aftermath 5).'
$cFull = Add-Check 'Fullscreen (borderless)' ([bool]$s.Fullscreen)
$cExcl = Add-Check 'Exclusive fullscreen instead' ([bool]$s.Exclusive) 'Real fullscreen mode instead of a borderless window (lower latency on some PCs; alt-tab is slower).'
$cWin = Add-Combo 'Window size' $winNames ([int]$s.WindowSize) 'The window''s size when not fullscreen. Default = the game''s own.'
$cFull.Add_CheckedChanged({ $cExcl.Enabled = $cFull.Checked; $cWin.Enabled = -not $cFull.Checked }); $cExcl.Enabled = $cFull.Checked; $cWin.Enabled = -not $cFull.Checked
$cMon = Add-Combo 'Monitor' $monNames ([int]$s.Monitor)
if ($screens.Count -lt 2) { $cMon.Enabled = $false }
$cShow = Add-Combo 'FPS counter' $fpsShowNames ([int]$s.FpsMode) "Bar: one line (FPS, frame time, slowest frame) drawn by the port itself, nothing in the game changed. Game's own: the game's built-in FPS / frame-time column."

# -- Graphics tab
[void](New-Page 'Graphics')
$cRes = Add-Combo 'Render resolution' $resNames ([int]$s.Resolution)
$cFps = Add-Combo 'Frame rate limit' $fpsNames ([int]$s.Fps)
# Launcher patch 0155 (SDK patch 37): the PC's own vsync - each frame waits for the screen's refresh, so no tearing. Not the
# game's Xbox vsync (that one is part of the frame-rate limit above and stays as it is).
$cVsync = Add-Check 'Vsync (no tearing)' ([bool]$s.Vsync) 'Each frame waits for your screen''s next refresh, so the picture never tears. With G-Sync / FreeSync the screen follows the game below its refresh rate. The frame rate is capped a little under your screen''s refresh rate (half of it with frame generation), which keeps the extra input lag small. Off = frames are shown at once (lowest lag, may tear).'
$aaTip = "SMAA: light, sharp, any GPU. FSR: temporal anti-aliasing of the 3D scene, any DX12 GPU. DLSS: the same on NVIDIA RTX cards. XeSS: the same, best on Intel Arc cards.`r`nYour graphics card: $(if ($gpuMain) { $gpuMain } else { 'not detected' })"
$cAA = Add-Combo 'Anti-aliasing' $aaNames ([int]$s.AAMode) $aaTip
# Patch 0142 (frame-rate item 5, upscaling): with FSR / DLSS / XeSS, the 3D scene can be drawn below the render
# resolution and the upscaler fills in the detail. Needs a render resolution the mode divides evenly (Performance:
# 2x or 4x; Quality: 3x); otherwise the game stays native and its log says so.
$sceneNames = @('Native (same as render resolution)', 'Quality (scene at 2/3)', 'Performance (scene at 1/2)', 'Ultra performance (scene at 1/3)')
$cScene = Add-Combo 'Scene resolution' $sceneNames ([int]$s.SceneMode) "With FSR, DLSS or XeSS: draw the 3D scene at a lower resolution and let the upscaler fill in the detail - more frames per second, a little softer. HUD and menus stay at the full resolution.`r`nPerformance needs render resolution 2x (1440p) or 4x; Quality and Ultra performance need 3x (4K)."
$cAA.Add_SelectedIndexChanged({ $cScene.Enabled = $cAA.SelectedIndex -ge 2 -and -not $script:sceneOff })
# Patches 0146 / 0147 (frame-rate item 6): AMD FSR frame generation - one generated frame between each two the game
# draws. Any DirectX 12 graphics card.
$fgNames = @('Off', 'AMD FSR frame generation')
$cFG = Add-Combo 'Frame generation' $fgNames ([int]$s.FrameGen) "Shows a generated frame between each two the game draws: smoother motion on a fast monitor, about one frame more input lag. Works best when the game already runs at 60 fps or more. Any DirectX 12 graphics card.`r`nEarly version: the HUD and characters' own movement can smear a little in the in-between frames." 
$cAniso = Add-Check 'Sharper textures (16x anisotropic)' ([bool]$s.Aniso)
$cTri = Add-Check 'Smooth texture detail steps (trilinear)' ([bool]$s.Trilinear) 'Texture detail levels blend instead of showing a line where they step down (the game uses point filtering between levels).'
$cMip = Add-Check 'Sharp textures show at once (fast mip fade)' ([bool]$s.MipFade) 'A newly loaded sharper texture shows at once instead of fading in over 0.3-2 seconds.'
$cPool = Add-Combo 'Texture memory' $poolNames ([int]$s.Pool) 'The texture streaming pool. Bigger = fewer blurry textures waiting to sharpen. The larger sizes leave less memory for the rest of the game.'
$cDraw = Add-Combo 'Draw distance' $drawNames ([int]$s.DrawDist) 'How far away detail and objects are still drawn.'
$cUW = Add-Check 'Ultrawide 21:9' ([bool]$s.Ultrawide) 'A wider view for a 21:9 screen; black bars on other shapes.'
$cHDR = Add-Check 'HDR' ([bool]$s.HDR) 'Needs HDR turned on in Windows display settings.'
$cPaper = Add-Number 'HDR paper white (nits)' 80 400 10 ([double]$s.PaperWhite) 'How bright normal scenes are. 203 is the reference; raise it in a bright room.'
$cPeak = Add-Number 'HDR peak (nits)' 400 4000 100 ([double]$s.Peak) "Your screen's peak brightness (see its specs or Windows HDR calibration)."
$cHDR.Add_CheckedChanged({ $cPaper.Enabled = $cHDR.Checked; $cPeak.Enabled = $cHDR.Checked }); $cPaper.Enabled = $cHDR.Checked; $cPeak.Enabled = $cHDR.Checked

# -- Effects tab (the game's own engine settings, changed in an edited copy of its config; its files are never written)
[void](New-Page 'Effects')
$cBlur = Add-Check 'Motion blur' ([bool]$s.MotionBlur)
$cDoF = Add-Check 'Depth of field' ([bool]$s.DoF) 'The blur of distant / out-of-focus areas (cutscenes, aiming).'
$cFlare = Add-Check 'Lens flares' ([bool]$s.LensFlares)
$cShake = Add-Combo 'Camera shake (roadie run)' $shakeNames ([int]$s.Shake) 'The camera shake while roadie-running. Less / Off are not yet checked by eye in every game - if Off looks stronger, tell Claude.'
$cFov = Add-Number 'Field of view (%)' 80 130 5 ([double]$s.Fov) '100 = the original. Widens (or narrows) every camera view by this much.'

# -- Controls & sound tab
[void](New-Page 'Controls and sound')
$cMnk = Add-Check 'Keyboard and mouse (Gears layout)' ([bool]$s.Mnk) ("WASD move, mouse look, left click fire, right click aim, Space cover/roll, " +
  "F melee, E use, Q look at, R reload, Tab Tac/Com, 1-4 weapons, middle click / Z zoom, C left-stick click, " +
  "Esc / Enter pause, Backspace back. A controller still works.")
$cSens = Add-Number 'Mouse sensitivity' 0.1 5 0.1 ([double]$s.Sens) 'How fast the mouse turns the camera.' 2
$cDead = Add-Combo 'Extra stick dead zone' $deadNames ([int]$s.DeadZone) 'Added on top of the game''s own dead zone, for worn sticks that drift. Game''s own = nothing added.'
$cVib = Add-Check 'Controller vibration' ([bool]$s.Vibration)
$cInv = Add-Check 'Invert look (up / down)' $false
$cLag = Add-Check 'Lower input lag' ([bool]$s.LowLag) 'Fewer frames waiting in line between the game and your screen, so the controller feels quicker. May cost a few fps on a busy graphics card; untick to compare.'
$cMute = Add-Check 'Mute when the game is not the active window' ([bool]$s.Mute) 'Silences the game while another window is in front; the sound comes back when you click back into the game.'
$cInv.Enabled = $false; $tip.SetToolTip($cInv, 'Use the game''s own Options menu (Controls: Invert Look) - it is saved with your profile in every game.')
$lBright = Add-Label 'Brightness: use the game''s own Options menu (Display: Brightness / Gamma).'; $lBright.ForeColor = [Drawing.Color]::DimGray

# ---- Buttons --------------------------------------------------------------------------------------------------------
$by = $tabs.Bottom + 8
function Add-Button([string]$t, [int]$x, [int]$w, [string]$tt) { $b = New-Object Windows.Forms.Button; $b.Text = $t; $b.Width = $w; $b.Height = 30; $b.Location = New-Object Drawing.Point $x, $script:by; $f.Controls.Add($b); if ($tt) { $tip.SetToolTip($b, $tt) }; return $b }
$script:by = $by
$bSaves = Add-Button 'Open saves' 8 100 'Opens the selected game''s save folder.'
$bBackup = Add-Button 'Back up saves' 112 100 'Zips the selected game''s save folder to your desktop (nothing else).'
$bLogs = Add-Button 'Open logs' 216 100 'Opens the folder with one log folder per play session.'
$bReport = Add-Button 'Problem report' 320 100 'Makes a zip on your desktop with the newest log of the selected game, its settings and any crash dump - no saves, no game files. Attach it to a GitHub issue.'
$script:by += 36
$bReset = Add-Button 'Reset to defaults' 8 100 'Puts every box back to its default (asks first).'
$bPre = Add-Button 'Pre-build shaders' 112 100 'Starts the selected game in each chapter for a short while, on its own, so the shaders are built before you play (fewer stutters the first time through). Takes a while; you can cancel. Your saves are not used.'
$bPre.Visible = $false   # patch 0132 (Shagg 21:46): replaying chapters to pre-build is dropped; the renderer builds in the background and new objects pop in
$clear = Add-Button 'Clear shaders' 216 100 'Clears the saved shaders and pipelines of the selected game, so the next start builds them again (as on a new PC). The folders are renamed, not deleted.'
$play = Add-Button 'Play' 320 100 ''; $play.Height = 34; $f.AcceptButton = $play
$note = New-Object Windows.Forms.Label; $note.AutoSize = $true; $note.ForeColor = [Drawing.Color]::DimGray
$note.Location = New-Object Drawing.Point 12, ($script:by + 42); $f.Controls.Add($note)
$lPad = New-Object Windows.Forms.Label; $lPad.AutoSize = $true; $lPad.ForeColor = [Drawing.Color]::DimGray; $lPad.Visible = $false
$lPad.Text = 'Controller: D-pad move, A choose, B back, LB / RB tabs, Start play, View save'
$lPad.Location = New-Object Drawing.Point 12, ($script:by + 62); $f.Controls.Add($lPad)
$f.ClientSize = New-Object Drawing.Size 428, ($script:by + 86)

# ---- Per-game state -------------------------------------------------------------------------------------------------
function Set-Can($c, [string]$why) {
  $c.Enabled = -not $why
  $base = $script:tips[$c]
  $tip.SetToolTip($c, $(if ($why) { ("$why $base").Trim() } else { $base }))
}
$script:shown = -1
# Idea 1: one set of graphics / effects / window boxes per game.
function Save-Per([int]$g) {
  $script:applying = $true
  $per["$g"] = @{ Preset = $cPreset.SelectedIndex; Resolution = $cRes.SelectedIndex; Fps = $cFps.SelectedIndex; AAMode = $cAA.SelectedIndex
    Trilinear = $cTri.Checked; MipFade = $cMip.Checked; Pool = $cPool.SelectedIndex; DrawDist = $cDraw.SelectedIndex
    MotionBlur = $cBlur.Checked; DoF = $cDoF.Checked; LensFlares = $cFlare.Checked; Shake = $cShake.SelectedIndex; Fov = [int]$cFov.Value
    Ultrawide = $cUW.Checked; HDR = $cHDR.Checked; PaperWhite = [int]$cPaper.Value; Peak = [int]$cPeak.Value; SkipLogos = $cSkip.Checked
    Players = $cPlayers.SelectedIndex; Fullscreen = $cFull.Checked; Exclusive = $cExcl.Checked; WindowSize = $cWin.SelectedIndex }
  $script:applying = $false
}
function Load-Per([int]$g) {
  $v = $per["$g"]; if (-not $v) { $v = @{}; foreach ($k in $perKeys) { $v[$k] = $s[$k] } }
  $script:applying = $true
  $sel = { param($c, $i) $c.SelectedIndex = [Math]::Max(0, [Math]::Min($c.Items.Count - 1, [int]$i)) }
  & $sel $cRes $v.Resolution; & $sel $cFps $v.Fps; & $sel $cAA $v.AAMode; & $sel $cPool $v.Pool; & $sel $cDraw $v.DrawDist
  & $sel $cShake $v.Shake; & $sel $cPlayers $v.Players; & $sel $cWin $v.WindowSize
  $cTri.Checked = [bool]$v.Trilinear; $cMip.Checked = [bool]$v.MipFade; $cBlur.Checked = [bool]$v.MotionBlur; $cDoF.Checked = [bool]$v.DoF
  $cFlare.Checked = [bool]$v.LensFlares; $cUW.Checked = [bool]$v.Ultrawide; $cHDR.Checked = [bool]$v.HDR; $cSkip.Checked = [bool]$v.SkipLogos
  $cFull.Checked = [bool]$v.Fullscreen; $cExcl.Checked = [bool]$v.Exclusive
  $cFov.Value = [decimal][Math]::Max(80, [Math]::Min(130, [double]$v.Fov))
  $cPaper.Value = [decimal][Math]::Max(80, [Math]::Min(400, [double]$v.PaperWhite)); $cPeak.Value = [decimal][Math]::Max(400, [Math]::Min(4000, [double]$v.Peak))
  & $sel $cPreset $v.Preset
  $script:applying = $false
}
function Exe-For([int]$g) {
  switch ($g) { 0 { if ($tu6Ready -and $cVer.SelectedIndex -eq 0) { $exeTu6 } else { $exeDisc } }
                1 { if ($g3Tu6Ready -and $cVer.SelectedIndex -eq 0) { $exeG3Tu6 } else { $exeG3 } } 2 { $exeGJ } }
}
function Update-ForGame {
  $g = $cGame.SelectedIndex
  if ($g -ne $script:shown) {
    # per-game boxes: start point, version, the patch boxes (Gears 3's patches are opt-in, so kept apart), and the set
    if ($script:shown -ge 0) {
      switch ($script:shown) { 0 { $s.StartG2 = $cStart.SelectedIndex; $s.Version = $cVer.SelectedIndex; $s.Aniso = $cAniso.Checked; $s.FpsMode = $cShow.SelectedIndex }
                               1 { $s.StartG3 = $cStart.SelectedIndex; $s.G3Version = $cVer.SelectedIndex; $s.G3Aniso = $cAniso.Checked; $s.G3FpsMode = $cShow.SelectedIndex }
                               2 { $s.StartGJ = $cStart.SelectedIndex; $s.GJAniso = $cAniso.Checked; $s.GJFpsMode = $cShow.SelectedIndex } }
      Save-Per $script:shown
    }
    $names = @($startNamesG2, $startNamesG3, $startNamesGJ)[$g]
    $want = @([int]$s.StartG2, [int]$s.StartG3, [int]$s.StartGJ)[$g]
    $cStart.Items.Clear(); [void]$cStart.Items.AddRange($names); $cStart.SelectedIndex = [Math]::Max(0, [Math]::Min($names.Count - 1, $want))
    $cVer.SelectedIndex = $(if ($g -eq 0) { $(if ($tu6Ready) { [int]$s.Version } else { 1 }) } elseif ($g -eq 1) { $(if ($g3Tu6Ready) { [int]$s.G3Version } else { 1 }) } else { 0 })
    $cAniso.Checked = @([bool]$s.Aniso, [bool]$s.G3Aniso, [bool]$s.GJAniso)[$g]
    $cShow.SelectedIndex = @([int]$s.FpsMode, [int]$s.G3FpsMode, [int]$s.GJFpsMode)[$g]
    Load-Per $g
    $script:shown = $g
  }
  Set-Can $cStart (Can $g 'start'); Set-Can $cVer (Can $g 'version'); Set-Can $cPlayers (Can $g 'players')
  Set-Can $cSkip (Can $g 'skiplogos'); Set-Can $cAniso (Can $g 'aniso'); Set-Can $cTri (Can $g 'trilinear')
  Set-Can $cMip (Can $g 'mipfade'); Set-Can $cPool (Can $g 'pool'); Set-Can $cDraw (Can $g 'drawdist'); Set-Can $cUW (Can $g 'ultrawide')
  Set-Can $cCheat (Can $g 'cheats'); Set-Can $cMut (Can $g 'mutators'); Set-Can $cLsel (Can $g 'levelsel')
  Set-Can $cMute (Can $g 'mute')
  Set-Can $cLag (Can $g 'lowlag')
  Set-Can $cVsync (Can $g 'vsync')   # launcher patch 0155
  Set-Can $cFG (Can $g 'framegen')   # 0146 / 0147
  $script:sceneOff = [bool](Can $g 'scenescale'); Set-Can $cScene (Can $g 'scenescale'); if ($cAA.SelectedIndex -lt 2) { $cScene.Enabled = $false }   # 0142
  $pre = Can $g 'prebuild'; $bPre.Enabled = -not $pre; $tip.SetToolTip($bPre, $(if ($pre) { "$pre " } else { '' }) + 'Starts the selected game in each chapter for a short while, on its own, so the shaders are built before you play (fewer stutters the first time through). Takes a while; you can cancel. Your saves are not used.')
  if ($g -eq 2 -and $cShow.SelectedIndex -eq 3) { $cShow.SelectedIndex = 0 }
  $tip.SetToolTip($cShow, $(if ($g -eq 2) { "$(Not-Yet 2 "game's own counter") $($script:tips[$cShow])" } else { $script:tips[$cShow] }))
  $tip.SetToolTip($cPlayers, $(if ($g -eq 2) { "Untested in Judgment. $($script:tips[$cPlayers])" } else { $script:tips[$cPlayers] }))
  # Idea 5: the build date of the exe that Play would start, under the buttons (with the graphics card).
  $exe = Exe-For $g
  if (-not $gameReady[$g] -or -not (Test-Path $exe)) { $play.Enabled = $false; $note.Text = "$($gameNames[$g]) is not built yet." }
  else {
    $play.Enabled = $true
    $card = $(if ($gpuMain) { $(if ($gpuMain.Length -gt 32) { $gpuMain.Substring(0, 32) + '...' } else { $gpuMain }) } else { 'card not detected' })
    $note.Text = "Build of $((Get-Item $exe).LastWriteTime.ToString('yyyy-MM-dd HH:mm')) | $card"
  }
}
$cVer.Add_SelectedIndexChanged({ if ($script:shown -ge 0) { Update-ForGame } })
# ---- Controller (patch 0131, Athelas ideas 9 / 10) -------------------------------------------------------------------
# The launcher works with an Xbox-type controller, only while one of its own windows is in front and never while a game
# it started is running: D-pad / left stick moves, A chooses (ticks a box, presses a button, opens a list), B goes back
# (closes an open list, else jumps to Play), LB / RB change tabs, Start = Play, View (Back) = save the settings.
# In a message box of the launcher: A = Enter (the default button), B = Esc, D-pad = arrow keys.
$script:padOn = $true
$script:pad = @{ slots = @(); scan = [DateTime]::MinValue; prev = 0; next = [DateTime]::MinValue; shown = $false }
function Pad-Buttons {
  $now = [DateTime]::Now
  if ($now -ge $script:pad.scan) {   # empty slots are slow to ask, so they are looked at every 2 s only
    $script:pad.scan = $now.AddSeconds(2)
    $script:pad.slots = @(0..3 | Where-Object { $st = New-Object GearsPad+State; [GearsPad]::Read([uint32]$_, [ref]$st) })
    $lPad.Visible = $script:pad.slots.Count -gt 0
  }
  $b = 0
  foreach ($i in $script:pad.slots) {
    $st = New-Object GearsPad+State
    if ([GearsPad]::Read([uint32]$i, [ref]$st)) {
      $b = $b -bor [int]$st.Buttons
      if ($st.LY -gt 20000) { $b = $b -bor 0x1 } elseif ($st.LY -lt -20000) { $b = $b -bor 0x2 }
      if ($st.LX -lt -20000) { $b = $b -bor 0x4 } elseif ($st.LX -gt 20000) { $b = $b -bor 0x8 }
    }
  }
  return $b
}
function Pad-Focused {
  $c = $f.ActiveControl
  while ($c -is [Windows.Forms.ContainerControl] -and $c.ActiveControl) { $c = $c.ActiveControl }
  return $c
}
function Pad-Move([bool]$forward) { $c = Pad-Focused; if (-not $c) { $c = $tabs }; [void]$f.SelectNextControl($c, $forward, $true, $true, $true) }
function Pad-Press([int]$btn) {
  $active = [Windows.Forms.Form]::ActiveForm
  if ($null -eq $active) {   # a message box of the launcher
    $k = @{ 0x1000 = '{ENTER}'; 0x2000 = '{ESC}'; 0x4 = '{LEFT}'; 0x8 = '{RIGHT}'; 0x1 = '{UP}'; 0x2 = '{DOWN}' }[$btn]
    if ($k) { [Windows.Forms.SendKeys]::SendWait($k) }
    return
  }
  if ($active -ne $f) { if ($btn -eq 0x2000) { [Windows.Forms.SendKeys]::SendWait('{ESC}') }; return }   # the pre-build window: B = Cancel
  if (-not $script:pad.shown) { [GearsPad]::ShowFocus($f.Handle); $script:pad.shown = $true }
  $c = Pad-Focused
  switch ($btn) {
    { $_ -eq 0x1 -or $_ -eq 0x2 } {
      $down = $btn -eq 0x2
      if ($c -is [Windows.Forms.ComboBox] -and $c.DroppedDown) { $c.SelectedIndex = [Math]::Max(0, [Math]::Min($c.Items.Count - 1, $c.SelectedIndex + $(if ($down) { 1 } else { -1 }))) }
      else { Pad-Move $down }
    }
    { $_ -eq 0x4 -or $_ -eq 0x8 } {
      $d = $(if ($btn -eq 0x8) { 1 } else { -1 })
      if ($c -is [Windows.Forms.ComboBox]) { if ($c.Enabled) { $c.SelectedIndex = [Math]::Max(0, [Math]::Min($c.Items.Count - 1, $c.SelectedIndex + $d)) } }
      elseif ($c -is [Windows.Forms.NumericUpDown]) { if ($d -gt 0) { $c.UpButton() } else { $c.DownButton() } }
      elseif ($c -is [Windows.Forms.TabControl]) { $c.SelectedIndex = ($c.SelectedIndex + $d + $c.TabCount) % $c.TabCount }
      elseif ($c -is [Windows.Forms.Button]) { Pad-Move ($d -gt 0) }
    }
    0x1000 {
      if ($c -is [Windows.Forms.CheckBox]) { if ($c.Enabled) { $c.Checked = -not $c.Checked } }
      elseif ($c -is [Windows.Forms.Button]) { if ($c.Enabled) { $c.PerformClick() } }
      elseif ($c -is [Windows.Forms.ComboBox]) { if ($c.Enabled) { $c.DroppedDown = -not $c.DroppedDown } }
      elseif ($c -is [Windows.Forms.TabControl]) { Pad-Move $true }
    }
    0x2000 { if ($c -is [Windows.Forms.ComboBox] -and $c.DroppedDown) { $c.DroppedDown = $false } else { [void]$play.Focus() } }
    { $_ -eq 0x100 -or $_ -eq 0x200 } {
      $tabs.SelectedIndex = ($tabs.SelectedIndex + $(if ($btn -eq 0x200) { 1 } else { -1 }) + $tabs.TabCount) % $tabs.TabCount
      [void]$tabs.Focus()
    }
    0x10 { if ($play.Enabled) { $play.PerformClick() } }
    0x20 { Save-Settings $cGame.SelectedIndex; Show-Saved }
  }
}
$padTimer = New-Object Windows.Forms.Timer; $padTimer.Interval = 30
$padTimer.Add_Tick({
  if (-not $script:padOn) { return }
  try {
    if (-not [GearsPad]::OursInFront()) { $script:pad.prev = 0; return }
    $b = Pad-Buttons
    $now = [DateTime]::Now
    $new = $b -band (-bnot $script:pad.prev)
    if ($new -band 0xF) { $script:pad.next = $now.AddMilliseconds(400) }           # a direction held: repeat after 0.4 s ...
    elseif (($b -band 0xF) -and $now -ge $script:pad.next) { $new = $new -bor ($b -band 0xF); $script:pad.next = $now.AddMilliseconds(120) }   # ... then every 0.12 s
    $script:pad.prev = $b   # before the presses: a press may open a message box, whose loop runs this timer again
    foreach ($bit in 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x100, 0x200, 0x1000, 0x2000) { if ($new -band $bit) { Pad-Press $bit } }
  } catch {}
})
$padTimer.Start()

# ---- Presets (Shagg's option 2): Original = every box at the game's own value; High / Ultra = our best picks ---------
$script:applying = $false
function Apply-Preset([int]$p) {
  if ($p -ge 3 -or $script:applying) { return }
  $script:applying = $true
  $v = @{
    0 = @{ Res = 0; Fps = 0; AA = 0; Aniso = $false; Tri = $false; Mip = $false; Pool = 0; Draw = 0; Blur = $true; DoF = $true; Flare = $true; Shake = 0; Fov = 100 }
    1 = @{ Res = 1; Fps = 1; AA = 1; Aniso = $true; Tri = $true; Mip = $true; Pool = 1; Draw = 1; Blur = $true; DoF = $true; Flare = $true; Shake = 0; Fov = 100 }
    2 = @{ Res = 4; Fps = 2; AA = 1; Aniso = $true; Tri = $true; Mip = $true; Pool = 2; Draw = 2; Blur = $true; DoF = $true; Flare = $true; Shake = 0; Fov = 100 }
  }[$p]
  $cRes.SelectedIndex = $v.Res; $cFps.SelectedIndex = $v.Fps; $cAA.SelectedIndex = $v.AA; $cAniso.Checked = $v.Aniso
  $cTri.Checked = $v.Tri; $cMip.Checked = $v.Mip; $cPool.SelectedIndex = $v.Pool; $cDraw.SelectedIndex = $v.Draw
  $cBlur.Checked = $v.Blur; $cDoF.Checked = $v.DoF; $cFlare.Checked = $v.Flare; $cShake.SelectedIndex = $v.Shake; $cFov.Value = $v.Fov
  $script:applying = $false
}
$toCustom = { if (-not $script:applying -and $cPreset.SelectedIndex -ne 3) { $cPreset.SelectedIndex = 3 } }
foreach ($c in @($cRes, $cFps, $cAA, $cPool, $cDraw, $cShake)) { $c.Add_SelectedIndexChanged($toCustom) }
foreach ($c in @($cAniso, $cTri, $cMip, $cBlur, $cDoF, $cFlare)) { $c.Add_CheckedChanged($toCustom) }
$cFov.Add_ValueChanged($toCustom)
$cPreset.Add_SelectedIndexChanged({ Apply-Preset $cPreset.SelectedIndex })
$cGame.Add_SelectedIndexChanged({ Update-ForGame })
Update-ForGame

# ---- Buttons: saves, logs, report, reset, clear shaders -------------------------------------------------------------
$bSaves.Add_Click({
  $d = $userDirs[$cGame.SelectedIndex]; New-Item -ItemType Directory -Force $d | Out-Null; Start-Process explorer.exe -ArgumentList "`"$d`""
})
$bLogs.Add_Click({
  $d = Join-Path $root 'out\play-logs'; New-Item -ItemType Directory -Force $d | Out-Null; Start-Process explorer.exe -ArgumentList "`"$d`""
})
# ---- Problem report (patch 0131, Athelas idea 15): the session's files, the settings and a description of the PC --------
function PC-Description {
  $l = @()
  try { $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop; $l += "Windows: $($os.Caption) $($os.Version) (build $($os.BuildNumber)), $([Math]::Round($os.TotalVisibleMemorySize / 1MB, 1)) GB memory, $env:PROCESSOR_ARCHITECTURE" }
  catch { $l += "Windows: $([Environment]::OSVersion.VersionString), $env:PROCESSOR_ARCHITECTURE" }
  try { foreach ($c in @(Get-CimInstance Win32_Processor -ErrorAction Stop)) { $l += "CPU: $(([string]$c.Name).Trim()) - $($c.NumberOfCores) cores, $($c.NumberOfLogicalProcessors) threads" } } catch {}
  try {
    foreach ($v in @(Get-CimInstance Win32_VideoController -ErrorAction Stop)) {
      $date = try { ([datetime]$v.DriverDate).ToString('yyyy-MM-dd') } catch { '?' }
      $l += "Graphics card: $($v.Name), driver $($v.DriverVersion) of $date$(if ($v.CurrentRefreshRate) { ", showing $($v.CurrentHorizontalResolution)x$($v.CurrentVerticalResolution) at $($v.CurrentRefreshRate) Hz" })"
    }
  } catch {}
  foreach ($sc in [Windows.Forms.Screen]::AllScreens) { $l += "Screen: $($sc.Bounds.Width)x$($sc.Bounds.Height)$(if ($sc.Primary) { ' (main)' })" }
  $pads = @(0..3 | Where-Object { $st = New-Object GearsPad+State; [GearsPad]::Read([uint32]$_, [ref]$st) } | ForEach-Object { $_ + 1 })
  $l += "Xbox-type controllers connected: $(if ($pads.Count) { $pads -join ', ' } else { 'none' })"
  return $l
}
# The Windows user name and PC name are blanked in every file of the report: text files get <user> / <pc>, other files
# (crash dumps) the same number of x's, so nothing else in them moves.
function Scrub-Folder([string]$dir) {
  $words = @(@(@($env:USERNAME, '<user>'), @($env:COMPUTERNAME, '<pc>')) | Where-Object { $_[0] -and $_[0].Length -ge 3 })
  foreach ($file in @(Get-ChildItem -LiteralPath $dir -Recurse -File)) {
    if ($file.Extension -match '^\.(txt|log|json|toml|ini|md|csv)$') {
      $t = [IO.File]::ReadAllText($file.FullName); $o = $t
      foreach ($w in $words) { $t = [regex]::Replace($t, [regex]::Escape($w[0]), $w[1], 'IgnoreCase') }
      if ($t -ne $o) { [IO.File]::WriteAllText($file.FullName, $t) }
    } else {
      $b = [IO.File]::ReadAllBytes($file.FullName); $n = 0
      foreach ($w in $words) { $n += [GearsPad]::Blank($b, [Text.Encoding]::ASCII.GetBytes($w[0])); $n += [GearsPad]::Blank($b, [Text.Encoding]::Unicode.GetBytes($w[0])) }
      if ($n) { [IO.File]::WriteAllBytes($file.FullName, $b) }
    }
  }
}
function Newest-Session([int]$g) {
  $pre = $logPrefix[$g]
  return @(Get-ChildItem (Join-Path $root 'out\play-logs') -Directory -ErrorAction SilentlyContinue | Where-Object {
      if ($pre) { $_.Name.StartsWith($pre) -and $_.Name -notmatch 'prebuild' } else { $_.Name -match '^\d{8}-\d{6}$' } } | Sort-Object LastWriteTime | Select-Object -Last 1)
}
# One zip on the desktop: the session's folder (log, args.txt, crash report and dump), the launcher settings and
# report.txt (what happened, the PC, the game files' fingerprints). No saves, no game files. Nothing is sent anywhere.
function Make-Report([int]$g, [string]$session, [string]$what = '') {
  $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
  $tmp = Join-Path $env:TEMP "gears-native-report-$stamp"; New-Item -ItemType Directory -Force $tmp | Out-Null
  try {
    Copy-Item -Path (Join-Path $session '*') -Destination $tmp -Recurse -Force
    if (Test-Path $store) { Copy-Item $store (Join-Path $tmp 'launcher-settings.json') }
    $tuOn = switch ($g) { 0 { $tu6Ready -and $cVer.SelectedIndex -eq 0 } 1 { $g3Tu6Ready -and $cVer.SelectedIndex -eq 0 } 2 { Test-Path (Join-Path $gjTu4Dir 'default.xexp') } }
    $files = Check-GameFiles $g $tuOn
    $exe = Exe-For $g
    $lines = @("Game: $($gameNames[$g])", "Session: $(Split-Path $session -Leaf)")
    if ($what) { $lines += "What happened (the launcher's reading): $what" }
    $lines += @("Exe: $exe$(if (Test-Path $exe) { ", built $((Get-Item $exe).LastWriteTime.ToString('yyyy-MM-dd HH:mm'))" })", '') + (PC-Description) + @('') + $files.lines +
              @('', "Made by the launcher at $stamp. Your Windows user name and PC name are replaced by <user> / <pc> (x's in crash dumps).")
    Set-Content (Join-Path $tmp 'report.txt') $lines
    Scrub-Folder $tmp
    $zip = Join-Path ([Environment]::GetFolderPath('Desktop')) "Gears-Native-report-$($gameShort[$g] -replace '[^A-Za-z0-9]', '')-$stamp.zip"
    Compress-Archive -Path (Join-Path $tmp '*') -DestinationPath $zip -Force
    [void][Windows.Forms.MessageBox]::Show("Saved on your desktop:`r`n$zip`r`n`r`nAttach it to a GitHub issue and say what you were doing. It holds the log, the settings, a description of this PC and any crash report - no saves, no game files, and your Windows user name is taken out. Nothing is sent by itself.", 'Problem report', 'OK', 'Information')
  } catch { [void][Windows.Forms.MessageBox]::Show("Could not make the report: $($_.Exception.Message)", 'Problem report', 'OK', 'Warning') }
  finally { Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue }
}
$bReport.Add_Click({
  $g = $cGame.SelectedIndex
  $last = Newest-Session $g
  if (-not $last.Count) { [void][Windows.Forms.MessageBox]::Show("No play session of $($gameNames[$g]) found yet - play once, then make the report.", 'Problem report', 'OK', 'Information'); return }
  Make-Report $g $last[0].FullName
})
$bBackup.Add_Click({
  # Idea 2: the selected game's save folder zipped to the desktop. Nothing else goes in.
  $g = $cGame.SelectedIndex; $d = $userDirs[$g]
  if (-not (Test-Path $d) -or -not @(Get-ChildItem $d -Recurse -File -ErrorAction SilentlyContinue).Count) { [void][Windows.Forms.MessageBox]::Show("$($gameNames[$g]) has no saves here yet.", 'Back up saves', 'OK', 'Information'); return }
  if (Get-Process $procNames[$g] -ErrorAction SilentlyContinue) { [void][Windows.Forms.MessageBox]::Show("$($gameNames[$g]) is running - close it first, so the save is complete.", 'Back up saves', 'OK', 'Information'); return }
  $zip = Join-Path ([Environment]::GetFolderPath('Desktop')) "Gears-Native-saves-$($gameShort[$g] -replace '[^A-Za-z0-9]', '')-$(Get-Date -Format 'yyyyMMdd-HHmmss').zip"
  try { Compress-Archive -Path (Join-Path $d '*') -DestinationPath $zip -Force; [void][Windows.Forms.MessageBox]::Show("Saved on your desktop:`r`n$zip`r`n`r`nTo restore: close the game and unzip it into`r`n$d", 'Back up saves', 'OK', 'Information') }
  catch { [void][Windows.Forms.MessageBox]::Show("Could not back up: $($_.Exception.Message)", 'Back up saves', 'OK', 'Warning') }
})
$bPre.Add_Click({
  # Idea 6: start the game in each chapter for a short while, on its own (no controller input, its own throw-away save
  # folder), so the shader cache Play uses is filled before playing. Same exe, same shader folder, same resolution / AA
  # choices as Play (they decide which pipelines are made). Each chapter is closed politely, then ended if it hangs.
  $g = $cGame.SelectedIndex
  if (Get-Process $procNames[$g] -ErrorAction SilentlyContinue) { [void][Windows.Forms.MessageBox]::Show("$($gameNames[$g]) is running. Close it first.", 'Pre-build shaders', 'OK', 'Information'); return }
  try {
    $lastLock = Get-Content $lockFile -ErrorAction Stop | Where-Object { $_.Trim() } | Select-Object -Last 1
    if ($lastLock -match '^\S+ \| (\S+) \| (\S+) \| started \|') { [void][Windows.Forms.MessageBox]::Show("A test job is running right now ($($Matches[1]): $($Matches[2])). Try again when it is done.", 'Pre-build shaders', 'OK', 'Information'); return }
  } catch {}
  $count = $cStart.Items.Count - 1
  $secs = 45
  if ([Windows.Forms.MessageBox]::Show("Pre-build the shaders of $($gameNames[$g])?`r`n`r`nThe game starts $count times, once per chapter, for about $secs seconds each (about $([Math]::Ceiling($count * ($secs + 15) / 60)) minutes in all). Don't play meanwhile; you can cancel at any time.`r`nYour saves are not used.", 'Pre-build shaders', 'YesNo', 'Question') -ne 'Yes') { return }
  $exe = Exe-For $g
  $base = Join-Path $root "out\play-logs\$($logPrefix[$g])prebuild-$(Get-Date -Format 'yyyyMMdd-HHmmss')"; New-Item -ItemType Directory -Force $base | Out-Null
  $ud = Join-Path $root "out\prebuild-userdata\$g"; New-Item -ItemType Directory -Force $ud | Out-Null
  $common = @("--user_data_root=`"$ud`"", '--input_backend=none', '--gow2_input_script=300:A,450:A', '--gow2_input_frames=true')
  $scale = [int]$cRes.SelectedIndex + 1
  if ($scale -ge 5) { $common += @('--gow2_renderer_resolution_scale=0', '--gow2_present_native_size=true') } else { $common += "--gow2_renderer_resolution_scale=$scale" }
  switch ($g) {
    0 { $common += @("--game_data_root=`"$(Join-Path $root 'game\extracted')`"", '--gpu_plugin=xenos'); if ($exe -eq $exeTu6) { $common += "--update_data_root=`"$tu6Dir`"" } }
    1 { $common += @("--game_data_root=`"$(Join-Path $g3Root 'game\extracted')`"", "--gow2_shader_cache_dir=`"$(Join-Path $root 'out\gears3-shader-cache')`"", '--gow2_renderer_memexport=true')
        if ($scale -gt 1) { $common += '--gow2_renderer_resolution_scale_threshold=1120' }; if ($exe -eq $exeG3Tu6) { $common += "--update_data_root=`"$g3Tu6Dir`"" } }
    2 { $common += @("--game_data_root=`"$(Join-Path $gjRoot 'game\extracted')`"", '--gpu_plugin=xenos', "--gow2_shader_cache_dir=`"$(Join-Path $root 'out\judgment-shader-cache')`"")
        if (Test-Path (Join-Path $gjTu4Dir 'default.xexp')) { $common += "--update_data_root=`"$gjTu4Dir`"" } }
  }
  switch ([int]$cAA.SelectedIndex) { 1 { $common += '--gow2_present_aa=smaa' } 2 { $common += '--gow2_upscaler=fsr' } 3 { $common += '--gow2_upscaler=dlss' } 4 { $common += '--gow2_upscaler=xess' } }
  $pf = New-Object Windows.Forms.Form; $pf.Text = 'Pre-building shaders'; $pf.FormBorderStyle = 'FixedDialog'; $pf.StartPosition = 'CenterScreen'; $pf.ClientSize = New-Object Drawing.Size 360, 110; $pf.Font = $f.Font
  $pl = New-Object Windows.Forms.Label; $pl.AutoSize = $true; $pl.Location = New-Object Drawing.Point 12, 14; $pf.Controls.Add($pl)
  $pc = New-Object Windows.Forms.Button; $pc.Text = 'Cancel'; $pc.Width = 100; $pc.Location = New-Object Drawing.Point 248, 70; $pf.Controls.Add($pc); $pf.CancelButton = $pc
  $st = @{ i = 0; proc = $null; t0 = $null; stop = $false; closing = $null }
  $pc.Add_Click({ $st.stop = $true; $pc.Enabled = $false; $pl.Text = 'Stopping...' })
  $timer = New-Object Windows.Forms.Timer; $timer.Interval = 1000
  $timer.Add_Tick({
    $p = $st.proc
    if ($p -and -not $p.HasExited) {
      $age = ((Get-Date) - $st.t0).TotalSeconds
      if ($st.stop -or $age -ge $secs) {
        if (-not $st.closing) { $st.closing = Get-Date; try { [void]$p.CloseMainWindow() } catch {} }
        elseif (((Get-Date) - $st.closing).TotalSeconds -ge 15) { try { $p.Kill() } catch {} }
      }
      if (-not $st.stop) { $pl.Text = "Chapter $($st.i) of $count - $([int]$age) s" }
      return
    }
    if ($st.stop -or $st.i -ge $count) { $timer.Stop(); $pf.Close(); return }
    $st.i++; $st.closing = $null
    $dir = Join-Path $base ("chapter-{0:D2}" -f $st.i); New-Item -ItemType Directory -Force $dir | Out-Null
    $url = Launch-Url $g ($st.i - 1)
    $a = $common + @("--log_file=`"$(Join-Path $dir $(@('gow2.log', 'gow3.log', 'gowj.log')[$g]))`"", "--gow2_crash_dir=`"$dir`"",
                     $(if ($g -eq 0) { "--gow2_launch=`"$url`"" } else { "--ue3_launch=`"$url`"" }))
    Set-Content (Join-Path $dir 'args.txt') ($a -join "`r`n")
    $st.proc = Start-Process -FilePath $exe -ArgumentList $a -WorkingDirectory (Split-Path $exe) -PassThru -WindowStyle Minimized
    $st.t0 = Get-Date
  })
  $timer.Start(); [void]$pf.ShowDialog(); $timer.Stop()
  if ($st.proc -and -not $st.proc.HasExited) { try { $st.proc.Kill() } catch {} }
  [void][Windows.Forms.MessageBox]::Show($(if ($st.stop) { "Stopped after chapter $($st.i). What was built is kept." } else { "Done: $count chapters. The shaders are saved for Play." }), 'Pre-build shaders', 'OK', 'Information')
})
$bReset.Add_Click({
  if ([Windows.Forms.MessageBox]::Show('Put every box back to its default? (Your saves are not touched.)', 'Reset to defaults', 'YesNo', 'Question') -ne 'Yes') { return }
  try { if (Test-Path $store) { Rename-Item $store ((Split-Path $store -Leaf) + '.before-reset-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) } } catch {}
  $f.Tag = 'reset'; $f.Close()
})
$clear.Add_Click({
  $g = $cGame.SelectedIndex; $name = $gameNames[$g]
  if (Get-Process $procNames[$g] -ErrorAction SilentlyContinue) { [void][Windows.Forms.MessageBox]::Show("$name is running. Close it first.", 'Clear shaders', 'OK', 'Information'); return }
  $dirs = switch ($g) { 0 { @((Join-Path $root 'build\RelWithDebInfo\out\shader-cache'), (Join-Path $root 'build\RelWithDebInfo-tu6\out\shader-cache')) }
                        1 { @(Join-Path $root 'out\gears3-shader-cache') } 2 { @(Join-Path $root 'out\judgment-shader-cache') } }
  $dirs = @($dirs | Where-Object { Test-Path -LiteralPath $_ })
  if (-not $dirs.Count) { [void][Windows.Forms.MessageBox]::Show("$name has no saved shaders to clear.", 'Clear shaders', 'OK', 'Information'); return }
  $ans = [Windows.Forms.MessageBox]::Show("Clear the saved shaders and pipelines of $($name)?`r`nThe next start builds them again, so expect stutter and pop-in the first time, as on a new PC.`r`n`r`nThe folders are renamed (moved aside), not deleted:`r`n$($dirs -join "`r`n")", 'Clear shaders', 'YesNo', 'Question')
  if ($ans -ne 'Yes') { return }
  $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'; $done = @(); $failed = @()
  foreach ($d in $dirs) { try { Rename-Item -LiteralPath $d -NewName ((Split-Path $d -Leaf) + '.cleared-' + $stamp) -ErrorAction Stop; $done += $d } catch { $failed += "$d ($($_.Exception.Message))" } }
  $msg = if ($failed.Count) { "Not cleared:`r`n$($failed -join "`r`n")" } else { "Cleared $($done.Count) folder(s) for $name." }
  [void][Windows.Forms.MessageBox]::Show($msg, 'Clear shaders', 'OK', $(if ($failed.Count) { 'Warning' } else { 'Information' }))
})

# ---- Play -----------------------------------------------------------------------------------------------------------
# Save every box (Play, and View on a controller). $g = the game the boxes are showing.
function Save-Settings([int]$g) {
  $s.Game = $g; $s.Preset = $cPreset.SelectedIndex
  switch ($g) { 0 { $s.StartG2 = $cStart.SelectedIndex; if ($tu6Ready) { $s.Version = $cVer.SelectedIndex }; $s.Aniso = $cAniso.Checked; $s.FpsMode = $cShow.SelectedIndex }
                1 { $s.StartG3 = $cStart.SelectedIndex; if ($g3Tu6Ready) { $s.G3Version = $cVer.SelectedIndex }; $s.G3Aniso = $cAniso.Checked; $s.G3FpsMode = $cShow.SelectedIndex }
                2 { $s.StartGJ = $cStart.SelectedIndex; $s.GJAniso = $cAniso.Checked; $s.GJFpsMode = $cShow.SelectedIndex } }
  $s.Resolution = $cRes.SelectedIndex; $s.Fps = $cFps.SelectedIndex; $s.AAMode = $cAA.SelectedIndex; $s.AA = $cAA.SelectedIndex -eq 1
  $s.SceneMode = $cScene.SelectedIndex   # patch 0142
  $s.FrameGen = $cFG.SelectedIndex       # patches 0146 / 0147
  $s.Ultrawide = $cUW.Checked; $s.HDR = $cHDR.Checked; $s.Fullscreen = $cFull.Checked; $s.Mnk = $cMnk.Checked; $s.Sens = [double]$cSens.Value
  $s.MotionBlur = $cBlur.Checked; $s.DoF = $cDoF.Checked; $s.LensFlares = $cFlare.Checked; $s.Fov = [int]$cFov.Value
  $s.PaperWhite = [int]$cPaper.Value; $s.Peak = [int]$cPeak.Value; $s.Exclusive = $cExcl.Checked; $s.Monitor = $cMon.SelectedIndex; $s.Players = $cPlayers.SelectedIndex
  $s.SkipLogos = $cSkip.Checked; $s.DrawDist = $cDraw.SelectedIndex; $s.Shake = $cShake.SelectedIndex; $s.Trilinear = $cTri.Checked; $s.MipFade = $cMip.Checked; $s.Pool = $cPool.SelectedIndex
  $s.WindowSize = $cWin.SelectedIndex; $s.DeadZone = $cDead.SelectedIndex; $s.Vibration = $cVib.Checked; $s.Cheats = $cCheat.Checked; $s.Mutators = $cMut.Checked; $s.LevelSelect = $cLsel.Checked; $s.Mute = $cMute.Checked; $s.LowLag = $cLag.Checked; $s.Vsync = $cVsync.Checked
  Save-Per $g; $s.PerGame = $per
  try { New-Item -ItemType Directory -Force (Split-Path $store) | Out-Null; $s | ConvertTo-Json -Depth 6 | Set-Content $store -Encoding UTF8 } catch {}
}
function Show-Saved {
  $was = $note.Text; $note.Text = 'Settings saved.'
  $t = New-Object Windows.Forms.Timer; $t.Interval = 2500; $t.Add_Tick({ $this.Stop(); $note.Text = $was }.GetNewClosure()); $t.Start()
}
$play.Add_Click({
  # A test job holds the shared PC lock: playing now slows it down and can spoil its measurements. Ask first.
  try {
    $lastLock = Get-Content $lockFile -ErrorAction Stop | Where-Object { $_.Trim() } | Select-Object -Last 1
    if ($lastLock -match '^\S+ \| (\S+) \| (\S+) \| started \|') {
      $ans = [Windows.Forms.MessageBox]::Show("A test job is running right now ($($Matches[1]): $($Matches[2])).`r`nPlaying now slows it down and can spoil its results.`r`n`r`nPlay anyway?", 'Test job running', 'YesNo', 'Warning')
      if ($ans -ne 'Yes') { return }
    }
  } catch {}
  $g = $cGame.SelectedIndex
  $isG3 = $g -eq 1; $isGJ = $g -eq 2
  # Idea 5: the game already running (a second copy would fight over the saves and the shader cache)
  if (Get-Process $procNames[$g] -ErrorAction SilentlyContinue) {
    $ans = [Windows.Forms.MessageBox]::Show("$($gameNames[$g]) is already running.`r`nA second copy would share its saves and shaders with the first.`r`n`r`nStart another one anyway?", 'Already running', 'YesNo', 'Warning')
    if ($ans -ne 'Yes') { return }
  }
  Save-Settings $g
  $on = { param($c) $c.Enabled -and $c.Checked }   # a greyed box ("not yet in this game") is never sent
  $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
  $logDir = Join-Path $root "out\play-logs\$($logPrefix[$g])$stamp"; New-Item -ItemType Directory -Force $logDir | Out-Null
  $notes = @()
  switch ($g) {
    0 {
      $a = @("--game_data_root=`"$(Join-Path $root 'game\extracted')`"", "--user_data_root=`"$($userDirs[0])`"",
             "--log_file=`"$(Join-Path $logDir 'gow2.log')`"", "--gow2_crash_dir=`"$logDir`"", '--gpu_plugin=xenos')
      $useTu6 = $tu6Ready -and $cVer.SelectedIndex -eq 0
      $exe = if ($useTu6) { $exeTu6 } else { $exeDisc }
      if ($useTu6) { $a += "--update_data_root=`"$tu6Dir`"" }
    }
    1 {
      $useTu6 = $g3Tu6Ready -and $cVer.SelectedIndex -eq 0
      $exe = if ($useTu6) { $exeG3Tu6 } else { $exeG3 }
      $a = @("--game_data_root=`"$(Join-Path $g3Root 'game\extracted')`"", "--user_data_root=`"$($userDirs[1])`"",
             "--log_file=`"$(Join-Path $logDir 'gow3.log')`"", "--gow2_crash_dir=`"$logDir`"",
             "--gow2_shader_cache_dir=`"$(Join-Path $root 'out\gears3-shader-cache')`"")
      if ($useTu6) { $a += "--update_data_root=`"$g3Tu6Dir`"" }
    }
    2 {
      $exe = $exeGJ
      $a = @("--game_data_root=`"$(Join-Path $gjRoot 'game\extracted')`"", "--user_data_root=`"$($userDirs[2])`"",
             "--log_file=`"$(Join-Path $logDir 'gowj.log')`"", "--gow2_crash_dir=`"$logDir`"", '--gpu_plugin=xenos',
             "--gow2_shader_cache_dir=`"$(Join-Path $root 'out\judgment-shader-cache')`"")
      if (Test-Path (Join-Path $gjTu4Dir 'default.xexp')) { $a += "--update_data_root=`"$gjTu4Dir`"" }
    }
  }
  # Patch 0131: the game files' fingerprints; another version only warns (Start anyway).
  $tuOn = switch ($g) { 0 { $useTu6 } 1 { $useTu6 } 2 { Test-Path (Join-Path $gjTu4Dir 'default.xexp') } }
  $files = Check-GameFiles $g $tuOn
  if (-not $files.ok) {
    $ans = [Windows.Forms.MessageBox]::Show("These game files are not the version this port is made and tested with:`r`n`r`n$($files.lines -join "`r`n")`r`n`r`nIt may crash or look wrong. Start anyway?", 'Different game version', 'YesNo', 'Warning')
    if ($ans -ne 'Yes') { return }
  }
  foreach ($fl in $files.lines) { $notes += "launcher: game files: $fl" }
  $chapter = $cStart.SelectedIndex - 1   # -1 = main menu
  if ($cStart.Enabled -and $chapter -ge 0) {
    $url = Launch-Url $g $chapter
    $a += $(if ($g -eq 0) { "--gow2_launch=`"$url`"" } else { "--ue3_launch=`"$url`"" })
  }
  $scale = [int]$s.Resolution + 1
  if ($scale -ge 5) {
    # "Match my monitor": the smallest whole-number scale that covers the monitor, then resampled to the window's exact
    # size in linear light (layer 0 present pass).
    $a += '--gow2_renderer_resolution_scale=0'; $a += '--gow2_present_native_size=true'; $scale = 2
  } else { $a += "--gow2_renderer_resolution_scale=$scale" }
  if ($isG3) {
    if ($scale -gt 1) { $a += '--gow2_renderer_resolution_scale_threshold=1120' }   # its chat: always with 2x+
    $a += '--gow2_renderer_memexport=true'                                          # its chat: always on for Gears 3
    if ([int]$s.Resolution -ge 4 -and -not $s.Ultrawide) {
      if (Exe-Has $exe 'gow3_aspect') { $a += '--gow3_aspect=auto' } else { $notes += 'launcher: this Gears 3 build has no --gow3_aspect yet - the picture stays 16:9' }
    }
  }
  # Anti-aliasing: DLSS / FSR / XeSS need their DLLs next to the exe; otherwise SMAA, and the note says why.
  $aa = [int]$s.AAMode
  $need = @{ 2 = 'amd_fidelityfx_loader_dx12.dll'; 3 = 'nvngx_dlss.dll'; 4 = 'libxess.dll' }
  if ($aa -eq 3 -and -not $hasRtx -and $gpus.Count) { $notes += "launcher: DLSS needs an NVIDIA RTX card ($gpuMain) - FSR instead"; $aa = 2 }
  if ($aa -ge 2 -and -not (Test-Path (Join-Path (Split-Path $exe) $need[$aa]))) { $notes += "launcher: $($aaNames[$aa]) is not available for this game / build - SMAA instead"; $aa = 1 }
  switch ($aa) { 1 { $a += '--gow2_present_aa=smaa' } 2 { $a += '--gow2_upscaler=fsr' } 3 { $a += '--gow2_upscaler=dlss' } 4 { $a += '--gow2_upscaler=xess' } }
  if ($aa -ge 2 -and $cScene.Enabled -and $cScene.SelectedIndex -gt 0) { $a += @('', '--gow2_upscale_mode=quality', '--gow2_upscale_mode=performance', '--gow2_upscale_mode=ultra')[$cScene.SelectedIndex] }   # 0142
  if ($cFG.Enabled -and $cFG.SelectedIndex -eq 1) { $a += '--gow2_frame_generation=fsr' }   # 0146 / 0147
  if (& $on $cAniso) { $a += @('--gow2_patch_aniso16=true', '--gow3_patch_aniso16=true', '--gowj_aniso16=true')[$g] }
  # Texture streaming pool and Gears 3's built-in texture edits (on by default there, so always sent explicitly)
  if ($cPool.Enabled) {
    $mb = $poolMb[$cPool.SelectedIndex]
    if ($mb -lt 0 -and $isGJ) { $mb = 200 }   # Judgment: 200 checked (its job 033: lowest free physical 206 MB)
    if ($mb -ge 0) { $a += @("--gow2_patch_texture_pool_mb=$mb", "--gow3_texture_pool_mb=$mb", "--gowj_texture_pool_mb=$mb")[$g] }
  }
  # Gears 3 / Judgment: trilinear and fast mip fade are their own switches, on by default there, so always sent.
  if ($isG3) { $a += "--gow3_trilinear=$($cTri.Checked.ToString().ToLower())"; $a += "--gow3_fast_mip_fade=$($cMip.Checked.ToString().ToLower())" }
  if ($isGJ) { $a += "--gowj_trilinear=$($cTri.Checked.ToString().ToLower())"; $a += "--gowj_fast_mip_fade=$($cMip.Checked.ToString().ToLower())" }
  # Gears 3 extras (its 20:40 mail)
  if (& $on $cCheat) { $a += '--gow3_cheats=true' }
  if (& $on $cMut) { $a += '--gow3_unlock_mutators=true' }
  if (& $on $cLsel) { $a += '--gowj_level_select=true' }
  # Engine settings through the game's own config (--ue3_ini_edits; the game's files are never written)
  $ei = $engineIni[$g]
  $ini = @()
  if (-not $s.MotionBlur) { $ini += "$ei|SystemSettings|MotionBlur=False" }
  if (-not $s.DoF) { $ini += "$ei|SystemSettings|DepthOfField=False" }
  if (-not $s.LensFlares) { $ini += "$ei|SystemSettings|LensFlares=False" }
  if ([int]$s.Fov -ne 100) {
    $k = ([double]$s.Fov / 100).ToString([Globalization.CultureInfo]::InvariantCulture); $ini += "GearCamera.ini|*|FOVAngle*=$k"
    if ($g -eq 0) { $ini += "GearPawn.ini|*|DefaultFOV*=$k" }
  }
  if (& $on $cSkip) {
    # Option 1: drop only the logo films from [FullScreenMovie] StartupMovies; the loading film stays (the '-' form).
    if ($g -eq 0) { $ini += "$ei|FullScreenMovie|-StartupMovies=EpicLogo"; $ini += "$ei|FullScreenMovie|-StartupMovies=MGSLogo" }
    elseif ($g -eq 1) { $ini += "$ei|FullScreenMovie|-StartupMovies=Open_Logos" }
    else { $a += '--gowj_skip_logos=true' }   # Judgment's own switch (its job 033), the same edit inside
  }
  if ($cDraw.Enabled -and $cDraw.SelectedIndex -gt 0) { $ini += "$ei|SystemSettings|MaxDrawDistanceScale=$($drawScale[$cDraw.SelectedIndex])" }
  if ($cShake.SelectedIndex -gt 0) {
    $v = $shakeValue[$cShake.SelectedIndex]
    switch ($g) {
      0 { $ini += "GearCamera.ini|GearGame.GearCamMod_CameraBone|RoadieRunShakyCamDampeningFactor=$v" }
      1 { foreach ($pf in 'GearPawn.ini', 'GearPawnMP.ini') { $ini += "$pf|GearGame.GearPawn|RoadieRunShakyCamDampeningFactor=$v" } }
      2 { foreach ($pf in 'GearPawn.ini', 'GearPawnMP.ini', 'GearPawnFFA.ini') { $ini += "$pf|GearGame.GearPawn|RoadieRunShakyCamDampeningFactor=$v" } }
    }
  }
  if ($g -eq 0 -and (& $on $cTri)) { $ini += "$ei|SystemSettings|~=MipFilter=point>MipFilter=linear" }   # patch 0127's replace form
  if ($ini.Count) { $a += "--ue3_ini_edits=`"$($ini -join ';')`"" }
  if (& $on $cUW) { $a += $(if ($isG3) { '--gow3_ultrawide=true' } else { '--gow2_ultrawide=true' }) }
  if ($s.HDR) { $a += @('--present_hdr=true', "--gow2_hdr_paper_white=$($s.PaperWhite)", "--gow2_hdr_peak=$($s.Peak)") }
  if ($s.Fullscreen) { $a += '--fullscreen=true'; if ($s.Exclusive) { $a += '--fullscreen_exclusive=true' } }
  elseif ($cWin.SelectedIndex -gt 0) { $ws = $winSizes[$cWin.SelectedIndex]; $a += "--window_width=$($ws[0])"; $a += "--window_height=$($ws[1])" }
  # Controller (the SDK's input layer, every game)
  if ($cDead.SelectedIndex -gt 0) {
    $dz = ([double]$deadValue[$cDead.SelectedIndex]).ToString([Globalization.CultureInfo]::InvariantCulture)
    $a += "--left_stick_deadzone_percentage=$dz"; $a += "--right_stick_deadzone_percentage=$dz"
  }
  if (-not $cVib.Checked) { $a += '--vibration=false' }
  # Patch 0133: lower input lag = 1 frame queued on the card (renderer) and, from SDK patch 34, 1 frame queued for the screen
  # (the SDK's default); unticked = both as before.
  if ($cLag.Enabled) {
    if ($cLag.Checked) {
      $a += '--gow2_renderer_frames_in_flight=1'
      # Patch 0135: on an NVIDIA card, NVIDIA Reflex too (the game reads the controller later and the frame waits less).
      if (($gpuMain -match 'NVIDIA|GeForce|Quadro') -and (Exe-Has $exe 'gow2_reflex')) { $a += '--gow2_reflex=true' }
      # Patch 0136: on an AMD card, AMD Anti-Lag 2 (same idea; needs a 2024-or-newer Radeon driver).
      if (($gpuMain -match 'AMD|Radeon') -and (Exe-Has $exe 'gow2_antilag2')) { $a += '--gow2_antilag2=true' }
    }
    else {  # SDK patch 34's switch lives in the SDK runtime DLL next to the exe, not in the exe
      # Patch 0152: the exe's own default is now 1 frame in flight, so unticked has to ask for 2 (as before) out loud.
      if (Exe-Has $exe 'gow2_renderer_frames_in_flight') { $a += '--gow2_renderer_frames_in_flight=2' }
      $rt = @('rexruntimerd.dll', 'rexruntime.dll' | ForEach-Object { Join-Path (Split-Path $exe) $_ } | Where-Object { Test-Path $_ }) | Select-Object -First 1
      if ($rt -and (Exe-Has $rt 'd3d12_max_frame_latency')) { $a += '--d3d12_max_frame_latency=0' }
    }
  }
  if (& $on $cMute) { $a += "--$($procNames[$g])_mute_unfocused=true" }   # patch 0130 (Gears 2 first; the others when they copy it)
  if ($s.Monitor -gt 0) { $a += "--monitor=$($s.Monitor)" }
  if ($cPlayers.Enabled -and $s.Players -gt 0) { $a += "--local_players=$($s.Players + 1)" }
  switch ($cShow.SelectedIndex) { 1 { $a += '--gow2_fps_bar=top' } 2 { $a += '--gow2_fps_bar=bottom' } 3 { if ($g -ne 2) { $a += $(if ($isG3) { '--gow3_patch_show_fps=true' } else { '--gow2_patch_show_fps=true' }) } } }
  if ($s.Mnk) {
    # The SDK's keyboard/mouse -> controller emulation, with every default bind replaced so none clash.
    $a += @('--mnk_mode=true', '--mnk_mouse=true', ('--mnk_sensitivity={0}' -f ([double]$s.Sens).ToString([Globalization.CultureInfo]::InvariantCulture)),
      '--keybind_right_trigger=LMB', '--keybind_left_trigger=RMB', '--keybind_a=Space', '--keybind_b=F', '--keybind_x=E',
      '--keybind_y=Q', '--keybind_right_shoulder=R', '--keybind_left_shoulder=Tab', '--keybind_dpad_left=1',
      '--keybind_dpad_right=2', '--keybind_dpad_up=3', '--keybind_dpad_down=4', '--keybind_rstick_press=MMB,Z',
      '--keybind_lstick_press=C', '--keybind_start=Escape,Return', '--keybind_back=Backspace')
  }
  # Frame rate. Gears 2: vsync off + our cap. Gears 3 (its job 049): 60 = vsync off and the game's own 62 cap; 120 /
  # unlimited add its cap. Judgment (its Play-Best.bat): 60 = vsync off and the game's own cap; 120 / unlimited not yet.
  $fps = [int]$s.Fps
  # Launcher patch 0155: with the PC vsync box, a cap (60 / 120 / Unlimited) at or above the screen's refresh becomes 3
  # under it (a vsync'd queue at the refresh rate adds lag; under it G-Sync / FreeSync stay in range). With AMD frame
  # generation each game frame is shown twice, so the game cap is half the refresh minus 2. All three games take any number
  # (Gears 3: its 10:54 mail - --gow3_max_fps N = N fps; Judgment: its 10:48 mail; launcher patch 0158).
  $cap = $fpsCaps[$fps]
  if ((& $on $cVsync) -and $fps -ge 1) {
    $scr = if ($s.Monitor -gt 0 -and $s.Monitor -le $screens.Count) { $screens[$s.Monitor - 1] } else { [Windows.Forms.Screen]::PrimaryScreen }
    $hz = 0; try { $hz = [GearsDisplay]::RefreshHz($scr.DeviceName) } catch {}
    $fgOn = $cFG.Enabled -and $cFG.SelectedIndex -eq 1
    $top = if ($fgOn) { [int][math]::Floor($hz / 2) - 2 } else { $hz - 3 }
    if ($isG3 -and $cap -eq 60) { $cap = 62 }   # review 0158: Gears 3's 60 setting is really its own 62 cap
    if ($hz -ge 50 -and ($cap -lt 0 -or $cap -gt $top)) { $cap = $top }
    $notes += "launcher: vsync on; $($scr.DeviceName) at $hz Hz (0 = unknown)$(if ($fgOn) { ', frame generation on' }) -> frame-rate cap $cap"
  }
  if (& $on $cVsync) { $a += '--d3d12_present_vsync=true' }
  if ($g -eq 0 -and $fps -gt 0) { $a += '--gow2_gpu_vsync=false'; $a += "--gow2_max_fps=$cap" }
  if ($isG3 -and $fps -gt 0) { $a += '--gow2_gpu_vsync=false'; if ($cap -ne $fpsCaps[$fps]) { $a += "--gow3_max_fps=$cap" } elseif ($fps -eq 2) { $a += '--gow3_max_fps=120' } elseif ($fps -eq 3) { $a += '--gow3_max_fps=-1' } }   # 0158: the vsync cap too
  if ($isGJ -and $fps -gt 0) { $a += '--gow2_gpu_vsync=false'; $a += "--gowj_max_fps=$cap" }   # its 20:45 mail: 0 game / N cap / -1 uncapped
  # Extra switches for a play session: one per line in out\extra-args.txt (# = comment, {LOG} = this session's folder).
  $extraFile = Join-Path $root 'out\extra-args.txt'
  if (Test-Path $extraFile) { $a += @(Get-Content $extraFile | ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith('#') } | ForEach-Object { $_.Replace('{LOG}', $logDir) }) }
  Set-Content (Join-Path $logDir 'args.txt') ($a -join "`r`n")
  Add-Content (Join-Path $logDir 'args.txt') "launcher: game $($gameNames[$g]), exe $exe"
  Add-Content (Join-Path $logDir 'args.txt') "launcher: graphics card(s): $($gpus -join ' / ')"
  foreach ($n in $notes) { Add-Content (Join-Path $logDir 'args.txt') $n }
  # Patch 0134: an exe built for AVX2 CPUs (its 'gn_cpu_level=x86-64-v3' text) on a CPU without AVX2 would die at once
  # with 0xC000001D; say so in plain English instead and do not start it. PF_AVX2_INSTRUCTIONS_AVAILABLE = 40.
  if ((Exe-Has $exe 'gn_cpu_level=x86-64-v') -and -not [GearsCpu]::HasAvx2()) {
    [void][Windows.Forms.MessageBox]::Show("This build of $($gameNames[$g]) needs a CPU with AVX2 (Intel Haswell / AMD Ryzen, about 2013 and newer), and this PC's CPU does not report it.`r`n`r`nIf this is a virtual machine, turn on passing the host CPU's features through to it.", 'CPU too old for this build', 'OK', 'Error')
    Add-Content (Join-Path $logDir 'args.txt') 'launcher: not started - the exe needs AVX2 and this CPU does not report it'
    return
  }
  $script:padOn = $false   # the launcher never reacts to the controller while the game runs
  $p = Start-Process -FilePath $exe -ArgumentList $a -WorkingDirectory (Split-Path $exe) -PassThru
  try { [void]$p.Handle } catch {}   # keeps the exit code readable after the game ends
  $script:game = @{ proc = $p; g = $g; log = $logDir }
  $f.Close()
})
# Patch 0131 (Athelas idea 16): after Play the launcher waits, out of sight, for the game to end. When it ends badly (a
# crash report, or an exit code other than 0) a plain-English message says what the code means and offers the report.
$exitNames = @{
  [uint32]3221225477 = 'it crashed (a memory access error, 0xC0000005)'
  [uint32]3221226505 = 'it stopped itself after a fatal error (0xC0000409)'
  [uint32]3221226356 = 'its memory got corrupted (0xC0000374)'
  [uint32]3221225501 = 'this CPU lacks an instruction the build needs (0xC000001D) - the ports are built for CPUs with AVX2 (2013 and newer)'
  [uint32]3221225781 = 'a DLL it needs is missing (0xC0000135) - try installing the latest Microsoft Visual C++ Redistributable (x64)'
  [uint32]3221225794 = 'a DLL it needs failed to start (0xC0000142)'
  [uint32]2289696773 = 'the graphics card stopped responding or its driver was reset (0x887A0005)'
  [uint32]2289696774 = 'the graphics card stopped responding (0x887A0006)'
  [uint32]2289696775 = 'the graphics driver was reset (0x887A0007)'
  [uint32]1 = 'it ended with an error, or was ended from outside (Task Manager)'
}
function Watch-Game {
  $p = $script:game.proc; $g = $script:game.g; $dir = $script:game.log
  try { $p.WaitForExit() } catch { return }
  $code = $null; try { $code = [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$p.ExitCode), 0) } catch {}
  $crash = @(Get-ChildItem -LiteralPath $dir -Filter 'crash-*.txt' -ErrorAction SilentlyContinue)
  try { Add-Content (Join-Path $dir 'args.txt') ("launcher: the game ended at $(Get-Date -Format 'HH:mm:ss'), exit code " + $(if ($null -ne $code) { '0x{0:X8}' -f $code } else { 'unknown' }) + ", crash reports $($crash.Count)") } catch {}
  if (($null -eq $code -or $code -eq 0) -and -not $crash.Count) { return }
  $what = if ($null -ne $code -and $exitNames.ContainsKey($code)) { $exitNames[$code] } elseif ($null -ne $code -and $code -ne 0) { 'it ended with exit code 0x{0:X8}' -f $code } else { 'it crashed' }
  $lg = Join-Path $dir @('gow2.log', 'gow3.log', 'gowj.log')[$g]
  if ((Test-Path $lg) -and (Select-String -Path $lg -Pattern 'DEVICE_REMOVED|DEVICE_HUNG|DXGI_ERROR_DEVICE' -Quiet)) { $what += '; its log mentions the graphics card being lost (a driver reset or an overheating / overclocked card)' }
  if ($crash.Count) { $what += '; it wrote a crash report' }
  $script:padOn = $true   # the controller can answer the message box
  $ans = [Windows.Forms.MessageBox]::Show("$($gameNames[$g]) closed unexpectedly: $what.`r`n`r`nMake a problem report (a zip on your desktop) for it?", "$($gameNames[$g]) closed", 'YesNo', 'Warning')
  if ($ans -eq 'Yes') { Make-Report $g $dir $what }
}
[void]$f.ShowDialog()
$padTimer.Stop()
if ($script:game) { $padTimer.Start(); Watch-Game; $padTimer.Stop() }
# Reset to defaults closes the window; open it again with the defaults.
if ($f.Tag -eq 'reset') { & $PSCommandPath }
