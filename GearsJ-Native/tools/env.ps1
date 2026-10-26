# GearsJ-Native (Judgment) tool environment, adapted from Gears3-Native's env.ps1 (same project, MIT).
# The toolchain is the one installed for Gears 2, used READ-ONLY from Documents\Gears2-Native\third_party.
# Dot-source it: . "$PSScriptRoot\env.ps1"   Sets $GJRoot, $G2Root, $G3Root, $G2Tp, $Sdk, $Rexglue, $LlvmBin, $Cmake,
# $Ninja, $Sccache, $VsPath, $PcLock; Use-VsEnv loads the Visual Studio x64 environment. Throws if something is missing.
$ErrorActionPreference = 'Stop'
$GJRoot = Split-Path -Parent $PSScriptRoot
$G2Root = Join-Path (Split-Path -Parent $GJRoot) 'Gears2-Native'
$G3Root = Join-Path (Split-Path -Parent $GJRoot) 'Gears3-Native'
$G2Tp = Join-Path $G2Root 'third_party'
if (-not (Test-Path $G2Tp)) { throw "ENV STOP: $G2Tp not found" }
# The shared PC lock lives with Gears 3 (Gears 2 and Gears 3 both use this one file); a copy would not lock anything.
$PcLock = Join-Path $G3Root 'tools\sharedpc\pclock.ps1'
# SDK: our own snapshot named in third_party\sdk.txt (job 001 makes it). Before that, nothing builds.
$sdkChoice = Join-Path $GJRoot 'third_party\sdk.txt'
$first = (Get-Content $sdkChoice -TotalCount 1 -ErrorAction SilentlyContinue)
if ($first -and $first.Trim() -like 'rexglue-install-*') { $Sdk = Join-Path $GJRoot ('third_party\' + $first.Trim()) } else { $Sdk = $null }
$Rexglue = if ($Sdk) { Join-Path $Sdk 'bin\rexglue.exe' } else { $null }
$LlvmBin = (Get-ChildItem (Join-Path $G2Tp 'llvm-23.1.4') -Recurse -Filter 'clang++.exe' | Select-Object -First 1).DirectoryName
$Cmake = (Get-ChildItem (Join-Path $G2Tp 'cmake-4.4.4') -Recurse -Filter 'cmake.exe' | Select-Object -First 1).FullName
$Sccache = (Get-ChildItem (Join-Path $G2Tp 'sccache-v0.18.0') -Recurse -Filter 'sccache.exe' | Select-Object -First 1).FullName
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$VsPath = & $vswhere -latest -property installationPath
$Ninja = "$VsPath\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
foreach ($p in @($LlvmBin, $Cmake, $Sccache, $Ninja, $PcLock)) { if (-not $p -or -not (Test-Path $p)) { throw "ENV STOP: tool missing ($p)" } }
# sccache: our own cache folder and port (Gears 3 uses 4237). Job 001 checks nothing else listens on it.
$env:SCCACHE_DIR = Join-Path $GJRoot 'out\sccache'; $env:SCCACHE_CACHE_SIZE = '30G'; $env:SCCACHE_SERVER_PORT = '4239'
$env:PATH = "$LlvmBin;$(Split-Path $Cmake);" + $env:PATH
try { (Get-Process -Id $PID).PriorityClass = 'BelowNormal' } catch {}
function Use-VsEnv {
  cmd /c "`"$VsPath\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] } }
}
# Wait for Gears 2's queue to be idle (guidebook: their queue heartbeat 'running=-', no queued .ps1).
# 15:52 (Shagg 15:49, "ask the other projects how they do their jobs"): Gears 3's jobs don't wait for Gears 2's queue
# at all - the shared PC lock (Shagg's OK 08:17) is the only turn-taking, and it takes turns between the three games.
# Our extra wait for an empty Gears 2 queue starved Judgment while Gears 2 queued 40-minute jobs back to back
# (job 017 waited from 15:20). Kept as a no-op so existing jobs still run; Take-PcLock does the waiting now.
function Wait-Gears2Idle([int]$MaxMinutes = 0) {
  Write-Host "  (no Gears 2 queue wait - the shared PC lock decides, as in Gears 3)"
  return $true
}
$ErrorActionPreference = 'Continue'
