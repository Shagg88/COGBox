# CogBox: assemble the player zip (CogBox-<version>.zip + .sha256) from the launcher and the four ports' builds.
# Run on the PC: pwsh -File cogbox\tools\package_cogbox.ps1 [-Version 0.1.0] [-Out <folder>] [-WhatIf]
# The zip and its .sha256 land in COGBox-Project\releases\<version>\; the
# unzipped working copy is built in Gears2-Native\out\pack\. A new build of the same version replaces the old files.
# Nothing of the games goes in: the script refuses to pack any .xex / .xexp / .iso / disc folder, and only copies the
# files named in the lists below (never a whole build folder, so test exes, logs, saves and shader caches stay out).
param(
  [string]$Version = '',
  [string]$Out = '',
  [switch]$WhatIf,
  [switch]$NoNormalCode   # cg050: pack without the "-normal" exes (the launcher then always runs the faster build, with no fallback)
)
$ErrorActionPreference = 'Stop'
$docs = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSCommandPath)))   # ...\COGBox-Project (the project folder)
$cogbox = Split-Path -Parent (Split-Path -Parent $PSCommandPath)                                          # ...\Gears2-Native\cogbox

# ---- What goes in ---------------------------------------------------------------------------------------------------
# Each port: the folder it goes to in the zip, the build it comes from; the exe and every DLL beside it go in (the SDK
# runtime - rexruntime.dll, or rexruntimerd.dll + TracyClientrd.dll in the debug-info SDK builds Gears 3 uses - the
# shader compiler and the vendor upscalers; Windows looks for DLLs next to the exe). Never .pdb, logs or old exes.
$needDlls = @('dxcompiler.dll', 'dxil.dll')   # plus a rexruntime*.dll, checked below
$ports = @(
  @{ to = 'Gears2\port-tu6';  from = "$docs\Gears2-Native\build\Release-tu6";       exe = 'gow2.exe' },
  # 0.4.0 (Shagg): title updates are required, so the Gears 2 / Gears 3 disc builds (port-disc) no longer ship
  @{ to = 'Gears3\port-tu6';  from = "$docs\Gears3-Native\build\Release-tu6";       exe = 'gow3.exe' },
  @{ to = 'Judgment\port';    from = "$docs\GearsJ-Native\build\Release";           exe = 'gowj.exe' },
  # 0.3.0: Gears 1. Its build has no upscaler or shader-compiler DLLs of its own (noShared): it uses shared\ like the
  # others, so those five files are checked and taken from the other builds only.
  @{ to = 'Gears1\port';      from = "$docs\Gears1-Native\build\Release";           exe = 'gow1.exe'; noShared = $true }
)
# Licences: CogBox's own (fonts, Dear ImGui, SDL3, miniz, stb), each project's LICENSE and THIRD_PARTY_NOTICES.md, and
# the licence files found at the top of each SDK whose code or DLLs ship.
$projects = @('Gears2-Native', 'Gears3-Native', 'GearsJ-Native', 'Gears1-Native')
# 0.4.9 (deps049): the vendor SDK versions the games build with (cmake/gn_deps.cmake) - their licence folders, and below
# a check that every port ships exactly those DLLs (a stale build folder would otherwise ship the old ones).
$sdks = @('rexglue-sdk', 'dlss-sdk', 'fidelityfx-sdk', 'xess-sdk-3.0.2', 'dxc-1.9.2609', 'antilag2-sdk', 'nvapi', 'streamline-sdk', 'agility-sdk-1.620.1')
$tpd = "$docs\Gears2-Native\third_party"
$xessSdk = @(Get-ChildItem -LiteralPath "$tpd\xess-sdk-3.0.2" -Recurse -Depth 2 -Filter libxess.dll -File -ErrorAction SilentlyContinue | Where-Object { $_.Directory.Name -eq 'bin' }) | Select-Object -First 1
$pinned = [ordered]@{
  'D3D12\D3D12Core.dll' = "$tpd\agility-sdk-1.620.1\build\native\bin\x64\D3D12Core.dll"   # must match the exe's D3D12SDKVersion 620
  'dxcompiler.dll'       = "$tpd\dxc-1.9.2609\bin\x64\dxcompiler.dll"
  'dxil.dll'             = "$tpd\dxc-1.9.2609\bin\x64\dxil.dll"
  'libxess.dll'          = $(if ($xessSdk) { $xessSdk.FullName } else { "$tpd\xess-sdk-3.0.2\bin\libxess.dll" })
}
$launcherSdl = '3.4.20'   # SDL3.dll beside CogBox.exe (SDL3-devel-3.4.20-mingw, the one build.sh links)

