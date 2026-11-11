<#
.SYNOPSIS
    Runs the scripted half of the hardware procedure: one script per test in
    docs/HARDWARE-TESTS.md.

.DESCRIPTION
    Each test script drives its procedure (the CLI, the service, the device node, the log) and
    asserts what the procedure's "Expect" lines say. What a script cannot do - power a sink,
    walk out of range, listen, read a recorder - it asks for in a prompt and then checks what
    the driver did with it, so an "assisted" test is still an automated verdict.

    Results go to tests\HardwareTests\results\<time>\<test>.json, which is not committed: a run
    belongs with the build it was made on. A test exits 0 when it passed, 1 when it failed and 2
    when its precondition was not met (the sink does not offer the codec, for instance).

.PARAMETER Test
    Test ids to run, for example T1, T30. Default: every test.

.PARAMETER Device
    Bluetooth address, list index or partial address, as OpenA2dpCtl takes it. Needed when the
    machine has more than one sink on the driver.

.PARAMETER DriverLog
    The kernel log file (WinDbg .logopen, or DbgView saved). Without it the driver-side lines
    are recorded as unchecked rather than assumed, and the log check covers the service log.

.PARAMETER NonInteractive
    Do not wait for Enter: prompts are printed and treated as done, and a manual confirmation is
    assumed. For a known-good rig and for re-running after a code change; a real pass uses the
    prompts.

.PARAMETER List
    Show the tests and their levels, then exit.

.EXAMPLE
    .\Invoke-HardwareTests.ps1 -List

.EXAMPLE
    .\Invoke-HardwareTests.ps1 -Test T1,T8,T19 -Device E8:07:BF:1B:3D:3F -DriverLog C:\OpenA2dp\logs\t19.txt
#>
param(
    [string[]]$Test,
    [string]$Device,
    [string]$Ctl,
    [string]$DriverLog,
    [string]$ResultDir,
    [switch]$NonInteractive,
    [switch]$List
)

$ErrorActionPreference = 'Stop'

function Get-TestId([string]$Name) {
    if ($Name -match '^T(\d+)') { return [int]$Matches[1] }
    return $null
}

function Get-TestHeader([string]$Path) {
    $text = Get-Content -LiteralPath $Path -Raw
    $title = if ($text -match '-Title\s+[''"]([^''"]+)[''"]') { $Matches[1] } else { '' }
    $level = if ($text -match '-Level\s+[''"]([^''"]+)[''"]') { $Matches[1] } else { 'assisted' }
    return [pscustomobject]@{ Title = $title; Level = $level }
}

$scripts = @(Get-ChildItem -LiteralPath $PSScriptRoot -Filter 'T*.ps1' -File | Sort-Object Name |
    ForEach-Object {
        $id = Get-TestId $_.BaseName
        $header = Get-TestHeader $_.FullName
        [pscustomobject]@{ Id = $id; File = $_; Title = $header.Title; Level = $header.Level }
    })

if ($List) {
    $scripts | Sort-Object Id | Format-Table @{ Label = 'test'; Expression = { 'T{0}' -f $_.Id } },
        Level, Title -AutoSize | Out-String -Width 200 | Write-Host
    return
}

$wanted = @()
if ($Test) {
    foreach ($entry in $Test) {
        if ($entry -match '^[Tt]?(\d+)$') { $wanted += [int]$Matches[1] }
        else { throw "not a test id: '$entry' (use T1, T2, ...)" }
    }
} else {
    $wanted = @($scripts | ForEach-Object { $_.Id })
}

$selected = @($scripts | Where-Object { $wanted -contains $_.Id } | Sort-Object Id)
$missing = @($wanted | Where-Object { $n = $_; -not ($scripts | Where-Object { $_.Id -eq $n }) })
if ($missing.Count -gt 0) { throw "no script for: $(($missing | ForEach-Object { "T$_" }) -join ', ')" }
if ($selected.Count -eq 0) { throw 'nothing to run' }

if (-not $ResultDir) { $ResultDir = Join-Path $PSScriptRoot ('results\' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force -Path $ResultDir | Out-Null

$results = @()
foreach ($entry in $selected) {
    $label = 'T{0}' -f $entry.Id
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $entry.File.FullName,
        '-ResultDir', $ResultDir)
    if ($Device) { $arguments += @('-Device', $Device) }
    if ($Ctl) { $arguments += @('-Ctl', $Ctl) }
    if ($DriverLog) { $arguments += @('-DriverLog', $DriverLog) }
    if ($NonInteractive) { $arguments += '-NonInteractive' }

    $started = Get-Date
    & powershell.exe @arguments
    $code = $LASTEXITCODE
    $seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
    $outcome = switch ($code) { 0 { 'PASS' } 2 { 'SKIP' } default { 'FAIL' } }

    $json = Join-Path $ResultDir ("{0}.json" -f $label)
    $record = if (Test-Path -LiteralPath $json) { Get-Content -LiteralPath $json -Raw | ConvertFrom-Json } else { $null }
    $results += [pscustomobject]@{
        Id      = $label
        Title   = if ($record) { $record.title } else { $entry.Title }
        Level   = $entry.Level
        Outcome = $outcome
        Seconds = $seconds
        Reason  = if ($record) { $record.reason } else { '' }
    }
}

Write-Host ''
Write-Host '=== summary ==='
$results | Format-Table Id, Outcome, Seconds, Title -AutoSize | Out-String -Width 200 | Write-Host
$counts = $results | Group-Object Outcome | ForEach-Object { '{0} {1}' -f $_.Count, $_.Name }
Write-Host ("{0}   results: {1}" -f ($counts -join ', '), $ResultDir)

$failed = @($results | Where-Object { $_.Outcome -eq 'FAIL' })
if ($failed.Count -gt 0) {
    Write-Host ''
    foreach ($entry in $failed) { Write-Host ("FAIL {0}: {1}" -f $entry.Id, $entry.Reason) -ForegroundColor Red }
    exit 1
}
exit 0
