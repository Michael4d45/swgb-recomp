<#
.SYNOPSIS
  The nightly test run: tools\test.py --level soak against the integration build.

.DESCRIPTION
  Runs L0-L3 (unit, smoke, functional, soak: a few hours, every game headless and
  --multi, nothing on the screen) against A:\Games\star\builds\integration, and keeps
  the newest -Keep result folders in A:\Games\star\test-results. The run's report is
  report.html in its folder; nightly.log beside them lists each night's verdict.

  Without -Register it runs once, now. With -Register it registers (or replaces) a
  Windows scheduled task, "swgb-recomp nightly tests", for the current user, that runs
  this script daily at -At. Registering is the user's decision: the games take a few
  cores for hours, on the machine the user plays on. -Unregister removes the task.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\test_nightly.ps1                 # once, now
  powershell -ExecutionPolicy Bypass -File tools\test_nightly.ps1 -Register -At 03:00
  powershell -ExecutionPolicy Bypass -File tools\test_nightly.ps1 -Unregister
#>
param(
    [switch] $Register,
    [switch] $Unregister,
    [string] $At = '03:00',
    [string] $Level = 'soak',
    [int] $Jobs = 4,
    [int] $Keep = 14,
    [string] $Star = 'A:\Games\star',
    [string] $Build = '',
    [string] $Checkout = ''
)
$ErrorActionPreference = 'Stop'
$task = 'swgb-recomp nightly tests'
if (-not $Checkout) { $Checkout = Join-Path $Star 'swgb-recomp' }
if (-not $Build) { $Build = Join-Path $Star 'builds\integration' }

if ($Unregister) {
    Unregister-ScheduledTask -TaskName $task -Confirm:$false
    Write-Host "removed the scheduled task '$task'"
    return
}
if ($Register) {
    $args_ = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Level $Level -Jobs $Jobs -Keep $Keep -Star `"$Star`" -Build `"$Build`" -Checkout `"$Checkout`""
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $args_ -WorkingDirectory $Checkout
    $trigger = New-ScheduledTaskTrigger -Daily -At $At
    # Only while the user is logged on (the headless desktop is the user's), never woken
    # for it, stopped if it runs past 8 hours.
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Hours 8) -StartWhenAvailable `
        -DontStopIfGoingOnBatteries -AllowStartIfOnBatteries -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName $task -Action $action -Trigger $trigger -Settings $settings `
        -Description "tools\test.py --level $Level against $Build (tools\test_nightly.ps1)" -Force | Out-Null
    Write-Host "registered '$task': daily at $At, tools\test.py --level $Level --jobs $Jobs"
    return
}

$results = Join-Path $Star 'test-results'
New-Item -ItemType Directory -Force $results | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $results "$stamp-nightly-$Level"
$py = (Get-Command py -ErrorAction SilentlyContinue).Source
if (-not $py) { $py = (Get-Command python).Source; $pyArgs = @() } else { $pyArgs = @('-3') }
& $py @pyArgs (Join-Path $Checkout 'tools\test.py') --level $Level --jobs $Jobs --build $Build --results $out *>&1 |
    Tee-Object -FilePath (Join-Path $results "$stamp-nightly.out")
$code = $LASTEXITCODE
$verdict = if ($code -eq 0) { 'PASS' } else { 'FAIL' }
Add-Content -Path (Join-Path $results 'nightly.log') -Value "$stamp $verdict $Level $out\report.html"

# keep the newest $Keep nightly runs (their own folders only)
Get-ChildItem $results -Directory | Where-Object { $_.Name -like '*-nightly-*' } | Sort-Object Name -Descending |
    Select-Object -Skip $Keep | ForEach-Object { Remove-Item -Recurse -Force $_.FullName }
exit $code
