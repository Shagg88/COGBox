# Unpacks the Gears of War (2006) disc image into Gears1-Native\game\extracted (copied from GearsJ-Native, Shagg's own).
# Read-only on the image. Writes only inside Gears1-Native\game. Windows PowerShell 5.1 compatible.
# Unpacks into game\unpack-tmp first and renames it to game\extracted only when every file checks out,
# so a stopped or failed run never leaves a half-filled "extracted" folder behind.
param(
  [string]$Iso = "C:\Emulation\Games\Xbox 360\Games\New folder\Gears of War (World) (En,Fr,De,Es,It,Zh,Ko).iso",
  [string]$GameDir = (Join-Path (Split-Path $PSScriptRoot -Parent) "game")
)
$ErrorActionPreference = "Stop"
$Out = Join-Path $GameDir "extracted"
$Tmp = Join-Path $GameDir "unpack-tmp"
$LogPath = Join-Path $GameDir "unpack-log.txt"
New-Item -ItemType Directory -Force -Path $GameDir | Out-Null
"" | Set-Content -Path $LogPath -Encoding ASCII
function Log([string]$m) { Write-Host $m; Add-Content -Path $LogPath -Value $m -Encoding ASCII }
function Fail([string]$m) { Log "FAILED: $m"; Log "# END-OF-UNPACK result=failed"; exit 1 }

Log ("unpack_disc.ps1 started " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))
Log "image: $Iso"
if (-not (Test-Path -LiteralPath $Iso)) { Fail "disc image not found" }
if (Test-Path -LiteralPath $Out) { Fail "$Out already exists; nothing done (it is never overwritten)" }
if (Test-Path -LiteralPath $Tmp) { Fail "$Tmp exists from an earlier run; tell Claude, it is not removed automatically" }

$fs = [IO.File]::Open($Iso, 'Open', 'Read', 'Read')
function ReadAt([int64]$off, [int]$n) {
  $fs.Position = $off
  $b = New-Object byte[] $n
  $r = 0
  while ($r -lt $n) { $k = $fs.Read($b, $r, $n - $r); if ($k -le 0) { break }; $r += $k }
  if ($r -ne $n) { throw "short read at $off ($r of $n bytes)" }
  return ,$b
}

# Find the Xbox file system (XGD1/XGD2/XGD3 partition offsets, or a plain XDVDFS image).
$base = -1
foreach ($b in @([int64]0x2080000, [int64]0xFD90000, [int64]0x18300000, [int64]0)) {
  if ($b + 0x10000 + 32 -gt $fs.Length) { continue }
  $h = ReadAt ($b + 0x10000) 32
  if ([Text.Encoding]::ASCII.GetString($h, 0, 20) -eq "MICROSOFT*XBOX*MEDIA") { $base = $b; break }
}
if ($base -lt 0) { $fs.Close(); Fail "no Xbox file system found in the image" }
Log ("file system at 0x{0:X}" -f $base)

# Walk the whole tree first: list every file, then check free space, then copy.
$files = New-Object System.Collections.ArrayList
$dirs = New-Object System.Collections.ArrayList
$todo = New-Object System.Collections.Stack
$todo.Push(@([uint32][BitConverter]::ToUInt32($h, 0x14), [uint32][BitConverter]::ToUInt32($h, 0x18), ""))
while ($todo.Count -gt 0) {
  $d = $todo.Pop()
  $dsec = [int64]$d[0]; $dsize = [int]$d[1]; $dpath = [string]$d[2]
  if ($dsize -eq 0) { continue }
  $tab = ReadAt ($base + $dsec * 2048) $dsize
  $st = New-Object System.Collections.Stack
  $st.Push(0)
  $seen = @{}
  while ($st.Count -gt 0) {
    $o = [int]$st.Pop()
    if ($seen.ContainsKey($o)) { continue }
    $seen[$o] = $true
    if ($o + 14 -gt $tab.Length) { continue }
    $l = [BitConverter]::ToUInt16($tab, $o)
    $r = [BitConverter]::ToUInt16($tab, $o + 2)
    if ($l -eq 0xFFFF -and $r -eq 0xFFFF) { continue }
    $sec = [BitConverter]::ToUInt32($tab, $o + 4)
    $size = [BitConverter]::ToUInt32($tab, $o + 8)
    $attr = $tab[$o + 12]
    $nl = $tab[$o + 13]
    if ($nl -eq 0 -or $o + 14 + $nl -gt $tab.Length) { throw "bad directory entry in '$dpath' at offset $o" }
    $name = [Text.Encoding]::ASCII.GetString($tab, $o + 14, $nl)
    if ($name -match '[\\/:*?"<>|]' -or $name -eq "." -or $name -eq "..") { throw "unsafe name '$name' in '$dpath'" }
    $rel = if ($dpath -eq "") { $name } else { "$dpath\$name" }
    if (($attr -band 0x10) -ne 0) {
      [void]$dirs.Add($rel)
      $todo.Push(@([uint32]$sec, [uint32]$size, $rel))
    } else {
      [void]$files.Add([pscustomobject]@{ Path = $rel; Sector = [int64]$sec; Size = [int64]$size })
    }
    if ($l -ne 0) { $st.Push([int]$l * 4) }
    if ($r -ne 0) { $st.Push([int]$r * 4) }
  }
}
$total = [int64]0
foreach ($f in $files) { $total += $f.Size }
Log ("found {0} files in {1} folders, {2:N0} bytes" -f $files.Count, $dirs.Count, $total)
if ($files.Count -eq 0) { $fs.Close(); Fail "the image lists no files" }

