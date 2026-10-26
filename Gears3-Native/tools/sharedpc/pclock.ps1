# SharedPC\pclock.ps1 - one PC, two projects (Gears 2, Gears 3). Dot-source it from a job:
#
#   . '%USERPROFILE%\Documents\SharedPC\pclock.ps1'
#   if (-not (Take-PcLock -Owner 'gears3' -Job '008-boot' -ExpectedMinutes 15 -RaisePriority)) { exit 5 }
#   try {
#     ... game run / benchmark / full build ...
#     Update-PcLock -Owner 'gears3' -ExpectedMinutes 10     # running long: push the expected end out
#   } finally {
#     Release-PcLock -Owner 'gears3'
#   }
#
# Rules (agreed by both chats, 2026-10-22):
#   * Take the lock for anything that needs the machine to itself: game runs, benchmarks, full builds, SDK rebuilds.
#     Not needed for copying files, listing folders, reading logs, or dropping a job into a queue.
#   * Jobs aim for 20 minutes or less. High priority only while holding the lock.
#   * Each side runs only its own queue and never touches the other's folders.
#
# How the lock works:
#   pc.lock always exists. EMPTY = free. One line = held: owner | job | pid | pidStart | started | expected end
#   (pidStart = the owning process's start time, UTC, round-trip format, so a reused process ID is not mistaken for
#   the owner - compared to within 2 s, since processes read it a few ms apart; started / expected end are local time, yyyy-MM-ddTHH:mm:ss).
#   Every read and write opens the file EXCLUSIVELY (FileShare None), checks, writes and closes in that one handle,
#   so two sides can never both win, nobody reads a half-written line, and Release/Update only ever change a lock
#   whose owner and process ID match their own. Nothing is deleted, so there is no gap between "check" and "delete".
#   A held lock is stale when no process has that ID with that start time, or it is more than 30 minutes past its
#   expected end; a stale lock is taken over in the same exclusive handle and a TAKEOVER line goes to status.txt.
#   Turn-taking: a side waiting for the lock keeps waiting-<owner>.txt fresh (rewritten every poll). A side that
#   released the lock less than 2 minutes ago, while the other side is waiting and has not had the lock since that
#   release (last-take-<owner>.txt), does not take it again until those 2 minutes are up.
#   status.txt: one line per event: time | project | job | event | detail.

