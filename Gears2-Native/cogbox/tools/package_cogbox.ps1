# CogBox: assemble the player zip (CogBox-<version>.zip + .sha256) from the launcher and the three ports' builds.
# Run on the PC: pwsh -File cogbox\tools\package_cogbox.ps1 [-Version 0.1.0] [-Out <folder>] [-WhatIf]
# The zip and its .sha256 land in COGBox-Project\releases\<version>\; the
# unzipped working copy is built in Gears2-Native\out\pack\. A new build of the same version replaces the old files.
# Nothing of the games goes in: the script refuses to pack any .xex / .xexp / .iso / disc folder, and only copies the
# files named in the lists below (never a whole build folder, so test exes, logs, saves and shader caches stay out).
param(
  [string]$Version = '',
  [string]$Out = '',
  [switch]$WhatIf
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
  @{ to = 'Gears2\port-disc'; from = "$docs\Gears2-Native\build\RelWithDebInfo";    exe = 'gow2.exe' },
  @{ to = 'Gears3\port-tu6';  from = "$docs\Gears3-Native\build\RelWithDebInfo-tu6"; exe = 'gow3.exe' },
  @{ to = 'Gears3\port-disc'; from = "$docs\Gears3-Native\build\RelWithDebInfo";    exe = 'gow3.exe' },
  @{ to = 'Judgment\port';    from = "$docs\GearsJ-Native\build\Release";           exe = 'gowj.exe' }
)
# Licences: CogBox's own (fonts, Dear ImGui, SDL3, miniz, stb), each project's LICENSE and THIRD_PARTY_NOTICES.md, and
# the licence files found at the top of each SDK whose code or DLLs ship.
$projects = @('Gears2-Native', 'Gears3-Native', 'GearsJ-Native')
$sdks = @('rexglue-sdk', 'dlss-sdk', 'fidelityfx-sdk', 'xess-sdk', 'dxc-1.9.2607', 'antilag2-sdk', 'nvapi', 'streamline-sdk', 'agility-sdk')

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
  foreach ($f in $needDlls) { if (-not (Test-Path (Join-Path $p.from $f))) { $problems += "missing next to $($p.to): $($p.from)\$f" } }
  if (-not @(Get-ChildItem -LiteralPath $p.from -Filter 'rexruntime*.dll' -File -ErrorAction SilentlyContinue).Count) { $problems += "no rexruntime*.dll in $($p.from)" }
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
Copy-Item (Join-Path $cogbox 'README.txt') $stage
# Launcher translations (0.2.0): cogbox\lang\<code>.txt -> lang\ beside CogBox.exe.
$langSrc = Join-Path $cogbox 'lang'
if (Test-Path $langSrc) { $langDst = Join-Path $stage 'lang'; New-Item -ItemType Directory -Force $langDst | Out-Null; Copy-Item (Join-Path $langSrc '*.txt') $langDst } else { throw 'cogbox\lang is missing' }
Copy-Item (Join-Path (Split-Path -Parent $cogbox) 'LICENSE') (Join-Path $stage 'LICENSE.txt')
$lic = Join-Path $stage 'licenses'; New-Item -ItemType Directory -Force $lic | Out-Null
Copy-Item (Join-Path $cogbox 'licenses\*') $lic
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
             Where-Object { $_.Name -match '^(LICENSE|LICENCE|COPYING|NOTICE)' -or $_.Name -match '^license\.(md|txt)$' -or $_.Name -match 'license.*\.(md|txt|rtf|pdf)$' })
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
# start with FSR and with XeSS from this layout). DLSS, dxil.dll and the runtimes stay beside each program.
$sharedNames = @('libxess.dll', 'amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll', 'amd_fidelityfx_framegeneration_dx12.dll', 'dxcompiler.dll')
$sh = Join-Path $stage 'shared'; New-Item -ItemType Directory -Force $sh | Out-Null
foreach ($n in $sharedNames) {
  $copies = @($ports | ForEach-Object { Join-Path $_.from $n } | Where-Object { Test-Path $_ })
  $hashes = @($copies | ForEach-Object { (Get-FileHash -Algorithm SHA256 -LiteralPath $_).Hash } | Sort-Object -Unique)
  if ($copies.Count -ne $ports.Count -or $hashes.Count -ne 1) { Write-Host "PROBLEM: $n is missing from a build or differs between builds ($($copies.Count) copies, $($hashes.Count) versions) - it can't be shared"; Write-Host 'RESULT: FAIL'; exit 1 }
  Copy-Item $copies[0] $sh
}
foreach ($p in $ports) {
  $dst = Join-Path $stage $p.to; New-Item -ItemType Directory -Force $dst | Out-Null
  Copy-Item (Join-Path $p.from $p.exe) $dst
  Get-ChildItem -LiteralPath $p.from -Filter *.dll -File | Where-Object { $sharedNames -notcontains $_.Name } | ForEach-Object { Copy-Item $_.FullName $dst }
  # 0.2.0 (#9): the Agility SDK's D3D12Core.dll in D3D12\ beside each game (the exe's D3D12SDKPath points there)
  $ag = Join-Path $p.from 'D3D12\D3D12Core.dll'
  New-Item -ItemType Directory -Force (Join-Path $dst 'D3D12') | Out-Null; Copy-Item $ag (Join-Path $dst 'D3D12')
}

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
  foreach ($f in @(Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll' })) {
    $b = [IO.File]::ReadAllBytes($f.FullName); $n = 0
    foreach ($enc in @([Text.Encoding]::ASCII, [Text.Encoding]::Unicode)) {
      $n += [CogBoxPack]::Swap($b, $enc.GetBytes($un.ToLower()), $enc.GetBytes($un.ToUpper()), $enc.GetBytes($rep))
    }
    if ($n) { [IO.File]::WriteAllBytes($f.FullName, $b); Write-Host "  user name replaced $n time(s) in $($f.FullName.Substring($stage.Length + 1))" }
  }
  # the check: the name must be gone from every file in the package
  $left = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $t = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($_.FullName)); $u = [Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes($_.FullName)); $t -match [regex]::Escape($un) -or $u -match [regex]::Escape($un) -or $t -match [regex]::Escape($env:COMPUTERNAME) -or $u -match [regex]::Escape($env:COMPUTERNAME) })   # -match ignores case; both names in both encodings
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
Set-Content "$zip.sha256" "$zh  $name.zip"
$mb = [Math]::Round((Get-Item $zip).Length / 1MB)
Write-Host "zip: $zip ($mb MB), SHA-256 $zh"
if ($mb -ge 2000) { Remove-Item -LiteralPath $zip, "$zip.sha256" -ErrorAction SilentlyContinue; Write-Host 'PROBLEM: over GitHub''s 2 GB limit for one release file (zip removed)'; Write-Host 'RESULT: FAIL'; exit 1 }
Write-Host 'RESULT: PASS'
# END-OF-JOB