# ---- Checks first ---------------------------------------------------------------------------------------------------
$problems = @()
$launcher = Join-Path $cogbox 'build'
foreach ($f in 'CogBox.exe', 'SDL3.dll') { if (-not (Test-Path (Join-Path $launcher $f))) { $problems += "missing launcher file: $launcher\$f" } }
if (-not $Version) {
  $m = Select-String -Path (Join-Path $cogbox 'src\app.h') -Pattern '#define COGBOX_VERSION "([0-9.]+)"' | Select-Object -First 1
  if ($m) { $Version = $m.Matches[0].Groups[1].Value } else { $problems += 'no version (src\app.h COGBOX_VERSION)' }
}
foreach ($p in $ports) {
  if (-not (Test-Path (Join-Path $p.from $p.exe))) { $problems += "missing port: $($p.from)\$($p.exe)" }
  if (-not (Test-Path (Join-Path $p.from 'D3D12\D3D12Core.dll'))) { $problems += "no D3D12\D3D12Core.dll beside $($p.from)\$($p.exe) (Agility SDK, 0.2.0)" }
  foreach ($f in $needDlls) { if ($p.noShared -and $f -eq 'dxcompiler.dll') { continue }; if (-not (Test-Path (Join-Path $p.from $f))) { $problems += "missing next to $($p.to): $($p.from)\$f" } }
  if (-not @(Get-ChildItem -LiteralPath $p.from -Filter 'rexruntime*.dll' -File -ErrorAction SilentlyContinue).Count) { $problems += "no rexruntime*.dll in $($p.from)" }
}
# cg050 (faster game code, always on; the launcher falls back to the normal build after repeated crashes): each port also ships <stem>-normal.exe - the same commit built
# without the faster recompiler option, made by tools\build_normal_code.ps1 into <build folder>-normal\ with
# normal-code.txt. It must be from this checkout's commit (a stale one would not match the runtime and DLLs shipped).
$head = (git -C $cogbox rev-parse HEAD 2>$null | Select-Object -First 1)
foreach ($p in $ports) {
  $p.normal = ''
  if ($NoNormalCode) { continue }
  $nd = "$($p.from)-normal"; $ne = Join-Path $nd $p.exe; $stampFile = Join-Path $nd 'normal-code.txt'
  if (-not (Test-Path -LiteralPath $ne) -or -not (Test-Path -LiteralPath $stampFile)) { $problems += "no normal-code exe $ne (run tools\build_normal_code.ps1, or pack with -NoNormalCode)"; continue }
  $st = @(Get-Content -LiteralPath $stampFile)
  $sc = ($st | Where-Object { $_ -like 'commit=*' } | Select-Object -First 1) -replace '^commit=', ''
  $ss = ($st | Where-Object { $_ -like 'sha256=*' } | Select-Object -First 1) -replace '^sha256=', ''
  if (-not $head -or $sc -ne $head.Trim()) { $problems += "$ne was built from commit '$sc', this checkout is '$head' - run tools\build_normal_code.ps1 again" }
  elseif ($ss -ne (Get-FileHash -Algorithm SHA256 -LiteralPath $ne).Hash.ToLower()) { $problems += "$ne is not the exe normal-code.txt describes" }
  elseif ((Get-FileHash -Algorithm SHA256 -LiteralPath $ne).Hash -eq (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $p.from $p.exe)).Hash) { $problems += "$ne is the same file as the faster $($p.exe)" }
  else { $p.normal = $ne }
}
foreach ($p in $ports) {
  foreach ($k in $pinned.Keys) {
    $f = Join-Path $p.from $k
    if (-not (Test-Path -LiteralPath $f)) { if (-not ($p.noShared -and $k -in 'dxcompiler.dll', 'libxess.dll')) { $problems += "missing next to $($p.to): $k" }; continue }
    if (-not (Test-Path -LiteralPath $pinned[$k])) { $problems += "no SDK copy to compare $k with: $($pinned[$k])"; continue }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $f).Hash -ne (Get-FileHash -Algorithm SHA256 -LiteralPath $pinned[$k]).Hash) {
      $problems += "$($p.to)\$k is not the 0.4.9 SDK's copy (file version $((Get-Item -LiteralPath $f).VersionInfo.FileVersion)) - rebuild" }
  }
}
$sdlv = (Get-Item -LiteralPath (Join-Path $launcher 'SDL3.dll') -ErrorAction SilentlyContinue).VersionInfo.ProductVersion
if (($sdlv -replace '[ ,]+', '.') -notlike "$launcherSdl*") { $problems += "launcher SDL3.dll is $sdlv, not $launcherSdl" }
# FFmpeg: the DLL names (library majors) must be the ones licenses\FFmpeg-NOTICE.txt names (LGPL: the source we name)
$ffNotice = Get-Content -Raw (Join-Path $cogbox 'licenses\FFmpeg-NOTICE.txt')
foreach ($f in @(Get-ChildItem $launcher -Filter 'av*-*.dll' -ErrorAction SilentlyContinue)) { if (-not $ffNotice.Contains($f.Name)) { $problems += "launcher $($f.Name) is not the FFmpeg build licenses\FFmpeg-NOTICE.txt describes" } }
# 0.2.2 (Shagg: developer tools never ship): no profiler, debug runtime, symbols or test program in any port folder, and
# no shipped program may load Tracy (TracyClient*.dll opens a network port; antivirus flags it, GitHub #24).
$deny = '^(TracyClient.*\.dll|rexruntimerd\.dll|rexgpu-.*rd\.dll|.*\.pdb|.*\.ilk|.*\.exp|.*\.lib|gn_tests\.exe|.*\.pre.*|.*\.old-.*|.*\.bak)$'
foreach ($p in $ports) {
  if (-not (Test-Path $p.from)) { continue }
  foreach ($f in @(Get-ChildItem -LiteralPath $p.from -File -Filter *.dll -ErrorAction SilentlyContinue)) {
    if ($f.Name -match $deny) { $problems += "developer file would ship: $($p.from)\$($f.Name)" }
  }
  foreach ($f in @(Get-ChildItem -LiteralPath $p.from -File -Filter *.dll -ErrorAction SilentlyContinue) + @(Get-Item -LiteralPath (Join-Path $p.from $p.exe) -ErrorAction SilentlyContinue) + @(if ($p.normal) { Get-Item -LiteralPath $p.normal })) {   # cg050: the -normal exe too
    if ($f -and [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($f.FullName)).Contains('TracyClient')) { $problems += "loads Tracy: $($f.FullName)" }
  }
}
# Job 642: the chapter recipe packs (launcher: <COGBox>\recipes\recipes-<exe>.rcp, job 630) and the keyboard prompt
# overlays (<COGBox>\prompts\keyboard\*.overlay, job 629) - no game data, our own lists and drawings.
$recipeSrc = Join-Path $launcher 'recipes'   # the copies the launcher uses on this PC (same files as work\recipes)
foreach ($g in 'gow2', 'gow3', 'gowj', 'gow1') { if (-not (Test-Path (Join-Path $recipeSrc "recipes-$g.rcp"))) { $problems += "missing recipe pack: $recipeSrc\recipes-$g.rcp" } }
$promptSrc = Join-Path $launcher 'prompts\keyboard'
if (-not @(Get-ChildItem -LiteralPath $promptSrc -Filter *.overlay -File -ErrorAction SilentlyContinue).Count) { $problems += "no keyboard prompt overlays in $promptSrc" }
# 0.3.7 (PR #117): the controller prompt glyphs (Kenney, CC0). Every port looks next to its exe, then one and two folders
# up (prompt_overlay_compositor.cpp ResolveGlyphsDir); all ports are <game>\<port>\, so one copy at the zip's top level,
# assets\glyphs\, serves them all.
$glyphSrc = Join-Path (Split-Path -Parent $cogbox) 'assets\glyphs'
foreach ($f in 'LICENSE.txt', 'xbox\a.png') { if (-not (Test-Path (Join-Path $glyphSrc $f))) { $problems += "missing glyph file: $glyphSrc\$f" } }
# 0.4.8: NVIDIA DLSS frame generation (Streamline 2.14.16, commit 371a409). The port loads sl.interposer.dll from its own
# folder after checking NVIDIA's signature (dlss_framegen.cpp), and the launcher offers the DLSS-G choices only when all
# six files are beside the exe (gears_settings.cpp dlss_fg_present). The production (not development) binaries of the
# Streamline release go beside every port exe built with Streamline (GN_WITH_STREAMLINE: it names sl.interposer.dll).
# They are copied byte for byte - never patched (the user-name pass below skips them) - and each must carry a valid
# Authenticode signature from NVIDIA, or nothing is packed.
$slDir = "$docs\Gears2-Native\third_party\streamline-release\bin\x64"
$slFiles = @('sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll', 'sl.reflex.dll', 'sl.pcl.dll', 'nvngx_dlssg.dll')
$slLicenses = @('nvngx_dlss.license.txt', 'reflex.license.txt')   # NVIDIA RTX SDKs licence (nvngx_*), Reflex SDK licence
$slPattern = '^(sl\..*|nvngx_.*)\.dll$'
foreach ($p in $ports) { $p.dlssg = $false }
foreach ($p in $ports) {
  $e = Join-Path $p.from $p.exe
  if (Test-Path $e) { $p.dlssg = [Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes($e)).Contains('sl.interposer.dll') }
}
if (@($ports | Where-Object { $_.dlssg }).Count) {
  foreach ($f in $slFiles + $slLicenses) { if (-not (Test-Path (Join-Path $slDir $f))) { $problems += "missing Streamline file: $slDir\$f" } }
  foreach ($f in $slFiles) {
    $fp = Join-Path $slDir $f
    if (-not (Test-Path $fp)) { continue }
    $sig = Get-AuthenticodeSignature -LiteralPath $fp
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'O="?NVIDIA Corporation') { $problems += "not validly signed by NVIDIA: $fp ($($sig.Status))" }
  }
}
# cleanup049: every .exe / .dll any earlier release shipped must be in src\stale_known.h, or the launcher being packed
# can't recognise it as COGBox's own when this release is unzipped over that one (stale_files.cpp). The lists: the
# SHA256SUMS.txt inside every archive in releases\<version>\ (finals, re-spins *.zip.old-* / *.zip.NNNN, test and rc
# builds), a SHA256SUMS*.txt kept beside them, and cogbox\test\stale_releases\*.txt. A hash missing from the header:
# the header is regenerated here and the packing FAILS - rebuild CogBox.exe (it compiles the header in) and pack again.
$knownFile = Join-Path $cogbox 'src\stale_known.h'
$known = @{}
foreach ($km in [regex]::Matches((Get-Content -Raw -LiteralPath $knownFile), '"([0-9a-f]{64})"')) { $known[$km.Groups[1].Value] = 1 }
$past = @{}   # STRING keys: hash -> where it was seen first
$pastLists = 0
function Add-PastSums([string]$text, [string]$from) {
  foreach ($ln in ($text -split "`n")) {
    $ln = $ln.Trim()
    if ($ln -match '^([0-9a-fA-F]{64})\s+\*?(.+\.(exe|dll))$') { $hh = $Matches[1].ToLower(); if (-not $past.ContainsKey($hh)) { $past["$hh"] = "${from}: $($Matches[2])" } }
  }
}
foreach ($f in @(Get-ChildItem -LiteralPath (Join-Path $cogbox 'test\stale_releases') -Filter '*.txt' -File -ErrorAction SilentlyContinue)) { Add-PastSums (Get-Content -Raw -LiteralPath $f.FullName) "test\stale_releases\$($f.Name)"; $pastLists++ }
$relRoot = Join-Path $docs 'releases'
$unread = @()
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
foreach ($relDir in @(Get-ChildItem -LiteralPath $relRoot -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne $Version })) {
  foreach ($f in @(Get-ChildItem -LiteralPath $relDir.FullName -Filter 'SHA256SUMS*.txt' -File -Recurse -Depth 2 -ErrorAction SilentlyContinue)) { Add-PastSums (Get-Content -Raw -LiteralPath $f.FullName) $f.FullName; $pastLists++ }
  foreach ($arc in @(Get-ChildItem -LiteralPath $relDir.FullName -File | Where-Object { $_.Name -match '^COGBox-.*\.zip(\.old-\d+|\.\d+)?$' })) {
    try {
      $za0 = [IO.Compression.ZipFile]::OpenRead($arc.FullName)
      $ent = @($za0.Entries | Where-Object { $_.FullName -match '(^|/)SHA256SUMS\.txt$' }) | Select-Object -First 1
      if ($ent) { $rs = New-Object IO.StreamReader($ent.Open()); Add-PastSums $rs.ReadToEnd() $arc.FullName; $rs.Dispose(); $pastLists++ } else { $unread += "$($arc.FullName) (no SHA256SUMS.txt inside)" }
      $za0.Dispose()
    } catch { $unread += "$($arc.FullName) ($($_.Exception.Message))" }
  }
}
$uncovered = @($past.Keys | Where-Object { -not $known.ContainsKey($_) } | Sort-Object)
Write-Host "  old-files check: $pastLists past file lists, $($past.Count) program-file hashes, $($uncovered.Count) not in stale_known.h"
foreach ($u in $unread) { Write-Host "  note: couldn't read a past release's file list: $u" }
if ($uncovered.Count) {
  $all = @(@($known.Keys) + $uncovered | Sort-Object -Unique)
  $hdr = @('// GENERATED: the SHA-256 of every .exe / .dll in the SHA256SUMS.txt of each published COGBox release (and each re-spin,',
           '// test and rc build archived in releases\). stale_files.cpp only moves an unlisted program file whose hash is in here',
           '// (or in a list a later COGBox saved in _old-files\shipped\). Regenerated by tools\package_cogbox.ps1 (cleanup049) or',
           '// tools/stale_known_gen.sh.', '#pragma once', 'static const char *const kKnownOldProgramHashes[] = {') + ($all | ForEach-Object { "    `"$_`"," }) + @('};')
  [IO.File]::WriteAllText($knownFile, ($hdr -join "`n") + "`n")
  foreach ($u in ($uncovered | Select-Object -First 10)) { Write-Host "    not covered: $u  ($($past[$u]))" }
  $problems += "src\stale_known.h was missing $($uncovered.Count) program file(s) of past releases: regenerated - commit it, rebuild CogBox.exe and package again"
}
if ($problems.Count) { $problems | ForEach-Object { Write-Host "PROBLEM: $_" }; Write-Host 'RESULT: FAIL (nothing written)'; exit 1 }

$name = "COGBox-$Version"
if (-not $Out) { $Out = Join-Path $docs "releases\$Version" }
$stage = Join-Path (Split-Path -Parent $cogbox) "out\pack\$name"
$zip = Join-Path $Out "$name.zip"
Write-Host "Packing $name into $Out"
if ($WhatIf) {
  foreach ($p in $ports) { Write-Host "  $($p.to)  <=  $($p.from) ($($p.exe) + $(@(Get-ChildItem -LiteralPath $p.from -Filter *.dll -File).Count) DLLs)" }
  Write-Host 'RESULT: WHATIF (nothing written)'; exit 0
}
# a new build of the same version replaces the old one (Shagg: always put / replace new builds in COGBox-Releases)
if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
foreach ($f in $zip, "$zip.sha256") { if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force } }
New-Item -ItemType Directory -Force $stage | Out-Null
New-Item -ItemType Directory -Force $Out | Out-Null

# ---- Copy -----------------------------------------------------------------------------------------------------------
Copy-Item (Join-Path $launcher 'CogBox.exe'), (Join-Path $launcher 'SDL3.dll') $stage
# The library music's XMA decoder (FFmpeg, LGPL 2.1, separate DLLs; licenses\FFmpeg-*.txt): optional - without them the
# library is silent, so a build without them still packages (with a warning).
$ff = @(Get-ChildItem $launcher -Filter 'av*-*.dll' -ErrorAction SilentlyContinue)
if ($ff.Count -ge 2) { $ff | Copy-Item -Destination $stage } else { Write-Warning 'no avcodec / avutil DLLs in cogbox\build: the library music will be silent' }
# GitHub #151: 0.4.1 - 0.4.3 shipped a CogBox.exe built without FFmpeg (no headers in that clone), so the library was
# silent although the DLLs were there. A launcher built that way is no longer packaged.
$exeText = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes((Join-Path $launcher 'CogBox.exe')))
if ($exeText.Contains('built without FFmpeg') -or -not $exeText.Contains('avcodec_find_decoder')) {
  throw 'CogBox.exe was built without FFmpeg (no library music) - rebuild it with FFMPEG_DIR set (cogbox\build.sh)' }
Copy-Item (Join-Path $cogbox 'README.txt') $stage
# Launcher translations (0.2.0): cogbox\lang\<code>.txt -> lang\ beside CogBox.exe.
$langSrc = Join-Path $cogbox 'lang'
if (Test-Path $langSrc) { $langDst = Join-Path $stage 'lang'; New-Item -ItemType Directory -Force $langDst | Out-Null; Copy-Item (Join-Path $langSrc '*.txt') $langDst } else { throw 'cogbox\lang is missing' }
# Job 642: recipe packs and keyboard prompts beside CogBox.exe.
$rd = Join-Path $stage 'recipes'; New-Item -ItemType Directory -Force $rd | Out-Null
foreach ($g in 'gow2', 'gow3', 'gowj', 'gow1') { Copy-Item (Join-Path $recipeSrc "recipes-$g.rcp") $rd }
$pd = Join-Path $stage 'prompts\keyboard'; New-Item -ItemType Directory -Force $pd | Out-Null
Copy-Item (Join-Path $promptSrc '*.overlay') $pd
$gd = Join-Path $stage 'assets'; New-Item -ItemType Directory -Force $gd | Out-Null
Copy-Item -LiteralPath $glyphSrc -Destination $gd -Recurse   # -> assets\glyphs\ (LICENSE.txt + one folder per style)
Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $cogbox) 'assets\sounds') -Destination $gd -Recurse   # -> assets\sounds\ (achievement popup sound, replaceable)
Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $cogbox) 'assets\fonts') -Destination $gd -Recurse    # -> assets\fonts\ (Inter for the achievement popups + its OFL licence)
Copy-Item (Join-Path (Split-Path -Parent $cogbox) 'LICENSE') (Join-Path $stage 'LICENSE.txt')
# legal050 (0.5.0): the legal notice, privacy and contributing texts from the repository root, beside CogBox.exe (the welcome
# notice's "Read more" opens LEGAL.md there). Missing = stop: the notice points to it.
foreach ($f in 'LEGAL.md', 'PRIVACY.md', 'CONTRIBUTING.md') { $src = Join-Path $docs $f; if (-not (Test-Path $src)) { throw "missing $f in $docs (legal050)" }; Copy-Item $src (Join-Path $stage $f) }
$lic = Join-Path $stage 'licenses'; New-Item -ItemType Directory -Force $lic | Out-Null
Copy-Item (Join-Path $cogbox 'licenses\*') $lic
Copy-Item (Join-Path $glyphSrc 'LICENSE.txt') (Join-Path $lic 'Kenney-Input-Prompts-LICENSE.txt')
foreach ($pr in $projects) {
  foreach ($f in 'LICENSE', 'THIRD_PARTY_NOTICES.md') {
    $src = Join-Path "$docs\$pr" $f
    if (Test-Path $src) { Copy-Item $src (Join-Path $lic "$pr-$f$(if ($f -eq 'LICENSE') { '.txt' })") } else { Write-Host "note: no $pr\$f" }
  }
}
foreach ($sdk in $sdks) {
  $dir = "$docs\Gears2-Native\third_party\$sdk"
  if (-not (Test-Path $dir)) { Write-Host "note: SDK folder not found: $sdk"; continue }
  $found = @(Get-ChildItem -LiteralPath $dir -File -Recurse -Depth 2 -ErrorAction SilentlyContinue |
             Where-Object { $_.Name -match '^(LICENSE|LICENCE|COPYING|NOTICE)' -or $_.Name -match '^license\.(md|txt)$' -or $_.Name -match 'license.*\.(md|txt|rtf|pdf)$' -or $_.Name -match '^third-party-programs\.txt$' } |   # xefg049: Intel's third-party-programs.txt (its LICENSE.txt points to it)
             # 0.2.2: only licences, and only of what ships - not a licence checker's config, the test framework, the
             # build tools or the profiler (none of them is in the zip)
             Where-Object { $_.Extension -ne '.cfg' -and $_.FullName -notmatch '\\thirdparty\\(catch2|tracy)\\' -and $_.FullName -notmatch '\\tools\\binutils\\' })
  if (-not $found.Count) { Write-Host "note: no licence file found in $sdk - add it by hand" }
  # job 530: each SDK in its own folder, keeping the path inside it - many files are just "LICENSE", and a flat folder let
  # them overwrite each other (and hid which bundled library a licence belongs to)
  foreach ($f in $found) {
    $rel = $f.FullName.Substring($dir.Length).TrimStart('\')
    $dst = Join-Path (Join-Path $lic $sdk) $rel
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    Copy-Item $f.FullName $dst -Force
    if ((Get-Content -LiteralPath $f.FullName -TotalCount 5 -ErrorAction SilentlyContinue) -match 'GNU (LESSER |LIBRARY )?GENERAL PUBLIC LICENSE') { Write-Host "  licence note: GPL-family text: $sdk\$rel" }
  }
}
# The upscaler and shader-compiler DLLs that are byte-identical in every build go in once, in shared\ (Shagg 20:27,
# option 1); COGBox puts that folder on each game's PATH and passes --gow2_dxc_dir. Checked in job 487 (all three games
# start with FSR and with XeSS from this layout).
# 0.4.9 (shared049, docs/plan-049-shared-dlls.md): also NVIDIA DLSS (nvngx_dlss.dll: the game's NGX looks beside the exe,
# then in shared\), dxil.dll (DXC loads it beside dxcompiler.dll), the AMD Vulkan DLLs (by name: shared\ is on the PATH,
# else gn_shared_dlls loads the loader from shared\) and NVIDIA Streamline (the game takes the whole set from shared\).
# They were byte-identical in all four 0.4.8 port folders: ~315 MiB less unpacked. Still beside each exe: the exe,
# rexruntime.dll / rexgpu-xenos.dll (load-time imports: Windows looks only beside the exe, System32 and PATH) and
# D3D12\D3D12Core.dll (the exe's D3D12SDKPath is relative to the exe).
$sharedNames = @('libxess.dll', 'amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll', 'amd_fidelityfx_framegeneration_dx12.dll', 'dxcompiler.dll',
                 'dxil.dll', 'nvngx_dlss.dll', 'amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_framegeneration_vk.dll')
$sharedOptional = @('amd_fidelityfx_upscaler_vk.dll')   # fsr049: shared when the builds have it (all the same), else a note
$sh = Join-Path $stage 'shared'; New-Item -ItemType Directory -Force $sh | Out-Null
foreach ($n in $sharedNames + $sharedOptional) {
  $needed = @($ports | Where-Object { -not ($_.noShared -and $n -in 'dxcompiler.dll', 'libxess.dll', 'amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll', 'amd_fidelityfx_framegeneration_dx12.dll') })
  $copies = @($ports | ForEach-Object { Join-Path $_.from $n } | Where-Object { Test-Path $_ })
  $hashes = @($copies | ForEach-Object { (Get-FileHash -Algorithm SHA256 -LiteralPath $_).Hash } | Sort-Object -Unique)
  if ($n -in $sharedOptional -and -not $copies.Count) { Write-Host "  note: no build has $n - not shipped"; continue }
  $missing = @($needed | Where-Object { -not (Test-Path (Join-Path $_.from $n)) })
  if (($missing.Count -and $n -notin $sharedOptional) -or $hashes.Count -ne 1) { Write-Host "PROBLEM: $n is missing from a build or differs between builds ($($copies.Count) copies, $($hashes.Count) versions) - it can't be shared"; Write-Host 'RESULT: FAIL'; exit 1 }
  Copy-Item $copies[0] $sh
}
# 0.5.0 (RC6-3): DXVK is no longer shipped - Gears of War PC refuses to start with its files (the switch is gone)
# NVIDIA Streamline: once, in shared\, from the release folder (signature checked above)
if (@($ports | Where-Object { $_.dlssg }).Count) { foreach ($f in $slFiles) { Copy-Item (Join-Path $slDir $f) $sh -Force } }
foreach ($p in $ports) {
  $dst = Join-Path $stage $p.to; New-Item -ItemType Directory -Force $dst | Out-Null
  Copy-Item (Join-Path $p.from $p.exe) $dst
  if ($p.normal) { Copy-Item -LiteralPath $p.normal (Join-Path $dst ($p.exe -replace '\.exe$', '-normal.exe')) }   # cg050
  Get-ChildItem -LiteralPath $p.from -Filter *.dll -File | Where-Object { ($sharedNames + $sharedOptional + $slFiles) -notcontains $_.Name } | ForEach-Object { Copy-Item $_.FullName $dst }
  # 0.2.0 (#9): the Agility SDK's D3D12Core.dll in D3D12\ beside each game (the exe's D3D12SDKPath points there)
  $ag = Join-Path $p.from 'D3D12\D3D12Core.dll'
  New-Item -ItemType Directory -Force (Join-Path $dst 'D3D12') | Out-Null; Copy-Item $ag (Join-Path $dst 'D3D12')
  # 0.4.8 put the Streamline runtime and the AMD Vulkan DLLs here; 0.4.9 (shared049): in shared\ (above)
  # xefg049: Intel XeSS frame generation (Gears 2 only for now) - libxess_fg.dll + libxell.dll come from the build folder
  # with the DLL loop above (Gears 2's CMake copies them from the XeSS 3 SDK); without them the launcher hides the choice
  if ($p.exe -eq 'gow2.exe') { foreach ($f in 'libxess_fg.dll', 'libxell.dll') {
    if (-not (Test-Path (Join-Path $dst $f))) { Write-Host "  note: $($p.to) has no $f - no Intel XeSS frame generation there" } } }
}
if (@($ports | Where-Object { $_.dlssg }).Count) {
  $sld = Join-Path $lic 'streamline-release'; New-Item -ItemType Directory -Force $sld | Out-Null
  foreach ($f in $slLicenses) { Copy-Item (Join-Path $slDir $f) $sld }
  Write-Host "  NVIDIA Streamline (DLSS-G) in shared\ for: $(($ports | Where-Object { $_.dlssg } | ForEach-Object { $_.to }) -join ', ')"
}

# ---- lic049 (Shagg 2026-11-08): every game folder and shared\ get their own full copy of licenses\ (text only, small), so
# the licence texts travel with the DLLs they cover. COGBox's own MIT licence goes in as licenses\COGBox-LICENSE.txt too.
# The update cleanup never moves these: stale_files.cpp only looks at .exe / .dll, upgrade_cleanup.cpp only at settings.ini
# and shadercache\, and the updater overwrites them like any other file of the release.
Copy-Item (Join-Path $stage 'LICENSE.txt') (Join-Path $lic 'COGBox-LICENSE.txt')
$licHomes = @('shared') + @($ports | ForEach-Object { Split-Path $_.to -Parent } | Sort-Object -Unique)
foreach ($lh in $licHomes) {
  $lhDir = Join-Path $stage $lh; New-Item -ItemType Directory -Force $lhDir | Out-Null
  Copy-Item -LiteralPath $lic -Destination (Join-Path $lhDir 'licenses') -Recurse -Force
}
# The check: each licences folder exists, and every shipped .exe / .dll has its licence texts in the licenses\ of its own
# top folder (game folder, shared\, or the release root). A DLL with no rule here fails the package: add its licence.
$licRules = [ordered]@{
  '^(cogbox|gow1|gow2|gow3|gowj)(-normal)?\.exe$' = @('COGBox-LICENSE.txt')   # cg050: -normal
  '^(rexruntime|rexgpu-xenos)\.dll$'             = @('rexglue-sdk\LICENSE', 'Xenia-LICENSE.txt', 'FFmpeg-NOTICE.txt', 'FFmpeg-LGPL-2.1.txt')
  '^(avcodec|avutil)-\d+\.dll$'                  = @('FFmpeg-NOTICE.txt', 'FFmpeg-LGPL-2.1.txt')
  '^sdl3\.dll$'                                  = @('SDL3-LICENSE.txt')
  '^d3d12core\.dll$'                             = @('agility-sdk-1.620.1\LICENSE.txt', 'Microsoft-DirectX-redist-NOTICE.txt')
  '^(dxcompiler|dxil)\.dll$'                     = @('dxc-1.9.2609\LICENSE-MS.txt', 'Microsoft-DirectX-redist-NOTICE.txt')
  '^nvngx_dlss(g)?\.dll$'                        = @('dlss-sdk\LICENSE.txt', 'NVIDIA-Streamline-DLSS-G-NOTICE.txt')
  '^sl\..+\.dll$'                                = @('streamline-sdk\license.txt', 'streamline-release\reflex.license.txt', 'NVIDIA-Streamline-DLSS-G-NOTICE.txt')
  '^amd_fidelityfx_.+\.dll$'                     = @('AMD-FidelityFX-SDK-LICENSE.md')
  '^(libxess|libxess_fg|libxell)\.dll$'          = @('xess-sdk-3.0.2\*\LICENSE.txt', 'xess-sdk-3.0.2\*\third-party-programs.txt')
}
$licProblems = @()
foreach ($lh in @('') + $licHomes) {
  $lhLic = if ($lh) { Join-Path (Join-Path $stage $lh) 'licenses' } else { $lic }
  if (-not (Test-Path -LiteralPath $lhLic)) { $licProblems += "no licenses\ folder in $(if ($lh) { $lh } else { 'the release root' })" }
}
foreach ($pf in @(Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll' })) {
  $pfRel = $pf.FullName.Substring($stage.TrimEnd('\').Length + 1)
  $pfTop = if ($pfRel.Contains('\')) { $pfRel.Split('\')[0] } else { '' }
  $pfLic = if ($pfTop) { Join-Path (Join-Path $stage $pfTop) 'licenses' } else { $lic }
  $rule = @($licRules.Keys | Where-Object { $pf.Name.ToLower() -match $_ }) | Select-Object -First 1
  if (-not $rule) { $licProblems += "$pfRel has no licence rule in package_cogbox.ps1 - add its licence texts and a rule"; continue }
  foreach ($need in $licRules[$rule]) {
    if (-not @(Get-ChildItem -Path (Join-Path $pfLic $need) -File -ErrorAction SilentlyContinue).Count) { $licProblems += "$pfRel : $need missing from $pfLic" }
  }
}
if ($licProblems.Count) { $licProblems | ForEach-Object { Write-Host "PROBLEM: $_" }; Write-Host 'RESULT: FAIL'; exit 1 }
Write-Host "  check: licenses\ in the root, shared\ and $(($licHomes | Where-Object { $_ -ne 'shared' }) -join ', '); every .exe / .dll has its licence texts beside it"

# ---- Your Windows user name out of the programs (Shagg 20:19 "1. Yes"). The compiler writes source folders such as
# C:/Users/<name>/Documents/... into the programs for error messages; the copies in the zip get the same number of
# letters ("user1" for a five-letter name) instead, so nothing else in the files moves. The PC's own builds are untouched.
$un = $env:USERNAME
if (-not ('CogBoxPack' -as [type])) {
  Add-Type -TypeDefinition @'
public static class CogBoxPack {
  // same-length, case-insensitive (ASCII letters) replacement of every occurrence; returns how many
  public static int Swap(byte[] d, byte[] lo, byte[] up, byte[] with) {
    int n = 0;
    for (int i = 0; i + lo.Length <= d.Length; i++) {
      int j = 0;
      while (j < lo.Length && (d[i + j] == lo[j] || d[i + j] == up[j])) j++;
      if (j == lo.Length) { for (j = 0; j < with.Length; j++) d[i + j] = with[j]; n++; i += lo.Length - 1; }
    }
    return n;
  }
}
'@
}
if ($un -and $un.Length -ge 3) {
  $rep = ('user' + ('1' * 40)).Substring(0, $un.Length)
  # NVIDIA's signed DLLs are never rewritten (a changed byte breaks the signature the port checks)
  foreach ($f in @(Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll' -and $_.Name -notmatch $slPattern })) {
    $b = [IO.File]::ReadAllBytes($f.FullName); $n = 0
    foreach ($enc in @([Text.Encoding]::ASCII, [Text.Encoding]::Unicode)) {
      $n += [CogBoxPack]::Swap($b, $enc.GetBytes($un.ToLower()), $enc.GetBytes($un.ToUpper()), $enc.GetBytes($rep))
    }
    if ($n) { [IO.File]::WriteAllBytes($f.FullName, $b); Write-Host "  user name replaced $n time(s) in $($f.FullName.Substring($stage.Length + 1))" }
  }
  # the check: the name must be gone from every file in the package
  $left = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.Name -notmatch $slPattern } | Where-Object { $t = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($_.FullName)); $u = [Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes($_.FullName)); $t -match [regex]::Escape($un) -or $u -match [regex]::Escape($un) -or $t -match [regex]::Escape($env:COMPUTERNAME) -or $u -match [regex]::Escape($env:COMPUTERNAME) })   # -match ignores case; both names in both encodings
  if ($left.Count) { $left | ForEach-Object { Write-Host "PROBLEM: the user or PC name is still in $($_.FullName)" }; Write-Host 'RESULT: FAIL'; exit 1 }
  Write-Host '  check: no user name or PC name left in any file of the package'
}

# ---- Nothing of the games, and checksums ------------------------------------------------------------------------------
$bad = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.Extension -in '.xex', '.xexp', '.iso', '.xiso' -or $_.FullName -match '\\(game|extracted|user|logs|shadercache)\\' })
if ($bad.Count) { $bad | ForEach-Object { Write-Host "PROBLEM: game or player file in the package: $($_.FullName)" }; Write-Host 'RESULT: FAIL'; exit 1 }
$sums = @()
foreach ($f in @(Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll' })) {
  $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash.ToLower()
  $sums += "$h  $($f.FullName.Substring($stage.Length + 1))"
}
Set-Content (Join-Path $stage 'SHA256SUMS.txt') $sums
# 0.2.0 (job 583): files only, names with / - Compress-Archive also wrote folder entries named with \ ("licenses\fidelityfx-sdk\"),
# which 0.1.0's updater took for files and could not replace ("Couldn't replace licenses\fidelityfx-sdk\"), and \ names
# are not folders to unzip tools on Linux / the Steam Deck
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$zfs = [IO.File]::Open($zip, [IO.FileMode]::CreateNew)
$za = New-Object IO.Compression.ZipArchive($zfs, [IO.Compression.ZipArchiveMode]::Create)
foreach ($f in (Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName)) {
  $rel = $f.FullName.Substring($stage.TrimEnd('\').Length + 1).Replace('\', '/')
  [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f.FullName, $rel, [IO.Compression.CompressionLevel]::Optimal)
}
$za.Dispose(); $zfs.Dispose()
$zh = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLower()
Copy-Item -LiteralPath (Join-Path $stage 'SHA256SUMS.txt') (Join-Path $Out 'SHA256SUMS.txt') -Force   # cleanup049: the next release's old-files check reads it
Set-Content "$zip.sha256" "$zh  $name.zip"
$mb = [Math]::Round((Get-Item $zip).Length / 1MB)
Write-Host "zip: $zip ($mb MB), SHA-256 $zh"
if ($mb -ge 2000) { Remove-Item -LiteralPath $zip, "$zip.sha256" -ErrorAction SilentlyContinue; Write-Host 'PROBLEM: over GitHub''s 2 GB limit for one release file (zip removed)'; Write-Host 'RESULT: FAIL'; exit 1 }
# 0.4.9 review: the .7z (smaller download; COGBox's updater reads it too). Solid blocks bounded at 64 MB (-ms=64m), so the
# launcher never has to unpack more than one 64 MB block in memory per file it extracts (archive7z.cpp refuses blocks over
# 512 MB). The same switches as the ship checklist; 7-Zip from Program Files or third_party\7zip.
$sevenZip = @("$env:ProgramFiles\7-Zip\7z.exe", "${env:ProgramFiles(x86)}\7-Zip\7z.exe", (Join-Path $PSScriptRoot '..\..\third_party\7zip\7z.exe')) |
  Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
$sz = Join-Path $Out "$name.7z"
if (-not $sevenZip) { Write-Host 'PROBLEM: no 7z.exe (Program Files\7-Zip or third_party\7zip) - no .7z made'; Write-Host 'RESULT: FAIL'; exit 1 }
if (Test-Path -LiteralPath $sz) { Write-Host "PROBLEM: ${sz} already exists"; Write-Host 'RESULT: FAIL'; exit 1 }
$szOut = @(& $sevenZip a -t7z -m0=lzma2 -mx=9 -mfb=64 -md=256m -ms=64m -mqs=on -mmt=on -bd -y $sz (Join-Path $stage '*') 2>&1 | ForEach-Object { "$_" })
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $sz)) { $szOut | Select-Object -Last 15 | ForEach-Object { Write-Host "    $_" }; Write-Host "PROBLEM: 7z exit $LASTEXITCODE"; Write-Host 'RESULT: FAIL'; exit 1 }
$szt = @(& $sevenZip t -bd $sz 2>&1 | ForEach-Object { "$_" })
if ($LASTEXITCODE -ne 0) { $szt | Select-Object -Last 10 | ForEach-Object { Write-Host "    $_" }; Write-Host 'PROBLEM: 7z t failed on the new .7z'; Write-Host 'RESULT: FAIL'; exit 1 }
$sh = (Get-FileHash -Algorithm SHA256 -LiteralPath $sz).Hash.ToLower()
Set-Content "$sz.sha256" "$sh  $name.7z"
Write-Host "7z: $sz ($([Math]::Round((Get-Item $sz).Length / 1MB)) MB, solid blocks <= 64 MB), SHA-256 $sh"
Write-Host 'RESULT: PASS'
# END-OF-JOB
