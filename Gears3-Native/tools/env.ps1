# Gears3-Native tool environment. The toolchain is the one installed for Gears 2 and is used READ-ONLY from
# Documents\Gears2-Native\third_party (agreed 2026-10-21 21:01: never write into the Gears 2 project).
# Dot-source it: . "$PSScriptRoot\env.ps1"   Sets $G3Root, $G2Tp, $Sdk, $Rexglue, $LlvmBin, $Cmake, $Ninja, $Sccache,
# $Xdvdfs, $VsPath; Use-VsEnv loads the Visual Studio x64 environment. Throws (stops the job) if something is missing.
$ErrorActionPreference = 'Stop'
$G3Root = Split-Path -Parent $PSScriptRoot
# Gears2-Native sits next to this project. (Not [Environment]::GetFolderPath('MyDocuments'): that is the OneDrive
# Documents folder on this PC - job 001.)
$G2Tp = Join-Path (Split-Path -Parent $G3Root) 'Gears2-Native\third_party'
if (-not (Test-Path $G2Tp)) { throw "ENV STOP: $G2Tp not found" }
# The SDK install: our own snapshot of Gears 2's install when third_party\sdk.txt names one (job 010: Gears 2 jobs
# reinstall theirs whenever their SDK patches change), otherwise Gears 2's live install (RelWithDebInfo runtime +
# Release tool, their SDK patches).
$sdkChoice = Join-Path $G3Root 'third_party\sdk.txt'
if (Test-Path $sdkChoice) { $Sdk = Join-Path $G3Root ('third_party\' + (Get-Content $sdkChoice -TotalCount 1).Trim()) }
else { $Sdk = Join-Path $G2Tp 'rexglue-install-llvm23-rwdi' }
$Rexglue = Join-Path $Sdk 'bin\rexglue.exe'
if (-not (Test-Path $Rexglue)) { throw "ENV STOP: $Rexglue not found" }
$LlvmBin = (Get-ChildItem (Join-Path $G2Tp 'llvm-23.1.4') -Recurse -Filter 'clang++.exe' | Select-Object -First 1).DirectoryName
$Cmake = (Get-ChildItem (Join-Path $G2Tp 'cmake-4.4.4') -Recurse -Filter 'cmake.exe' | Select-Object -First 1).FullName
$Sccache = (Get-ChildItem (Join-Path $G2Tp 'sccache-v0.18.0') -Recurse -Filter 'sccache.exe' | Select-Object -First 1).FullName
$Xdvdfs = Join-Path $G2Tp 'xdvdfs-v0.8.3\xdvdfs.exe'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$VsPath = & $vswhere -latest -property installationPath
$Ninja = "$VsPath\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
foreach ($p in @($LlvmBin, $Cmake, $Sccache, $Ninja)) { if (-not $p -or -not (Test-Path $p)) { throw "ENV STOP: tool missing ($p)" } }
# sccache: our own cache folder and server port, so Gears 3 never writes Gears 2's cache (its out\sccache).
$env:SCCACHE_DIR = Join-Path $G3Root 'out\sccache'; $env:SCCACHE_CACHE_SIZE = '30G'; $env:SCCACHE_SERVER_PORT = '4237'
$env:PATH = "$LlvmBin;$(Split-Path $Cmake);" + $env:PATH
try { (Get-Process -Id $PID).PriorityClass = 'BelowNormal' } catch {}
function Use-VsEnv {
  cmd /c "`"$VsPath\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] } }
}
$ErrorActionPreference = 'Continue'