$drive = [IO.Path]::GetPathRoot((Resolve-Path -LiteralPath $GameDir).Path)
$free = (New-Object IO.DriveInfo $drive).AvailableFreeSpace
Log ("free space on {0}: {1:N0} bytes" -f $drive, $free)
if ($free -lt $total + 2GB) { $fs.Close(); Fail ("not enough free space: need {0:N1} GB plus 2 GB spare" -f ($total / 1GB)) }

# Copy.
New-Item -ItemType Directory -Path $Tmp | Out-Null
foreach ($d in $dirs) { New-Item -ItemType Directory -Force -Path (Join-Path $Tmp $d) | Out-Null }
$buf = New-Object byte[] 4194304
$done = [int64]0
$sw = [Diagnostics.Stopwatch]::StartNew()
$lastShow = -1
foreach ($f in $files) {
  $dst = [IO.File]::Create((Join-Path $Tmp $f.Path))
  try {
    $fs.Position = $base + $f.Sector * 2048
    $left = $f.Size
    while ($left -gt 0) {
      $k = $fs.Read($buf, 0, [int][Math]::Min($left, [int64]$buf.Length))
      if ($k -le 0) { throw "image ended early while copying $($f.Path)" }
      $dst.Write($buf, 0, $k); $left -= $k; $done += $k
    }
  } finally { $dst.Close() }
  $pct = [int](100 * $done / [Math]::Max($total, 1))
  if ($pct -ne $lastShow -and $pct % 5 -eq 0) {
    Write-Host ("  {0,3}%  {1:N1} of {2:N1} GB" -f $pct, ($done / 1GB), ($total / 1GB)); $lastShow = $pct
  }
}
$fs.Close()
Log ("copied {0:N0} bytes in {1:N0} s" -f $done, $sw.Elapsed.TotalSeconds)

# Check every file's size, then rename into place.
$bad = 0
foreach ($f in $files) {
  $len = (Get-Item -LiteralPath (Join-Path $Tmp $f.Path)).Length
  if ($len -ne $f.Size) { Log "SIZE MISMATCH: $($f.Path) $len vs $($f.Size)"; $bad++ }
}
if ($bad -gt 0) { Fail "$bad files have the wrong size; left in $Tmp for inspection" }

$list = Join-Path $GameDir "file-list.txt"
$files | Sort-Object Path | ForEach-Object { "{0,14}  {1}" -f $_.Size, $_.Path } | Set-Content -Path $list -Encoding ASCII
Rename-Item -LiteralPath $Tmp -NewName "extracted"

$xex = Join-Path $Out "default.xex"
if (Test-Path -LiteralPath $xex) {
  Log ("default.xex: {0:N0} bytes, SHA-1 {1}" -f (Get-Item -LiteralPath $xex).Length, (Get-FileHash -LiteralPath $xex -Algorithm SHA1).Hash)
} else { Log "note: no default.xex at the top of the disc" }

# Mark the copy read-only: nothing should ever write into it.
Get-ChildItem -LiteralPath $Out -Recurse -File | ForEach-Object { $_.IsReadOnly = $true }
Log "file list: $list"
Log "# END-OF-UNPACK result=ok"
exit 0
