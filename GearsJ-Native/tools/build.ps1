# GearsJ-Native build, copied from Gears3-Native's tools\build.ps1 (Shagg's own project, D5): Visual Studio environment + the portable
# LLVM/CMake installed for Gears 2 (read-only, tools\env.ps1), configure, codegen, compile.
# Below Normal priority (rule agreed with the Gears 2 chat); own sccache folder and port.
# Usage: tools\build.ps1 [-Config RelWithDebInfo|Release|Debug] [-Jobs N] [-KeepGoing] [-Layer0Home <folder> -Name <dir>]
#   -Layer0Home: build layer 0 from another folder (default: the shared copy in Gears2-Native, see CMakeLists.txt);
#   -Name: build folder name under build\ (default = Config), so an A/B build doesn't overwrite the normal one.
param([string]$Config = 'RelWithDebInfo', [int]$Jobs = 0, [switch]$KeepGoing, [string]$Layer0Home = '', [string]$Name = '')
. "$PSScriptRoot\env.ps1"
Use-VsEnv
if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }
$bd = Join-Path $GJRoot "build\$(if ($Name) { $Name } else { $Config })"
$l0 = @()  # (an if-expression would unwrap a one-item array into a string, which splats character by character)
if ($Layer0Home) { $l0 += "-DGN_LAYER0_HOME=$($Layer0Home.Replace('\', '/'))" }
Write-Host "BUILD sdk=$(Split-Path $Sdk -Leaf) config=$Config jobs=$Jobs priority=$((Get-Process -Id $PID).PriorityClass) | clang $(& "$LlvmBin\clang++.exe" --version | Select-Object -First 1) | MSVC $env:VCToolsVersion"
New-Item -ItemType Directory -Force -Path (Join-Path $GJRoot 'out') | Out-Null

$t = Get-Date
$cfg = & $Cmake -S $GJRoot -B $bd -G Ninja "-DCMAKE_BUILD_TYPE=$Config" "-DCMAKE_MAKE_PROGRAM=$Ninja" `
  "-DCMAKE_C_COMPILER=$LlvmBin\clang.exe" "-DCMAKE_CXX_COMPILER=$LlvmBin\clang++.exe" `
  "-DCMAKE_C_COMPILER_LAUNCHER=$Sccache" "-DCMAKE_CXX_COMPILER_LAUNCHER=$Sccache" "-DCMAKE_PREFIX_PATH=$Sdk" "-Drexglue_DIR=$Sdk\lib\cmake\rexglue" "-DGN_THIRD_PARTY=$G2Tp" @l0 2>&1
$cfgOk = $LASTEXITCODE -eq 0
$cfg | Select-Object -Last $(if ($cfgOk) { 6 } else { 40 }) | ForEach-Object { Write-Host "  cfg: $_" }
if (-not $cfgOk) { Write-Host "  configure FAILED (exit $LASTEXITCODE)" }
$tc = [int]((Get-Date)-$t).TotalSeconds

$t = Get-Date
$gen = & $Cmake --build $bd --target gowj_codegen 2>&1
$genOk = $LASTEXITCODE -eq 0
$gen | Out-File -LiteralPath (Join-Path $GJRoot 'out\codegen.log') -Encoding utf8
$gen | Select-String 'Unresolved|error|Error|warn' | Select-Object -First 30 | ForEach-Object { Write-Host "  gen: $($_.Line)" }
$gen | Select-Object -Last 4 | ForEach-Object { Write-Host "  gen: $_" }
$tg = [int]((Get-Date)-$t).TotalSeconds

$tb = 0; $ok = $false
if ($genOk) {
  & $Cmake -S $GJRoot -B $bd 2>&1 | Out-Null   # pick up generated/default/sources.cmake
  $t = Get-Date
  $kg = if ($KeepGoing) { @('--', '-k', '0') } else { @() }
  $out = & $Cmake --build $bd --parallel $Jobs @kg 2>&1
  $ok = $LASTEXITCODE -eq 0
  $out | Out-File -LiteralPath (Join-Path $GJRoot 'out\build.log') -Encoding utf8
  $tb = [int]((Get-Date)-$t).TotalSeconds
  Write-Host "  warnings: $(($out | Select-String 'warning:' | Measure-Object).Count)"
  $errs = $out | Select-String 'error:|FAILED:|undefined symbol|unresolved external'
  Write-Host "  error lines: $(($errs | Measure-Object).Count)"
  $errs | Select-Object -First 60 | ForEach-Object { Write-Host "  ERR $($_.Line)" }
  if (-not $ok) { $out | Select-Object -Last 15 | ForEach-Object { Write-Host "  | $_" } }
}
$exe = Join-Path $bd 'gowj.exe'
Write-Host ("BUILD RESULT ok={0} | configure {1} s, codegen {2} s, compile {3} s | exe {4}" -f $ok, $tc, $tg, $tb, $(if (Test-Path $exe) { "{0} ({1:N0} MB)" -f $exe, ((Get-Item $exe).Length/1MB) } else { 'missing' }))
& $Sccache --show-stats 2>&1 | Select-String 'Compile requests executed|Cache hits  |Cache misses  ' | ForEach-Object { Write-Host "  sccache: $($_.Line.Trim())" }
if ($ok) { exit 0 } else { exit 1 }
