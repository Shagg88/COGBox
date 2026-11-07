# PROVES: which Judgment GearProfileSettings ids hold campaign/Aftermath progress, collectibles and unlockables (name -> id), read from GearGame.xxx
# PREDICTION: GearGame.xxx decompresses (LZX, 128 KB blocks); its name table holds ProfileMappings names such as CompletedChapters*/Unlocked*/Discover*; C:\COGBox\work\gj-profile-ids.txt lists Id=<n> Name=<name> pairs plus DefaultSettings PropertyId/Value1
# GAMES STARTED 0
$ErrorActionPreference = 'Continue'
$work = 'C:\COGBox\work'; New-Item -ItemType Directory -Force $work | Out-Null
$log = Join-Path $work 'gj-profile-ids-job.txt'; "start $(Get-Date -Format s)" | Set-Content $log
function L($m) { Write-Host $m; Add-Content $log $m }
$src = 'C:\COGBox\My Game\Judgment\game\extracted\GearGame\CookedXbox360\GearGame.xxx'
$plain = Join-Path $work 'GearGame-gowj.upk'
if (-not (Test-Path $src)) { L "STOP: $src missing"; L '# END-OF-JOB'; exit 1 }
$docs = [Environment]::GetFolderPath('UserProfile')
$roots = @('C:\COGBox', "$docs\Documents", "$docs\OneDrive\Documents") | Where-Object { Test-Path $_ }

