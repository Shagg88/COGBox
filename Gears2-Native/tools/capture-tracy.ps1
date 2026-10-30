# Capture the running COGBox game's on-demand Tracy client. Does not launch/stop a game.
param(
    [string]$Output = '',
    [ValidateRange(1, 3600)][int]$Seconds = 30,
    [string]$Address = '127.0.0.1',
    [ValidateRange(1, 65535)][int]$Port = 8086
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$captureTool = Join-Path $projectRoot 'third_party/tracy-tools-0.14.1/tracy-capture.exe'
if (-not (Test-Path -LiteralPath $captureTool)) {
    throw 'Matching Tracy tools are missing. See notes/tracy-profiling-2026-10-29.md.'
}
if (-not $Output) {
    $Output = Join-Path $projectRoot ("out/tracy/capture-{0}.tracy" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$capturePath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Output)
if (Test-Path -LiteralPath $capturePath) { throw "Capture already exists: $capturePath" }
New-Item -ItemType Directory -Force (Split-Path -Parent $capturePath) | Out-Null
& $captureTool -a $Address -p $Port -s $Seconds -m 512 -o $capturePath
if ($LASTEXITCODE -ne 0) { throw "Tracy capture failed with exit code $LASTEXITCODE" }
Write-Host "Saved Tracy capture: $capturePath"
