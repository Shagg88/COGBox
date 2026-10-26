#!/bin/bash
# Tests for pclock.ps1 with PowerShell 7 on Linux (cloud workspace): bash test_pclock.sh <pwsh> <empty scratch dir>
# 1) single-process cases, 2) race: 8 processes x 3 holds - no two holds may overlap, no takeovers,
# 3) two owners each queueing 4 short jobs back to back - turns must alternate and there must be no 2-minute stalls.
set -u
PS=$1; T=$2; HERE=$(cd "$(dirname "$0")" && pwd)
rm -rf "$T"; mkdir -p "$T"; cp "$HERE/pclock.ps1" "$T/"; cd "$T"
echo "== single-process cases"
$PS -NoProfile -c '
. ./pclock.ps1
$ok = $true; function Check($name, $cond) { if ($cond) { "  ok   $name" } else { "  FAIL $name"; $script:ok = $false } }
[void](Take-PcLock -Owner a -Job t1 -ExpectedMinutes 5)
Check "held after take" ((Get-PcLock) -like "held: a | t1 |*")
Check "update own" (Update-PcLock -Owner a -ExpectedMinutes 60)
Check "update other refused" (-not (Update-PcLock -Owner b -ExpectedMinutes 60))
Release-PcLock -Owner b | Out-Null
Check "release by other leaves it" ((Get-PcLock) -like "held: a |*")
Release-PcLock -Owner a | Out-Null
Check "free after release" ((Get-PcLock) -eq "free")
$me = Get-PcProcessStart $PID
Set-Content pc.lock "b | x | 999999 | 2026-01-01T00:00:00.0000000Z | 2026-01-01T00:00:00 | 2026-01-01T01:00:00"
Check "dead pid is stale" ((Get-PcLock) -like "STALE (process 999999*")
Set-Content pc.lock ("b | x | 1 | 2000-01-01T00:00:00.0000000Z | {0} | {1}" -f (Get-Date).ToString("s"), (Get-Date).AddMinutes(10).ToString("s"))
Check "reused pid is stale" ((Get-PcLock) -like "STALE (process ID 1 now belongs*")
Set-Content pc.lock ("b | x | {0} | {1} | {2} | {3}" -f $PID, $me, (Get-Date).AddMinutes(-90).ToString("s"), (Get-Date).AddMinutes(-31).ToString("s"))
Check "31 min past end is stale" ((Get-PcLock) -like "STALE (more than 30 min*")
Set-Content pc.lock ("b | x | {0} | {1} | {2} | {3}" -f $PID, $me, (Get-Date).AddMinutes(-90).ToString("s"), (Get-Date).AddMinutes(-29).ToString("s"))
Check "29 min past end is not stale" ((Get-PcLock) -like "held: b |*")
Check "timeout while held" (-not (Take-PcLock -Owner a -Job t5 -ExpectedMinutes 5 -PollSeconds 1 -TimeoutMinutes 0.05))
Check "waiting file removed on timeout" (-not (Test-Path waiting-a.txt))
Set-Content pc.lock "hello"
Check "garbage taken over" (Take-PcLock -Owner a -Job t6 -ExpectedMinutes 5)
Release-PcLock -Owner a | Out-Null
try { [void](Take-PcLock -Owner a -Job t7 -ExpectedMinutes 5); try { throw "boom" } finally { Release-PcLock -Owner a "error" | Out-Null } } catch {}
Check "finally released after error" ((Get-PcLock) -eq "free")
Check "takeovers logged" (@(Select-String -Path status.txt -Pattern TAKEOVER).Count -eq 1)
if (-not $ok) { exit 1 }
' || exit 1

echo "== race: 8 processes x 3 holds"
: > pc.lock; rm -f status.txt holds.txt waiting-* last-*
for i in 1 2 3 4 5 6 7 8; do $PS -NoProfile -c "
. ./pclock.ps1
for (\$r = 0; \$r -lt 3; \$r++) {
  [void](Take-PcLock -Owner p$i -Job r\$r -ExpectedMinutes 1 -PollSeconds 1)
  try { \$a = [DateTime]::UtcNow.Ticks; Start-Sleep -Milliseconds 300; \$b = [DateTime]::UtcNow.Ticks; Add-Content holds.txt \"\$a \$b p$i\" } finally { Release-PcLock -Owner p$i | Out-Null }
}" >/dev/null 2>&1 & done
wait
python3 -I - <<'EOF' || exit 1
h = sorted((int(a), int(b), p) for a, b, p in (l.split() for l in open('holds.txt')))
ov = sum(1 for x, y in zip(h, h[1:]) if y[0] < x[1])
tk = sum(1 for l in open('status.txt') if 'TAKEOVER' in l)
print(f"  holds {len(h)} (want 24), overlaps {ov} (want 0), takeovers {tk} (want 0)")
raise SystemExit(0 if len(h) == 24 and ov == 0 and tk == 0 else 1)
EOF
echo "== turns: two owners, 4 jobs each, back to back"
: > pc.lock; rm -f status.txt holds.txt waiting-* last-*
for o in g2 g3; do $PS -NoProfile -c "
. ./pclock.ps1
for (\$r = 0; \$r -lt 4; \$r++) {
  [void](Take-PcLock -Owner $o -Job j\$r -ExpectedMinutes 1 -PollSeconds 2)
  try { Add-Content holds.txt \"\$([DateTime]::UtcNow.Ticks) $o\"; Start-Sleep 3 } finally { Release-PcLock -Owner $o | Out-Null }
}" >/dev/null 2>&1 & done
start=$(date +%s); wait; secs=$(( $(date +%s) - start ))
python3 -I - "$secs" <<'EOF' || exit 1
import sys
seq = [l.split()[1] for l in sorted(open('holds.txt'))]
alt = sum(1 for a, b in zip(seq, seq[1:]) if a != b)
secs = int(sys.argv[1])
print(f"  order {' '.join(seq)}; switches {alt} (want 7); took {secs} s (want under 60)")
raise SystemExit(0 if alt == 7 and secs < 60 else 1)
EOF
echo "ALL PASS"