# (a) an already plain copy (package patch cache) with the same size note, or an upk_decompress exe, or build one
if (-not (Test-Path $plain)) {
  $exe = $roots | ForEach-Object { Get-ChildItem $_ -Recurse -Filter 'upk_decompress*.exe' -ErrorAction SilentlyContinue } | Select-Object -First 1
  if (-not $exe) {
    $c = $roots | ForEach-Object { Get-ChildItem $_ -Recurse -Filter 'upk_decompress.c' -ErrorAction SilentlyContinue } | Select-Object -First 1
    $lzxd = $roots | ForEach-Object { Get-ChildItem $_ -Recurse -Filter 'lzxd.c' -ErrorAction SilentlyContinue } | Where-Object { Test-Path (Join-Path $_.DirectoryName 'lzx.h') } | Select-Object -First 1
    $cc = (Get-ChildItem "$docs\Documents\Gears2-Native\third_party" -Recurse -Filter 'clang.exe' -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
    if (-not $cc) { $cc = (Get-Command clang, gcc -ErrorAction SilentlyContinue | Select-Object -First 1).Source }
    L "build: src=$($c.FullName) lzxd=$($lzxd.FullName) cc=$cc"
    if ($c -and $lzxd -and $cc) {
      $bd = Join-Path $work 'upkbuild'; New-Item -ItemType Directory -Force $bd | Out-Null
      Copy-Item (Join-Path $lzxd.DirectoryName "*") $bd -Recurse -Force
      $patch = Join-Path $c.DirectoryName 'lzxd-xmem-nopad.patch'
      if ((Test-Path $patch) -and (Get-Command git -ErrorAction SilentlyContinue)) { Push-Location $bd; git apply --unsafe-paths -p0 --include=lzxd.c $patch 2>&1 | ForEach-Object { L "patch: $_" }; Pop-Location }
      $out = Join-Path $bd 'upk_decompress.exe'
      & $cc -O2 -I $bd -o $out $c.FullName (Join-Path $bd 'lzxd.c') (Join-Path $bd 'system.c') 2>&1 | ForEach-Object { L "cc: $_" }
      if (Test-Path $out) { $exe = Get-Item $out }
    }
  }
  if ($exe) { L "decompress with $($exe.FullName)"; & $exe.FullName $src $plain 2>&1 | ForEach-Object { L "upk: $_" } }
  if (-not (Test-Path $plain)) {
    $cache = $roots | ForEach-Object { Get-ChildItem $_ -Recurse -Filter 'GearGame.xxx' -ErrorAction SilentlyContinue } | Where-Object { $_.FullName -match 'ini-edits' -and $_.FullName -match 'Judgment|gowj' } | Select-Object -First 1
    if ($cache) { L "using plain package-patch copy $($cache.FullName)"; Copy-Item $cache.FullName $plain }
  }
}
if (-not (Test-Path $plain)) { L 'STOP: no decompressed GearGame (no exe, no compiler/libmspack, no plain copy)'; L '# END-OF-JOB'; exit 2 }
$b = [IO.File]::ReadAllBytes($plain)
if ($b[0] -ne 0x9E -or $b[1] -ne 0x2A) { L ("STOP: not a plain package (first bytes {0:X2}{1:X2})" -f $b[0], $b[1]); L '# END-OF-JOB'; exit 3 }

# (c) name table + property-tag scan (C#: fast). Tags are Name(i32 idx,i32 num) Type(idx,num) i32 size i32 arrayIndex value.
Add-Type -TypeDefinition @"
using System; using System.Collections.Generic; using System.Text;
public static class GjScan {
  static byte[] b;
  static int I(int o) { return (b[o] << 24) | (b[o+1] << 16) | (b[o+2] << 8) | b[o+3]; }
  public static List<string> Run(byte[] data) {
    b = data; var r = new List<string>(); var names = new List<string>();
    int o = 8; int hdr = I(o); o += 4; int fl = I(o); o += 4; o += fl > 0 ? fl : -fl * 2;
    o += 4; int nc = I(o); int no = I(o + 4);
    r.Add(String.Format("version 0x{0:X8} header {1} names {2} at {3}", I(4), hdr, nc, no));
    o = no;
    for (int i = 0; i < nc; i++) { int l = I(o); o += 4; string s;
      if (l >= 0) { s = Encoding.ASCII.GetString(b, o, Math.Max(0, l - 1)); o += l; } else { s = Encoding.BigEndianUnicode.GetString(b, o, -l * 2 - 2); o += -l * 2; }
      o += 8; names.Add(s); }
    Func<string,int> N = s => names.IndexOf(s);
    int nId = N("Id"), nName = N("Name"), nInt = N("IntProperty"), nNameP = N("NameProperty"), nPid = N("PropertyId"), nV1 = N("Value1"), nByte = N("ByteProperty"), nMap = N("MappingType");
    r.Add(String.Format("idx Id={0} Name={1} IntProperty={2} NameProperty={3} PropertyId={4} Value1={5}", nId, nName, nInt, nNameP, nPid, nV1));
    string[] kw = { "chapter", "complet", "unlock", "aftermath", "epilog", "declass", "star", "discover", "collect", "campaign", "difficult", "insane", "act", "ribbon", "medal" };
    for (int i = 0; i < names.Count; i++) { string lo = names[i].ToLower(); foreach (var k in kw) if (lo.Contains(k)) { r.Add("name " + i + " " + names[i]); break; } }
    for (int p = 0; p + 64 < b.Length; p += 1) {
      if (I(p) == nId && I(p+4) == 0 && I(p+8) == nInt && I(p+16) == 4 && I(p+20) == 0) {
        int id = I(p+24); int q = p + 28; string nm = "?";
        for (int k = 0; k < 6 && q + 32 < b.Length; k++) { int tn = I(q), sz = I(q+16);
          if (tn == nName && I(q+8) == nNameP) { int ni = I(q+24); nm = ni >= 0 && ni < names.Count ? names[ni] : "#" + ni; break; }
          if (sz < 0 || sz > 4096 || tn < 0 || tn >= names.Count) break;
          q += 24 + sz + (I(q+8) == N("BoolProperty") ? 1 : 0) + (I(q+8) == nByte ? 8 : 0); }
        r.Add(String.Format("mapping @0x{0:X} Id={1} Name={2}", p, id, nm));
      }
      if (nPid >= 0 && I(p) == nPid && I(p+8) == nInt && I(p+16) == 4) {
        int pid = I(p+24); string v = "?";
        for (int q = p + 28; q < p + 240 && q + 28 < b.Length; q++) if (I(q) == nV1 && I(q+8) == nInt && I(q+16) == 4) { v = I(q+24).ToString(); break; }
        r.Add(String.Format("default @0x{0:X} PropertyId={1} Value1={2}", p, pid, v));
      }
    }
    return r;
  }
}
"@
$res = [GjScan]::Run($b)
$res | Set-Content (Join-Path $work 'gj-profile-ids.txt')
L "wrote $($res.Count) lines to $work\gj-profile-ids.txt; mappings $(@($res -match '^mapping').Count), defaults $(@($res -match '^default').Count)"
$res | Where-Object { $_ -match '^mapping' } | Select-Object -First 400 | ForEach-Object { L $_ }
L '# END-OF-JOB'
