# kit_build.ps1 - builds a game's program from a build kit (make_build_kit.ps1) and the player's own copy of the game:
# what COGBox will do on a player's PC, written out as a script to prove and time it. No CMake, Ninja or sccache: the
# codegen, then clang on each recompiled file (in parallel, as many as the free memory allows), then one link.
#
# Usage: tools\kit_build.ps1 -Kit <kit> -Xex <folder with default.xex> [-Xexp <folder with default.xexp>] -Out <folder>
#        [-Clang <folder with clang++.exe>] [-Jobs N]
# The toolchain: the kit's pinned clang, and the Windows SDK and C runtime from a Visual Studio on this PC for now
# (COGBox will download them from Microsoft when there is none).
param(
  [Parameter(Mandatory)][string]$Kit,
  [Parameter(Mandatory)][string]$Xex,
  [string]$Xexp = '',
  [Parameter(Mandatory)][string]$Out,
  [string]$Clang = '',
  [int]$Jobs = 0
)
# (Continue: Windows PowerShell turns a native program's stderr line into a stopping error under Stop; every step
# checks its exit code instead)
$ErrorActionPreference = 'Continue'
$t0 = Get-Date
$Kit = (Resolve-Path $Kit).Path
$k = Get-Content -LiteralPath (Join-Path $Kit 'kit.json') -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$work = Join-Path $Out 'build'
if (Test-Path $work) { Remove-Item -LiteralPath $work -Recurse -Force }
$gameDir = Join-Path $work 'game'; $gen = Join-Path $work 'generated'; $objDir = Join-Path $work 'obj'
New-Item -ItemType Directory -Force $gameDir, $gen, $objDir | Out-Null
function Phase($what) { Write-Host ("[{0,5:N0} s] {1}" -f ((Get-Date) - $t0).TotalSeconds, $what) }

