# Gears2-Native build: Visual Studio environment + portable LLVM/CMake, configure, codegen + compile.
# Usage: tools\build.ps1 [-Config RelWithDebInfo|Release|Debug] [-Jobs N]
# Measures the time of each phase and the lowest free RAM seen while building (R12: size jobs so nothing swaps).
param([string]$Config = 'RelWithDebInfo', [int]$Jobs = 0, [switch]$KeepGoing, [switch]$DebugSdk, [switch]$Tu6, [string]$CMakeDefs = '')   # -CMakeDefs 'A=ON B=OFF': extra -D options (kept in the CMake cache)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$tp = Join-Path $root 'third_party'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -property installationPath
if (-not $vs) { Write-Host "BUILD STOP: Visual Studio not found"; exit 2 }
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] } }
$llvmBin = (Get-ChildItem (Join-Path $tp 'llvm-23.1.4') -Recurse -Filter 'clang++.exe' | Select-Object -First 1).DirectoryName
$cmake   = (Get-ChildItem (Join-Path $tp 'cmake-4.4.4') -Recurse -Filter 'cmake.exe' | Select-Object -First 1).FullName
$ninja   = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$sccache = (Get-ChildItem (Join-Path $tp 'sccache-v0.18.0') -Recurse -Filter 'sccache.exe' | Select-Object -First 1).FullName
# Release SDK install. (A RelWithDebInfo install was tried in job 024: its exported rex::rexglue tool target has no
# RelWithDebInfo location, so the game project can't configure against it.)
$sdk     = Join-Path $tp 'rexglue-install-llvm23'
# Debug-info SDK (job 032): RelWithDebInfo runtime + the Release tool in one prefix (workaround from notes/31, Brief 1).
# Used when -DebugSdk is passed or the file 'use-rwdi-sdk' exists in third_party.
if ($DebugSdk -or (Test-Path (Join-Path $tp 'use-rwdi-sdk'))) { $sdk = Join-Path $tp 'rexglue-install-llvm23-rwdi' }
$env:SCCACHE_DIR = Join-Path $root 'out\sccache'; $env:SCCACHE_CACHE_SIZE = '30G'
$env:PATH = "$llvmBin;$(Split-Path $cmake);" + $env:PATH
# R13 (user, 14:33): high priority, use as many threads as help. Default = every logical thread; the free-RAM log shows if that ever swaps.
try { (Get-Process -Id $PID).PriorityClass = 'High' } catch {}
if ($Jobs -le 0) { $Jobs = (Get-CimInstance Win32_Processor | Measure-Object NumberOfLogicalProcessors -Sum).Sum }
# -Tu6 (notes/63): the Title Update 6 build - gow2_manifest_tu6.toml, generated\tu6, build\<Config>-tu6.
$bd = Join-Path $root "build\$Config$(if ($Tu6) { '-tu6' })"
$genDir = Join-Path $root $(if ($Tu6) { 'generated\tu6' } else { 'generated\default' })
$logSfx = if ($Tu6) { '-tu6' } else { '' }   # out\codegen-tu6.log / build-tu6.log keep the base logs
Write-Host "BUILD sdk=$(Split-Path $sdk -Leaf) config=$Config game=$(if ($Tu6) { 'tu6' } else { 'base' }) jobs=$Jobs | clang $(& "$llvmBin\clang++.exe" --version | Select-Object -First 1) | MSVC $env:VCToolsVersion"

