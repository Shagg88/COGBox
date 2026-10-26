# Gears2-Native: check the disc image and one file in it, read-only (the ISO is never written).
#   tools\iso_check.ps1 -Iso <path> [-File GearGame\CookedXenon\SP_Maria_1A.xxx] [-Extracted <copy>] [-Out <folder>] [-NoHash]
# Prints MD5 / SHA-1 / CRC32 of the whole image (to compare with Redump), finds the file in the image's XDVDFS game
# partition, writes the file from the image to -Out, and lists runs of all-zero 2 KB sectors in both copies.
param([Parameter(Mandatory)][string]$Iso, [string]$File = 'GearGame\CookedXenon\SP_Maria_1A.xxx', [string]$Extracted = '',
      [string]$Out = '', [switch]$NoHash)
$ErrorActionPreference = 'Stop'
if (-not ('Gn.Xiso' -as [type])) {
Add-Type -TypeDefinition @'
using System; using System.IO; using System.Collections.Generic; using System.Security.Cryptography;
namespace Gn {
public static class Xiso {
  static uint[] crcT;
  public static string[] HashAll(string path) {
    crcT = new uint[256];
    for (uint i = 0; i < 256; i++) { uint c = i; for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crcT[i] = c; }
    uint crc = 0xFFFFFFFFu;
    using (var md5 = MD5.Create()) using (var sha = SHA1.Create())
    using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20)) {
      var buf = new byte[8 << 20]; int n;
      while ((n = fs.Read(buf, 0, buf.Length)) > 0) {
        md5.TransformBlock(buf, 0, n, null, 0); sha.TransformBlock(buf, 0, n, null, 0);
        for (int i = 0; i < n; i++) crc = crcT[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
      }
      md5.TransformFinalBlock(buf, 0, 0); sha.TransformFinalBlock(buf, 0, 0);
      return new[] { BitConverter.ToString(md5.Hash).Replace("-", ""), BitConverter.ToString(sha.Hash).Replace("-", ""), (crc ^ 0xFFFFFFFFu).ToString("X8") };
    }
  }
  // Game partition offsets: plain xiso, XGD2, XGD3, XGD1.
  static readonly long[] Parts = { 0, 0xFD90000, 0x2080000, 0x18300000 };
  public static long Partition(FileStream fs) {
    var m = new byte[20];
    foreach (var p in Parts) {
      if (p + 0x10000 + 28 > fs.Length) continue;
      fs.Position = p + 0x10000; fs.Read(m, 0, 20);
      if (System.Text.Encoding.ASCII.GetString(m) == "MICROSOFT*XBOX*MEDIA") return p;
    }
    return -1;
  }
  static uint U32(byte[] b, int o) { return (uint)(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24); }
  static int U16(byte[] b, int o) { return b[o] | b[o + 1] << 8; }
  // Returns { partition, byteOffset, size } of the file, or null.
  public static long[] Find(string iso, string relPath) {
    using (var fs = new FileStream(iso, FileMode.Open, FileAccess.Read, FileShare.Read)) {
      long part = Partition(fs); if (part < 0) return null;
      var vd = new byte[28]; fs.Position = part + 0x10000; fs.Read(vd, 0, 28);
      uint sec = U32(vd, 20), size = U32(vd, 24);
      foreach (var name in relPath.Split(new[] { '\\', '/' }, StringSplitOptions.RemoveEmptyEntries)) {
        var tab = new byte[size]; fs.Position = part + (long)sec * 2048; fs.Read(tab, 0, (int)size);
        long[] hit = null; var todo = new Stack<int>(); todo.Push(0); var seen = new HashSet<int>();
        while (todo.Count > 0 && hit == null) {
          int o = todo.Pop(); if (o + 14 > tab.Length || !seen.Add(o)) continue;
          int l = U16(tab, o), r = U16(tab, o + 2); if (l == 0xFFFF) continue;
          int nl = tab[o + 13]; string n = System.Text.Encoding.ASCII.GetString(tab, o + 14, Math.Min(nl, tab.Length - o - 14));
          if (string.Equals(n, name, StringComparison.OrdinalIgnoreCase)) hit = new long[] { U32(tab, o + 4), U32(tab, o + 8) };
          if (l != 0) todo.Push(l * 4); if (r != 0) todo.Push(r * 4);
        }
        if (hit == null) return null;
        sec = (uint)hit[0]; size = (uint)hit[1];
      }
      return new long[] { part, part + (long)sec * 2048, size };
    }
  }
  public static void Copy(string iso, long off, long size, string dest) {
    using (var fs = new FileStream(iso, FileMode.Open, FileAccess.Read, FileShare.Read)) using (var o = File.Create(dest)) {
      fs.Position = off; var buf = new byte[1 << 20]; long left = size;
      while (left > 0) { int n = fs.Read(buf, 0, (int)Math.Min(buf.Length, left)); if (n <= 0) break; o.Write(buf, 0, n); left -= n; }
    }
  }
  // Runs of >= minSectors all-zero 2 KB sectors: "start-end (count)" in hex byte offsets.
  public static string[] ZeroRuns(string path, int minSectors) {
    var res = new List<string>(); var b = new byte[2048]; long s = -1, i = 0;
    using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20)) {
      int n;
      while ((n = fs.Read(b, 0, 2048)) > 0) {
        bool z = n == 2048; for (int k = 0; z && k < n; k++) z = b[k] == 0;
        if (z) { if (s < 0) s = i; } else { if (s >= 0 && i - s >= minSectors) res.Add(string.Format("0x{0:X}-0x{1:X} ({2} sectors)", s * 2048, i * 2048, i - s)); s = -1; }
        i++;
      }
      if (s >= 0 && i - s >= minSectors) res.Add(string.Format("0x{0:X}-end ({1} sectors)", s * 2048, i - s));
    }
    return res.ToArray();
  }
}}
'@
}
$isoItem = Get-Item -LiteralPath $Iso
Write-Host ("  iso: {0} ({1:N0} bytes)" -f $isoItem.FullName, $isoItem.Length)
if (-not $NoHash) {
  $t = Get-Date; $h = [Gn.Xiso]::HashAll($isoItem.FullName)
  Write-Host ("  iso hashes ({0} s): MD5 {1}  SHA-1 {2}  CRC32 {3}" -f [int]((Get-Date) - $t).TotalSeconds, $h[0], $h[1], $h[2])
}
$f = [Gn.Xiso]::Find($isoItem.FullName, $File)
if (-not $f) { Write-Host "  $File not found in the image (no XDVDFS partition, or wrong path)"; return }
Write-Host ("  {0}: game partition at 0x{1:X}, file at image offset 0x{2:X}, {3:N0} bytes" -f $File, $f[0], $f[1], $f[2])
if ($Out) {
  New-Item -ItemType Directory -Force $Out | Out-Null
  $dest = Join-Path $Out (Split-Path $File -Leaf)
  [Gn.Xiso]::Copy($isoItem.FullName, $f[1], $f[2], $dest)
  Write-Host "  from the image: sha256 $((Get-FileHash $dest -Algorithm SHA256).Hash)"
  $z = [Gn.Xiso]::ZeroRuns($dest, 16); Write-Host "  from the image: zero runs of 16+ sectors: $(if ($z.Count) { $z -join '; ' } else { 'none' })"
}
if ($Extracted -and (Test-Path -LiteralPath $Extracted)) {
  $e = Get-Item -LiteralPath $Extracted
  Write-Host ("  extracted copy: {0:N0} bytes, sha256 {1}" -f $e.Length, (Get-FileHash $e.FullName -Algorithm SHA256).Hash)
  $z = [Gn.Xiso]::ZeroRuns($e.FullName, 16); Write-Host "  extracted copy: zero runs of 16+ sectors: $(if ($z.Count) { $z -join '; ' } else { 'none' })"
}