# ---- toolchain: the kit's clang, the Windows SDK / CRT from Visual Studio
if (-not $Clang) { throw '-Clang <folder with the kit''s clang++.exe> is needed for now' }
$clangxx = Join-Path $Clang 'clang++.exe'
$ver = & $clangxx --version | Select-Object -First 1
if ($ver -ne $k.clang) { throw "clang mismatch: the kit needs '$($k.clang)', this is '$ver'" }
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] } }
if ($env:VCToolsVersion -ne $k.msvc) { Write-Host "note: MSVC $env:VCToolsVersion here, the kit was made with $($k.msvc)" }
if ($Jobs -le 0) {
  $cpu = (Get-CimInstance Win32_Processor | Measure-Object NumberOfLogicalProcessors -Sum).Sum
  $freeGB = (Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB
  $Jobs = [Math]::Max(1, [Math]::Min($cpu, [int][Math]::Floor(($freeGB - 2) / 1.5)))   # ~1.5 GB a compile at -O3, 2 GB left over
  Write-Host ("{0} threads, {1:N1} GB free -> {2} compiles at a time" -f $cpu, $freeGB, $Jobs)
}

# ---- 1. the game's program file(s), and the manifest pointed at them
Copy-Item -LiteralPath (Join-Path $Xex 'default.xex') $gameDir
if ($Xexp) { Copy-Item -LiteralPath (Join-Path $Xexp 'default.xexp') $gameDir }
$m = Get-Content -LiteralPath (Join-Path $Kit 'manifest.toml') -Raw
$fwd = { param($p) $p -replace '\\', '/' }
$m = [regex]::Replace($m, '(?m)^(game_root\s*=\s*)"[^"]*"', ('${1}"' + (& $fwd $gameDir) + '"'))
$m = [regex]::Replace($m, '(?m)^(file_path\s*=\s*)"[^"]*"', ('${1}"' + (& $fwd (Join-Path $gameDir 'default.xex')) + '"'))
$m = [regex]::Replace($m, '(?m)^(out_directory_path\s*=\s*)"[^"]*"', ('${1}"' + (& $fwd $gen) + '"'))
$manifest = Join-Path $work 'manifest.toml'
Set-Content -LiteralPath $manifest -Value $m -Encoding utf8
Phase 'codegen'
Push-Location $work
& (Join-Path $Kit 'bin\rexglue.exe') codegen $manifest 2>&1 | Select-Object -Last 4 | ForEach-Object { Write-Host "  codegen: $_" }
$cg = $LASTEXITCODE
Pop-Location
if ($cg -ne 0) { throw "codegen failed ($cg)" }
$srcs = @(Get-ChildItem -LiteralPath $gen -Filter *.cpp | Sort-Object Length -Descending)
$pchH = Get-ChildItem -LiteralPath $gen -Filter '*_pch.h' | Select-Object -First 1
Phase ("codegen done: {0} files, {1:N0} MB" -f $srcs.Count, (($srcs | Measure-Object Length -Sum).Sum / 1MB))

# ---- 2. compile flags
$inc = @()
foreach ($i in $k.includes) {
  $p = switch ($i.kind) { 'gen' { $gen } 'sdk' { Join-Path $Kit 'include\sdk' } 'sdk-dxc' { Join-Path $Kit 'include\sdk\dxc' } 'project' { Join-Path $Kit 'include\project' } 'src' { Join-Path $Kit 'include\project\src' } }
  if ($i.sys) { $inc += '-isystem'; $inc += $p } else { $inc += "-I$p" }
}
$pgo = Join-Path $Kit 'pgo\profile.profdata'
$flags = @($k.flags | ForEach-Object { $_ -replace '\$\{PGO\}', $pgo })
$base = @($k.defines) + $inc + $flags

# the precompiled header, as CMake made it: a .hxx that includes the generated one
$pchHxx = Join-Path $work 'pch.hxx'; $pchCxx = Join-Path $work 'pch.hxx.cxx'; $pch = Join-Path $work 'pch.hxx.pch'
if (-not $pchH) { throw 'the codegen wrote no *_pch.h' }
Set-Content -LiteralPath $pchHxx -Value "#include `"$($pchH.FullName -replace '\\','/')`"" -Encoding ascii
Set-Content -LiteralPath $pchCxx -Value '/* precompiled header */' -Encoding ascii
Phase 'precompiled header'
& $clangxx @base -Xclang -emit-pch -Xclang -include -Xclang $pchHxx -x c++-header -o $pch -c $pchCxx
if ($LASTEXITCODE -ne 0) { throw 'precompiled header failed' }
$usePch = @('-Xclang', '-include-pch', '-Xclang', $pch, '-Xclang', '-include', '-Xclang', $pchHxx)

# ---- 3. compile, the biggest first, $Jobs at a time; lowest free memory sampled
Phase "compiling $($srcs.Count) files, $Jobs at a time"
$queue = New-Object System.Collections.Queue; $srcs | ForEach-Object { $queue.Enqueue($_) }
$running = @(); $done = 0; $failed = @(); $minFree = [double]::MaxValue; $lastShown = -1
while ($queue.Count -or $running.Count) {
  while ($running.Count -lt $Jobs -and $queue.Count) {
    $s = $queue.Dequeue()
    $o = Join-Path $objDir ($s.Name + '.obj')
    $cargs = $base + $usePch + @('-o', $o, '-c', $s.FullName)
    $p = Start-Process -FilePath $clangxx -ArgumentList ($cargs | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }) -NoNewWindow -PassThru `
      -RedirectStandardError (Join-Path $objDir ($s.Name + '.err'))
    $running += @{ p = $p; s = $s }
  }
  Start-Sleep -Milliseconds 500
  $free = (Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB
  if ($free -lt $minFree) { $minFree = $free }
  $still = @()
  foreach ($r in $running) {
    if ($r.p.HasExited) { $done++; if ($r.p.ExitCode -ne 0) { $failed += $r.s.Name } } else { $still += $r }
  }
  $running = $still
  $pct = [int](100 * $done / $srcs.Count)
  if ($pct -ne $lastShown -and $pct % 10 -eq 0) { Phase "  $pct%"; $lastShown = $pct }
}
if ($failed.Count) {
  foreach ($f in $failed | Select-Object -First 3) { Get-Content (Join-Path $objDir ($f + '.err')) -TotalCount 15 | ForEach-Object { Write-Host "  $f | $_" } }
  throw "$($failed.Count) files didn't compile"
}

# ---- 4. link
Phase 'link'
$objs = @(Get-ChildItem -LiteralPath $objDir -Filter *.obj | ForEach-Object FullName) + @($k.objects | ForEach-Object { Join-Path $Kit $_ })
$libs = @($k.libraries | ForEach-Object { Join-Path $Kit $_ }) + @($k.system_libraries | ForEach-Object { "-l$_" })
$exe = Join-Path $Out $k.exe
$rsp = Join-Path $work 'link.rsp'
(@($k.link_flags) + @('-o', $exe) + $objs + $libs) | ForEach-Object { if ($_ -match '\s') { "`"$($_ -replace '\\','/')`"" } else { $_ -replace '\\','/' } } | Set-Content -LiteralPath $rsp -Encoding ascii
& $clangxx "@$rsp"
if ($LASTEXITCODE -ne 0) { throw 'link failed' }
Phase ("built {0} ({1:N0} MB) | lowest free memory {2:N1} GB" -f $exe, ((Get-Item $exe).Length / 1MB), $minFree)