# Free-RAM sampler (every 3 s) for the whole build.
$memLog = Join-Path $root 'out\build-mem.txt'
New-Item -ItemType Directory -Force -Path (Split-Path $memLog) | Out-Null
$sampler = Start-Job -ArgumentList $memLog -ScriptBlock { param($f) Remove-Item $f -ErrorAction SilentlyContinue; while ($true) { (Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory | Add-Content $f; Start-Sleep 3 } }

$t = Get-Date
& $cmake -S $root -B $bd -G Ninja "-DCMAKE_BUILD_TYPE=$Config" "-DCMAKE_MAKE_PROGRAM=$ninja" `
  "-DCMAKE_C_COMPILER=$llvmBin\clang.exe" "-DCMAKE_CXX_COMPILER=$llvmBin\clang++.exe" `
  "-DCMAKE_C_COMPILER_LAUNCHER=$sccache" "-DCMAKE_CXX_COMPILER_LAUNCHER=$sccache" "-DCMAKE_PREFIX_PATH=$sdk" "-Drexglue_DIR=$sdk\lib\cmake\rexglue" `
  "-DGN_GAME_VERSION=$(if ($Tu6) { 'tu6' } else { 'base' })" @(($CMakeDefs -split '\s+') | Where-Object { $_ } | ForEach-Object { "-D$_" }) 2>&1 |
  Select-Object -Last 6 | ForEach-Object { Write-Host "  cfg: $_" }
$tc = [int]((Get-Date)-$t).TotalSeconds

$t = Get-Date
$gen = & $cmake --build $bd --target gow2_codegen 2>&1
$genOk = $LASTEXITCODE -eq 0
$gen | Out-File -LiteralPath (Join-Path $root "out\codegen$logSfx.log") -Encoding utf8
$gen | Select-String 'Unresolved|error|Error|warn' | Select-Object -First 15 | ForEach-Object { Write-Host "  gen: $($_.Line)" }
$gen | Select-Object -Last 3 | ForEach-Object { Write-Host "  gen: $_" }
$tg = [int]((Get-Date)-$t).TotalSeconds

$tb = 0; $ok = $false
if ($genOk) {
  # Re-configure so CMake picks up generated/default/sources.cmake, then compile everything.
  & $cmake -S $root -B $bd 2>&1 | Out-Null
  $t = Get-Date
  $kg = if ($KeepGoing) { @('--', '-k', '0') } else { @() }
  $out = & $cmake --build $bd --parallel $Jobs @kg 2>&1
  $out | Out-File -LiteralPath (Join-Path $root "out\build$logSfx.log") -Encoding utf8
  $ok = $LASTEXITCODE -eq 0
  $tb = [int]((Get-Date)-$t).TotalSeconds
  $warn = ($out | Select-String 'warning:' | Measure-Object).Count
  $out | Select-String 'error:|FAILED:|undefined symbol|unresolved external' | Select-Object -First 20 | ForEach-Object { Write-Host "  ERR $($_.Line)" }
  if (-not $ok) { $out | Select-Object -Last 15 | ForEach-Object { Write-Host "  | $_" } }
  Write-Host "  warnings: $warn"
  # Freshness check (D27 lesson 3, from gears-judgement-recomp): a generated source newer than its object means ninja
  # linked a stale object (the patch or hook in that file would silently be missing). Fail loudly.
  if ($ok) {
    $stale = @(); $objs = @{}
    Get-ChildItem (Join-Path $bd 'CMakeFiles') -Recurse -Filter '*.cpp.obj' -ErrorAction SilentlyContinue | ForEach-Object { $objs[$_.Name] = $_ }
    foreach ($src in Get-ChildItem $genDir -Filter '*.cpp') {
      $obj = $objs[$src.Name + '.obj']
      if ($obj -and $src.LastWriteTime -gt $obj.LastWriteTime) { $stale += $src.Name }
    }
    if ($stale.Count) { $ok = $false; Write-Host "  ERR STALE objects (generated source newer than object): $($stale -join ', ')" }
    else { Write-Host "  freshness: every generated source is older than its object" }
  }
}
Stop-Job $sampler; Remove-Job $sampler
$mem = Get-Content $memLog -ErrorAction SilentlyContinue | ForEach-Object { [double]$_ }
if ($mem) { Write-Host ("  free RAM during build: lowest {0:N1} GB, highest {1:N1} GB" -f (($mem | Measure-Object -Minimum).Minimum/1MB), (($mem | Measure-Object -Maximum).Maximum/1MB)) }
$exe = Join-Path $bd 'gow2.exe'
Write-Host ("BUILD RESULT ok={0} | configure {1} s, codegen {2} s, compile {3} s | exe {4}" -f $ok, $tc, $tg, $tb, $(if (Test-Path $exe) { "{0} ({1:N0} MB)" -f $exe, ((Get-Item $exe).Length/1MB) } else { 'missing' }))
& $sccache --show-stats 2>&1 | Select-String 'Compile requests executed|Cache hits  |Cache misses  ' | ForEach-Object { Write-Host "  sccache: $($_.Line.Trim())" }
if ($ok) { exit 0 } else { exit 1 }
