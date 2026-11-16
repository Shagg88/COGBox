param(
    [ValidateSet('baseline','candidate')][string]$Variant='baseline',
    [ValidateSet('off','quality')][string]$Dlss='off',
    [string]$LauncherDirectory='', [string]$OutputRoot='',
    [switch]$Restore, [switch]$DryRun
)
$ErrorActionPreference='Stop'
$Repo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if(-not $LauncherDirectory){$LauncherDirectory=Join-Path $Repo 'Gears2-Native/cogbox/build'}
if(-not $OutputRoot){$OutputRoot=Join-Path $Repo 'work/hdr-ab-judgment'}
$LauncherDirectory=(Resolve-Path -LiteralPath $LauncherDirectory).Path
if(-not(Test-Path -LiteralPath (Join-Path $LauncherDirectory 'CogBox.exe'))){throw 'Select the folder containing CogBox.exe.'}
$Target=Join-Path $LauncherDirectory 'extra-args.txt'
$State=Join-Path $OutputRoot 'launcher-settings.json'
if($Restore){
    if(-not(Test-Path -LiteralPath $State)){throw 'No saved comparison settings to restore.'}
    $Saved=Get-Content -LiteralPath $State -Raw | ConvertFrom-Json
    if($Saved.target -ne $Target){throw 'The saved launcher directory differs.'}
    if((Get-FileHash -LiteralPath $Target).Hash -ne $Saved.appliedHash){throw 'extra-args.txt was edited after preparation; retain those edits before restoring.'}
    Move-Item -LiteralPath $Target -Destination (Join-Path $OutputRoot ('completed-extra-args-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'.txt'))
    if($Saved.existed){[IO.File]::WriteAllBytes($Target,[Convert]::FromBase64String($Saved.originalBytes))}
    Move-Item -LiteralPath $State -Destination (Join-Path $OutputRoot ('completed-launcher-settings-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'.json'))
    Write-Output 'Original launcher extra arguments restored.'
    return
}
$Enabled=if($Variant -eq 'candidate'){'true'}else{'false'}
$Upscaler=if($Dlss -eq 'quality'){'dlss'}else{'off'}
$Arguments=@('--present_hdr=true','--gow2_renderer_resolution_scale=3','--gow2_upscale_mode=quality',
    "--gow2_upscaler=$Upscaler",'--gow2_present_aa=smaa','--gowj_max_fps=-1',
    '--gowj_hdr_native=true',"--gowj_hdr_single_pass=$Enabled",'--gow2_hdr_ui_automatic=true',
    "--gow2_hdr_compact_targets=$Enabled",'--gow2_hdr_scene_effects=false',
    '--gow2_frame_log=true','--gow2_frame_log_csv=true','--gow2_frame_log_every=600','--gow2_frame_log_spike_lines=0')
if($DryRun){[pscustomobject]@{Target=$Target;Variant=$Variant;Dlss=$Dlss;Arguments=$Arguments};return}
if(Get-Process gowj -ErrorAction SilentlyContinue){throw 'Save and close Judgment before preparing the next comparison.'}
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
if(Test-Path -LiteralPath $State){
    $Saved=Get-Content -LiteralPath $State -Raw | ConvertFrom-Json
    if($Saved.target -ne $Target){throw 'The saved launcher directory differs.'}
    if((Get-FileHash -LiteralPath $Target).Hash -ne $Saved.appliedHash){throw 'extra-args.txt changed; retain those edits before replacing the comparison block.'}
}else{
    $Exists=Test-Path -LiteralPath $Target
    $Original=if($Exists){[Convert]::ToBase64String([IO.File]::ReadAllBytes($Target))}else{''}
    $Saved=[pscustomobject]@{target=$Target;existed=$Exists;originalBytes=$Original;appliedHash='';variant='';dlss=''}
}
$OriginalText=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($Saved.originalBytes))
$Text=$OriginalText.TrimEnd()+"`n# Temporary Judgment 4K HDR comparison; launch Judgment only during this test.`n"+($Arguments -join "`n")+"`n"
[IO.File]::WriteAllText($Target,$Text,(New-Object Text.UTF8Encoding($false)))
$Saved.appliedHash=(Get-FileHash -LiteralPath $Target).Hash
$Saved.variant=$Variant;$Saved.dlss=$Dlss
$Saved | ConvertTo-Json | Set-Content -LiteralPath $State -Encoding UTF8
[pscustomobject]@{Variant=$Variant;Dlss=$Dlss;LauncherDirectory=$LauncherDirectory;State=$State;Next='Launch Judgment using COGBox Play.'}
