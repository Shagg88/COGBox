# Gears2-Native: list / extract an Xbox 360 STFS package (LIVE / PIRS / CON - title updates, DLC). Our code (MIT),
# written from the public description of the STFS layout. Read-only on the package.
# Usage: tools\stfs_extract.ps1 -Package <file> [-OutDir <folder>]   (no -OutDir: list only)
# Only contiguous files are extracted (title updates store every file contiguously); others are listed as CHAIN.
param([Parameter(Mandatory = $true)][string]$Package, [string]$OutDir = '')
$ErrorActionPreference = 'Stop'
$d = [IO.File]::ReadAllBytes($Package)
function BE32([int]$o) { return ([uint32]$d[$o] -shl 24) -bor ([uint32]$d[$o + 1] -shl 16) -bor ([uint32]$d[$o + 2] -shl 8) -bor [uint32]$d[$o + 3] }
function LE24([int]$o) { return [int]$d[$o] -bor ([int]$d[$o + 1] -shl 8) -bor ([int]$d[$o + 2] -shl 16) }
$magic = [Text.Encoding]::ASCII.GetString($d, 0, 4)
if ($magic -notin @('LIVE', 'PIRS', 'CON ')) { Write-Host "STFS: not a LIVE/PIRS/CON package ($magic)"; exit 1 }
$hdr = BE32 0x340
$vd = 0x379
$ftBlocks = [int]$d[$vd + 3] -bor ([int]$d[$vd + 4] -shl 8)
$ftStart = LE24 ($vd + 5)
$baseOff = ($hdr + 0xFFF) -band (-bnot 0xFFF)
$per = @(170, 28900, 4913000)
function BlockOffset([int]$b, [int]$tables) {
  $blk = $b + [Math]::Floor(($b + $per[0]) / $per[0]) * $tables
  if ($b -ge $per[0]) {
    $blk += [Math]::Floor(($b + $per[1]) / $per[1]) * $tables
    if ($b -ge $per[1]) { $blk += [Math]::Floor(($b + $per[2]) / $per[2]) * $tables }
  }
  return [int64]$baseOff + ([int64]$blk -shl 12)
}
function ReadEntries([int]$tables) {
  $list = New-Object System.Collections.Generic.List[object]
  for ($i = 0; $i -lt $ftBlocks; $i++) {
    $o = BlockOffset ($ftStart + $i) $tables
    for ($k = 0; $k -lt 64; $k++) {
      $e = [int]($o + $k * 64)
      if ($e + 64 -gt $d.Length) { break }
      $nl = $d[$e + 0x28] -band 0x3F
      if ($nl -eq 0) { continue }
      $name = [Text.Encoding]::ASCII.GetString($d, $e, $nl)
      $path = ([int]$d[$e + 0x32] -shl 8) -bor [int]$d[$e + 0x33]
      if ($path -ge 0x8000) { $path -= 0x10000 }  # signed 16-bit: -1 = the root
      $list.Add([pscustomobject]@{ Name = $name; Dir = (($d[$e + 0x28] -band 0x80) -ne 0); Contig = (($d[$e + 0x28] -band 0x40) -ne 0)
                                   Blocks = LE24 ($e + 0x29); Start = LE24 ($e + 0x2F); Path = $path; Size = [int64](BE32 ($e + 0x34)) })
    }
  }
  return ,$list
}
$entries = $null; $tables = 0
foreach ($t in @(1, 2)) {  # read-only packages keep one hash table per level, others two
  $es = ReadEntries $t
  $ok = $es.Count -gt 0 -and -not ($es | Where-Object { $_.Name -notmatch '^[\x20-\x7E]+$' })
  if ($ok) { $entries = $es; $tables = $t; break }
}
if (-not $entries) { Write-Host 'STFS: no readable file table'; exit 1 }
function FullPath([int]$i) {
  $e = $entries[$i]; $p = $e.Name
  while ($e.Path -ge 0 -and $e.Path -lt $entries.Count) { $e = $entries[$e.Path]; $p = $e.Name + '\' + $p }
  return $p
}
Write-Host "STFS: $magic package, $($entries.Count) entries (hash tables per level: $tables)"
$sha = [Security.Cryptography.SHA1]::Create()
for ($i = 0; $i -lt $entries.Count; $i++) {
  $e = $entries[$i]
  if ($e.Dir) { Write-Host "  D $(FullPath $i)"; continue }
  $line = "    $(FullPath $i) $($e.Size) bytes"
  if (-not $e.Contig) { Write-Host "$line CHAIN (not extracted)"; continue }
  if ($OutDir) {
    $buf = New-Object byte[] $e.Size
    $left = $e.Size; $pos = 0
    for ($j = 0; $j -lt $e.Blocks -and $left -gt 0; $j++) {
      $o = BlockOffset ($e.Start + $j) $tables
      $n = [int][Math]::Min(4096, $left)
      [Array]::Copy($d, $o, $buf, $pos, $n)
      $pos += $n; $left -= $n
    }
    $dst = Join-Path $OutDir (FullPath $i)
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    [IO.File]::WriteAllBytes($dst, $buf)
    $line += " SHA-1 " + (($sha.ComputeHash($buf) | ForEach-Object { $_.ToString('x2') }) -join '').Substring(0, 12)
  }
  Write-Host $line
}