$script:PcDir = $PSScriptRoot
if (-not $script:PcDir) { $script:PcDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
$script:PcLockPath = Join-Path $script:PcDir 'pc.lock'
$script:PcStatusPath = Join-Path $script:PcDir 'status.txt'
$script:PcStaleGraceMinutes = 30
$script:PcTurnMinutes = 2
$script:PcTimeFmt = 'yyyy-MM-ddTHH:mm:ss'
$script:PcPrevPriority = $null
$script:PcJob = ''

# Start time of a process (UTC, round-trip format); $null when no process has that ID; 'unknown' when the process
# exists but its start time cannot be read (treated as alive, never as stale).
function Get-PcProcessStart([int]$ProcessId) {
  $p = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
  if (-not $p) { return $null }
  try { return $p.StartTime.ToUniversalTime().ToString('o') } catch { return 'unknown' }
}

# Same process? Start times read by different processes can differ by a few milliseconds (seen in testing), so they
# match when within 2 seconds of each other; a process ID is never reused that quickly.
function Test-PcSameStart([string]$a, [string]$b) {
  if ($a -eq 'unknown' -or $b -eq 'unknown') { return $true }
  try {
    $da = [datetime]::Parse($a, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind)
    $db = [datetime]::Parse($b, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind)
    return ([Math]::Abs(($da - $db).TotalSeconds) -lt 2)
  } catch { return ($a -eq $b) }
}

function Test-PcIoError($err) {
  $e = $err.Exception
  while ($e) { if ($e -is [IO.IOException]) { return $true }; $e = $e.InnerException }
  return $false
}

# Opens pc.lock exclusively, retrying while the other side has it open (milliseconds at a time). Returns a FileStream.
function Open-PcLockExclusive {
  for ($i = 0; $i -lt 200; $i++) {
    try {
      return [IO.File]::Open($script:PcLockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    } catch {
      if (-not (Test-PcIoError $_)) { throw }
      Start-Sleep -Milliseconds (50 + (Get-Random -Maximum 100))
    }
  }
  throw "pclock: could not open $($script:PcLockPath) exclusively for 20 s"
}

function Read-PcLockStream($fs) {
  $fs.Position = 0
  $buf = New-Object byte[] ([int]$fs.Length)
  $n = 0; while ($n -lt $buf.Length) { $r = $fs.Read($buf, $n, $buf.Length - $n); if ($r -le 0) { break }; $n += $r }
  return ([Text.Encoding]::UTF8.GetString($buf, 0, $n)).Trim([char]0xFEFF, ' ', "`r", "`n", "`t")
}

function Write-PcLockStream($fs, [string]$text) {
  $bytes = [Text.Encoding]::UTF8.GetBytes($text)
  $fs.Position = 0; $fs.SetLength(0)
  if ($bytes.Length) { $fs.Write($bytes, 0, $bytes.Length) }
  $fs.Flush($true)
}

# Parses a lock line. Returns $null for an empty lock, or an object; .Bad is set for a line that cannot be parsed.
function ConvertFrom-PcLockLine([string]$line) {
  if (-not $line) { return $null }
  $f = $line -split '\s*\|\s*'
  $o = [pscustomobject]@{ Line = $line; Owner = ''; Job = ''; ProcessId = 0; ProcessStart = ''; Started = $null; ExpectedEnd = $null; Bad = $false }
  if ($f.Count -ne 6) { $o.Bad = $true; return $o }
  $o.Owner = $f[0]; $o.Job = $f[1]; $o.ProcessStart = $f[3]
  $pidValue = 0
  if (-not [int]::TryParse($f[2], [ref]$pidValue)) { $o.Bad = $true; return $o }
  $o.ProcessId = $pidValue
  try {
    $o.Started = [datetime]::ParseExact($f[4], $script:PcTimeFmt, [Globalization.CultureInfo]::InvariantCulture)
    $o.ExpectedEnd = [datetime]::ParseExact($f[5], $script:PcTimeFmt, [Globalization.CultureInfo]::InvariantCulture)
  } catch { $o.Bad = $true }
  return $o
}

# Returns '' when the holder is alive and within time, otherwise the reason the lock is stale.
function Get-PcStaleReason($lock) {
  if ($lock.Bad) { return 'lock line cannot be parsed' }
  $start = Get-PcProcessStart $lock.ProcessId
  if (-not $start) { return "process $($lock.ProcessId) no longer exists" }
  if (-not (Test-PcSameStart $start $lock.ProcessStart)) { return "process ID $($lock.ProcessId) now belongs to a different process (started $start)" }
  if ((Get-Date) -gt $lock.ExpectedEnd.AddMinutes($script:PcStaleGraceMinutes)) { return "more than $($script:PcStaleGraceMinutes) min past its expected end $($lock.ExpectedEnd.ToString($script:PcTimeFmt))" }
  return ''
}

function Write-PcStatus([string]$Owner, [string]$Job, [string]$Event, [string]$Detail = '') {
  $line = "{0} | {1} | {2} | {3} | {4}`r`n" -f (Get-Date).ToString($script:PcTimeFmt), $Owner, $Job, $Event, $Detail
  $bytes = [Text.Encoding]::UTF8.GetBytes($line)
  for ($i = 0; $i -lt 100; $i++) {
    try {
      $fs = [IO.File]::Open($script:PcStatusPath, [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]::Read)
      try { $fs.Write($bytes, 0, $bytes.Length) } finally { $fs.Close() }
      return
    } catch { if (-not (Test-PcIoError $_)) { throw }; Start-Sleep -Milliseconds (50 + (Get-Random -Maximum 100)) }
  }
  Write-Host "pclock: could not write status.txt: $line"
}

# Shows who holds the lock (read only; for logs and for the other chat).
function Get-PcLock {
  $fs = Open-PcLockExclusive
  try { $lock = ConvertFrom-PcLockLine (Read-PcLockStream $fs) } finally { $fs.Close() }
  if (-not $lock) { return 'free' }
  $why = Get-PcStaleReason $lock
  return $(if ($why) { "STALE ($why): $($lock.Line)" } else { "held: $($lock.Line)" })
}

# True when another side is waiting (fresh waiting file) AND has not had the lock since our last release - i.e. it is
# still owed its turn. (Without the second part, two sides that both just finished would each give way to the other.)
function Test-PcOtherOwedTurn([string]$Owner, [datetime]$OurLastRelease) {
  $fresh = (Get-Date).AddMinutes(-3)
  foreach ($w in @(Get-ChildItem $script:PcDir -Filter 'waiting-*.txt' -File -ErrorAction SilentlyContinue)) {
    if ($w.Name -eq "waiting-$Owner.txt" -or $w.LastWriteTime -le $fresh) { continue }
    $other = $w.BaseName.Substring(8)
    $take = Join-Path $script:PcDir "last-take-$other.txt"
    if (-not (Test-Path $take) -or (Get-Item $take).LastWriteTime -lt $OurLastRelease) { return $true }
  }
  return $false
}

# Waits until the lock is free (or stale), takes it, and returns $true. Returns $false after -TimeoutMinutes
# (0 = wait for ever). -RaisePriority sets this process to High while it holds the lock (Release puts it back).
function Take-PcLock([string]$Owner, [string]$Job, [double]$ExpectedMinutes, [int]$PollSeconds = 30, [double]$TimeoutMinutes = 0, [switch]$RaisePriority) {
  if (-not $Owner -or -not $Job -or $ExpectedMinutes -le 0) { throw 'Take-PcLock: -Owner, -Job and -ExpectedMinutes are required' }
  if ($Owner -match '\|' -or $Job -match '\|') { throw 'Take-PcLock: owner and job must not contain |' }
  $myStart = Get-PcProcessStart $PID
  $waitFile = Join-Path $script:PcDir "waiting-$Owner.txt"
  $lastReleaseFile = Join-Path $script:PcDir "last-release-$Owner.txt"
  $t0 = Get-Date; $lastShown = ''; $announced = $false
  while ($true) {
    $deferTurn = $false
    if (Test-Path $lastReleaseFile) {
      $rel = (Get-Item $lastReleaseFile).LastWriteTime
      if (((Get-Date) - $rel).TotalMinutes -lt $script:PcTurnMinutes -and (Test-PcOtherOwedTurn $Owner $rel)) { $deferTurn = $true }
    }
    if (-not $deferTurn) {
      $fs = Open-PcLockExclusive
      try {
        $lock = ConvertFrom-PcLockLine (Read-PcLockStream $fs)
        $why = ''
        if ($lock) { $why = Get-PcStaleReason $lock }
        if (-not $lock -or $why) {
          $now = Get-Date
          $line = '{0} | {1} | {2} | {3} | {4} | {5}' -f $Owner, $Job, $PID, $myStart, $now.ToString($script:PcTimeFmt), $now.AddMinutes($ExpectedMinutes).ToString($script:PcTimeFmt)
          Write-PcLockStream $fs $line
          $fs.Close(); $fs = $null
          if ($why) { Write-PcStatus $Owner $Job 'TAKEOVER' "stale lock ($why): $($lock.Line)"; Write-Host "pclock: took over a stale lock ($why): $($lock.Line)" }
          Remove-Item $waitFile -ErrorAction SilentlyContinue
          Set-Content -Path (Join-Path $script:PcDir "last-take-$Owner.txt") -Value $now.ToString($script:PcTimeFmt) -Encoding UTF8
          $script:PcJob = $Job
          if ($RaisePriority) { try { $p = Get-Process -Id $PID; $script:PcPrevPriority = $p.PriorityClass; $p.PriorityClass = 'High' } catch {} }
          Write-PcStatus $Owner $Job 'started' "lock taken, expected end $($now.AddMinutes($ExpectedMinutes).ToString($script:PcTimeFmt))"
          Write-Host "pclock: lock taken by $Owner/$Job after $([int]((Get-Date) - $t0).TotalSeconds) s"
          return $true
        }
        if ($lock.Line -ne $lastShown) { Write-Host "pclock: $(Get-Date -Format 'HH:mm:ss') waiting - held by $($lock.Owner)/$($lock.Job), expected end $($lock.ExpectedEnd.ToString($script:PcTimeFmt))"; $lastShown = $lock.Line }
      } finally { if ($fs) { $fs.Close() } }
    } elseif (-not $announced) { Write-Host "pclock: $(Get-Date -Format 'HH:mm:ss') the other side is waiting - giving it its turn"; $announced = $true }
    Set-Content -Path $waitFile -Value "$Owner | $Job | waiting since $($t0.ToString($script:PcTimeFmt))" -Encoding UTF8
    if ($TimeoutMinutes -gt 0 -and ((Get-Date) - $t0).TotalMinutes -ge $TimeoutMinutes) {
      Remove-Item $waitFile -ErrorAction SilentlyContinue
      Write-Host "pclock: gave up after $TimeoutMinutes min"; return $false
    }
    Start-Sleep $PollSeconds
  }
}

# Pushes the expected end of OUR lock out to now + -ExpectedMinutes. Returns $false (and changes nothing) when the
# lock is not ours any more.
function Update-PcLock([string]$Owner, [double]$ExpectedMinutes) {
  $fs = Open-PcLockExclusive
  try {
    $lock = ConvertFrom-PcLockLine (Read-PcLockStream $fs)
    if (-not $lock -or $lock.Bad -or $lock.Owner -ne $Owner -or $lock.ProcessId -ne $PID -or -not (Test-PcSameStart $lock.ProcessStart (Get-PcProcessStart $PID))) {
      Write-Host "pclock: Update-PcLock - the lock is not ours (now: $(if ($lock) { $lock.Line } else { 'free' })); unchanged"; return $false
    }
    $f = $lock.Line -split '\s*\|\s*'
    $f[5] = (Get-Date).AddMinutes($ExpectedMinutes).ToString($script:PcTimeFmt)
    Write-PcLockStream $fs ($f -join ' | ')
    return $true
  } finally { $fs.Close() }
}

# Frees OUR lock (empties pc.lock). Leaves someone else's lock alone. Safe to call twice; call it in a finally block.
function Release-PcLock([string]$Owner, [string]$Result = 'done') {
  $mine = $false
  $fs = Open-PcLockExclusive
  try {
    $lock = ConvertFrom-PcLockLine (Read-PcLockStream $fs)
    if ($lock -and -not $lock.Bad -and $lock.Owner -eq $Owner -and $lock.ProcessId -eq $PID -and (Test-PcSameStart $lock.ProcessStart (Get-PcProcessStart $PID))) {
      Write-PcLockStream $fs ''; $mine = $true
    }
  } finally { $fs.Close() }
  if ($script:PcPrevPriority) { try { (Get-Process -Id $PID).PriorityClass = $script:PcPrevPriority } catch {}; $script:PcPrevPriority = $null }
  if ($mine) {
    Set-Content -Path (Join-Path $script:PcDir "last-release-$Owner.txt") -Value (Get-Date).ToString($script:PcTimeFmt) -Encoding UTF8
    Write-PcStatus $Owner $script:PcJob 'finished' "lock released, $Result"
    Write-Host "pclock: lock released by $Owner"
  } else {
    Write-Host "pclock: Release-PcLock - the lock was not ours (now: $(if ($lock) { $lock.Line } else { 'free' })); left alone"
  }
  Remove-Item (Join-Path $script:PcDir "waiting-$Owner.txt") -ErrorAction SilentlyContinue
}
