# make_build_kit.ps1 - the build kit for one game version: everything a player's PC needs to build the game's program
# from their own copy, and nothing made from the game.
#
# Usage (after a normal tools\build.ps1 of that version):
#   tools\make_build_kit.ps1 -Project C:\COGBox\Gears2-Native -BuildDir build\Release-tu6 -Manifest gow2_manifest_tu6.toml -Out <kit folder>
#
# The kit:
#   kit.json        the exact compile and link commands of the build it came from, with paths made relative to the kit
#   manifest.toml   the recompiler manifest (its game paths are filled in on the player's PC)
#   bin\            rexglue.exe (the codegen tool) and what it needs
#   include\        the headers the recompiled code includes: ours (src\...) and the SDK's
#   obj\            our compiled code that goes into the program (layer 3, the SDK's app entry), already built
#   lib\            the libraries it links: our layers 0-2, the SDK runtime, SDL3, spdlog, fmt, nvapi, DLSS
#   pgo\            the profile the recompiled code is optimised with (counts from test runs: no game code or data)
# Not in it: the recompiled code (generated\), the game's files, the program itself. The script refuses to copy any
# .xex / .xexp / generated source, and every file it copies is listed in kit.json's "files" with its SHA-256.
param(
  [Parameter(Mandatory)][string]$Project,
  [Parameter(Mandatory)][string]$BuildDir,
  [Parameter(Mandatory)][string]$Manifest,
  [Parameter(Mandatory)][string]$Out,
  [string]$Exe = ''
)
$ErrorActionPreference = 'Stop'
$Project = (Resolve-Path $Project).Path.TrimEnd('\')
$bd = (Resolve-Path (Join-Path $Project $BuildDir)).Path.TrimEnd('\')
$ninja = Get-Content -LiteralPath (Join-Path $bd 'build.ninja') -Raw
if (-not $Exe) { $Exe = ([regex]::Match($ninja, '(?m)^build ([A-Za-z0-9_]+)\.exe: CXX_EXECUTABLE_LINKER__')).Groups[1].Value }
if (-not $Exe) { throw 'no executable in build.ninja' }
Write-Host "kit for $Exe from $bd"

# ---- build.ninja: an edge's line and its indented variables
function Edge([string]$startsWith) {
  $i = $ninja.IndexOf("`nbuild $startsWith")
  if ($i -lt 0) { throw "no edge: build $startsWith" }
  $lines = $ninja.Substring($i + 1).Split("`n")
  $vars = @{}; $head = $lines[0].TrimEnd("`r")
  for ($k = 1; $k -lt $lines.Count; $k++) {
    $l = $lines[$k].TrimEnd("`r")
    if ($l -notmatch '^  (\w+) = (.*)$') { break }
    $vars[$matches[1]] = $matches[2]
  }
  return @{ head = $head; vars = $vars }
}
function Unescape([string]$p) { $p -replace '\$:', ':' -replace '\$ ', ' ' }
function Norm([string]$p) { (Unescape $p) -replace '\\', '/' }

$link = Edge "$Exe.exe:"
$genObj = [regex]::Match($ninja, "(?m)^build (CMakeFiles/${Exe}_recomp\.dir/[^:]*_recomp\.0\.cpp\.obj):").Groups[1].Value
if (-not $genObj) { throw 'no recompiled-code object in build.ninja' }
$comp = Edge "$($genObj):"

# the link inputs: before '|' are explicit inputs, after it the libraries (implicit), '||' order-only
$inputs = ($link.head -replace '^build [^:]+: \S+\s*', '') -split '\s+\|\|?\s+' | Select-Object -First 1
$objs = @($inputs -split '\s+' | Where-Object { $_ -and $_ -notmatch "${Exe}_recomp\.dir/" })
$libsLine = $link.vars['LINK_LIBRARIES']

# ---- the kit folder
if (Test-Path $Out) { Remove-Item -LiteralPath $Out -Recurse -Force }
foreach ($d in 'bin', 'include', 'obj', 'lib', 'pgo') { New-Item -ItemType Directory -Force (Join-Path $Out $d) | Out-Null }
$files = [ordered]@{}
function Put([string]$from, [string]$rel) {
  # (the project's generated\ folder is the recompiled game: never in a kit; an SDK folder that happens to be called
  # "generated" is fine)
  if ($from -match '\.(xex|xexp|iso)$' -or $from.StartsWith((Join-Path $Project 'generated'), [StringComparison]::OrdinalIgnoreCase)) { throw "refused (made from the game): $from" }
  $to = Join-Path $Out $rel
  New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
  Copy-Item -LiteralPath $from -Destination $to -Force
  $files[$rel -replace '\\', '/'] = (Get-FileHash -Algorithm SHA256 -LiteralPath $to).Hash
}

# objects: our prebuilt code that goes into the program
$objRel = @()
foreach ($o in $objs) {
  $src = Join-Path $bd (Unescape $o)
  $rel = 'obj\' + (Split-Path $o -Leaf)
  if ($objRel -contains ($rel -replace '\\', '/')) { throw "two objects named $(Split-Path $o -Leaf)" }
  Put $src $rel; $objRel += ($rel -replace '\\', '/')
}
# libraries: the ones with a path are copied; bare names (-lkernel32.lib, gn_layer0_xenos.lib in the build dir) too when
# they're files of ours, else they are Windows SDK / CRT libraries the toolchain provides
$libOut = @(); $sysLibs = @()
foreach ($t in ($libsLine -split '\s+' | Where-Object { $_ })) {
  $p = Norm $t
  if ($p -match '^-l(.+)$') { $sysLibs += ($matches[1] -replace '\.lib$', ''); continue }
  $cand = if ([IO.Path]::IsPathRooted($p)) { $p } else { Join-Path $bd $p }
  if (Test-Path -LiteralPath $cand) {
    $rel = 'lib\' + (Split-Path $cand -Leaf)
    if (-not ($libOut -contains ($rel -replace '\\', '/'))) { Put $cand $rel; $libOut += ($rel -replace '\\', '/') }
  } else { $sysLibs += ($p -replace '\.lib$', '') }
}
$sysLibs = @($sysLibs | Select-Object -Unique)

# compile flags of the recompiled code, with the build's own paths swapped for kit tokens
$v = $comp.vars
$defines = @($v['DEFINES'] -split '\s+' | Where-Object { $_ })
$flags = @(($v['FLAGS'] -split '\s+') | Where-Object { $_ })
$incs = @()
$sdkInc = $null
$tokens = $v['INCLUDES'] -split '\s+' | Where-Object { $_ }
for ($k = 0; $k -lt $tokens.Count; $k++) {
  $t = $tokens[$k]
  if ($t -eq '-isystem') { $k++; $p = Norm $tokens[$k]; $sys = $true } elseif ($t -like '-I*') { $p = Norm $t.Substring(2); $sys = $false } else { continue }
  if ($p -match '/generated/') { $incs += @{ kind = 'gen'; sys = $sys }; continue }
  if ($p -match '/include/dxc$') { $incs += @{ kind = 'sdk-dxc'; sys = $sys }; continue }
  if ($p -match '/include$') { $sdkInc = $p; $incs += @{ kind = 'sdk'; sys = $sys }; continue }
  if ($p -eq (Norm $Project)) { $incs += @{ kind = 'project'; sys = $sys }; continue }
  if ($p -eq ((Norm $Project) + '/src')) { $incs += @{ kind = 'src'; sys = $sys }; continue }
  throw "unknown include folder $p"
}
# drop what the kit's builder puts back itself: the PCH use and the profile path
$keep = @(); $profile = $null
for ($k = 0; $k -lt $flags.Count; $k++) {
  $f = $flags[$k]
  if ($f -eq '-Xclang' -and $k + 1 -lt $flags.Count -and $flags[$k + 1] -in '-include-pch', '-include') { $k += 3; continue }
  if ($f -like '-fprofile-instr-use=*' -or $f -like '-fprofile-use=*') { $profile = Norm ($f -replace '^-fprofile-(instr-)?use=', ''); $keep += ($f -replace '=.*$', '=${PGO}'); continue }
  $keep += $f
}
if ($profile) { Put $profile 'pgo\profile.profdata' }

# headers: ours (everything under the project except third_party, build, out, generated, game) and the SDK's include
Get-ChildItem -LiteralPath $Project -Recurse -File -Include *.h, *.hpp, *.inl -ErrorAction SilentlyContinue |
  Where-Object { $_.FullName.Substring($Project.Length) -notmatch '^\\(third_party|build|out|generated|game|tests|cogbox)\\' } |
  ForEach-Object { Put $_.FullName ('include\project\' + $_.FullName.Substring($Project.Length + 1)) }
if ($sdkInc) {
  $sdkIncWin = $sdkInc -replace '/', '\'
  Get-ChildItem -LiteralPath $sdkIncWin -Recurse -File | ForEach-Object { Put $_.FullName ('include\sdk\' + $_.FullName.Substring($sdkIncWin.Length + 1)) }
  $sdkRoot = Split-Path $sdkIncWin
  Put (Join-Path $sdkRoot 'bin\rexglue.exe') 'bin\rexglue.exe'
}
Put (Join-Path $Project $Manifest) 'manifest.toml'

# the clang the build used: the player's PC must use exactly this one
$clang = ([regex]::Match($ninja, '(?m)command = .*?([A-Za-z]:\\[^ ]*\\clang\+\+\.exe)')).Groups[1].Value
$clangVer = (& $clang --version | Select-Object -First 1)
$kit = [ordered]@{
  format = 1
  exe = "$Exe.exe"
  made_from = (Split-Path $bd -Leaf)
  clang = $clangVer
  msvc = $env:VCToolsVersion
  defines = $defines
  flags = $keep
  includes = $incs
  link_flags = @(($link.vars['FLAGS'] -split '\s+') + ($link.vars['LINK_FLAGS'] -split '\s+') | Where-Object { $_ })
  objects = $objRel
  libraries = $libOut
  system_libraries = $sysLibs
  files = $files
}
$kit | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $Out 'kit.json') -Encoding utf8
"kit: {0} files, {1:N0} MB, {2} objects, {3} libraries, {4} system libraries | {5} | MSVC {6}" -f $files.Count, ((Get-ChildItem $Out -Recurse -File | Measure-Object Length -Sum).Sum / 1MB), $objRel.Count, $libOut.Count, $sysLibs.Count, $clangVer, $env:VCToolsVersion
