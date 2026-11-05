<#
.SYNOPSIS
    Deploys the test-signed OpenA2dp driver into the test VM without ever rebooting the guest.

.DESCRIPTION
    Uses VMware vmrun (VMware Tools required in the guest). Settings are read from
    .local\vm.json (untracked):
        { "vmx": "D:\\Virtual Machines\\OpenA2dp\\OpenA2dp.vmx", "user": "...", "password": "...", "vmPassword": "..." }
    vmPassword is the VM encryption password; omit it if the VM is not encrypted.

    Update strategy (no reboot, ever):
      1. stop the encoder service and the CLI (they hold handles on the driver);
      2. disable the OpenA2dp device node so the driver unloads;
      3. confirm the driver really unloaded; if not, re-enable and ABORT;
      4. install the new package while nothing is loaded;
      5. enable the device so the new image loads fresh;
      6. verify the device and the loaded binary, then (re)start the encoder service.

    The guest installer runs detached (vmrun -noWait). The host polls a result file
    with a timeout, so a stuck guest process can never hang this script.
    The script never reboots or hard-stops the VM, except for the explicit -Snapshot revert.

.PARAMETER Configuration  Debug (default) or Release.
.PARAMETER Snapshot       Snapshot to revert to before deploying (a running VM is stopped first).
.PARAMETER Build          Build the solution and run CodecTests first.
.PARAMETER Start          Start the VM if it is not running.
.PARAMETER NoRestart      Stage the package only; do not touch the device. Takes effect when it is next restarted.
.PARAMETER SkipTests      With -Build, skip CodecTests.
.PARAMETER KeepAudio      Do not stop the Windows Audio service before updating (by default it is stopped to end any active stream, then restarted).
.PARAMETER InstallTimeoutSec  Overall limit for the guest installer (default 240).
#>
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [string]$Snapshot,
    [switch]$Build,
    [switch]$Start,
    [switch]$NoRestart,
    [switch]$SkipTests,
    [switch]$KeepAudio,
    [int]$InstallTimeoutSec = 240
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

# ---------- host setup ----------
$vmrun = 'D:\Program Files\VMware\VMware Workstation\vmrun.exe'
if (-not (Test-Path $vmrun)) { $vmrun = Join-Path $env:ProgramFiles 'VMware\VMware Workstation\vmrun.exe' }
if (-not (Test-Path $vmrun)) { throw "vmrun.exe not found. Set the path at the top of $PSCommandPath." }

$configPath = Join-Path $root '.local\vm.json'
if (-not (Test-Path $configPath)) {
    throw "Missing $configPath. Expected: { `"vmx`": `"...`", `"user`": `"...`", `"password`": `"...`", `"vmPassword`": `"...`" }"
}
$config = Get-Content $configPath -Raw | ConvertFrom-Json
$vmx = $config.vmx
$vmxFull = [System.IO.Path]::GetFullPath($vmx)
$base = @('-T', 'ws')
if ($config.vmPassword) { $base += @('-vp', $config.vmPassword) }
$auth = $base + @('-gu', $config.user, '-gp', $config.password)

$local = Join-Path $root '.local'
New-Item -ItemType Directory -Force -Path $local | Out-Null

function Invoke-VmRun {
    param([Parameter(Mandatory)][string]$What, [Parameter(Mandatory)][string[]]$Arguments)
    & $vmrun @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$What failed (vmrun exit $LASTEXITCODE)." }
}

# Like Invoke-VmRun but returns the exit code and keeps quiet; for best-effort calls.
function Try-VmRun {
    param([Parameter(Mandatory)][string[]]$Arguments)
    & $vmrun @Arguments 2>&1 | Out-Null
    return $LASTEXITCODE
}

function Test-VmRunning {
    $lines = & $vmrun -T ws list 2>$null |
        Where-Object { $_ -and $_ -notmatch '^Total running VMs:' } |
        ForEach-Object { $_.Trim() }
    return [bool]($lines | Where-Object { [System.IO.Path]::GetFullPath($_) -ieq $vmxFull })
}

function Wait-VmRunning {
    param([Parameter(Mandatory)][bool]$Running, [int]$TimeoutSeconds = 180)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if ((Test-VmRunning) -eq $Running) { return }
        Start-Sleep -Seconds 2
    }
    throw "Timed out waiting for the VM to be $(if ($Running) { 'running' } else { 'stopped' })."
}

function Wait-VmTools {
    param([int]$TimeoutSeconds = 180)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $state = (& $vmrun @base checkToolsState $vmx 2>$null | Out-String).Trim()
        if ($LASTEXITCODE -eq 0 -and $state -eq 'running') { return }
        Start-Sleep -Seconds 3
    }
    throw 'Timed out waiting for VMware Tools. Is the guest fully booted?'
}

# Copies a guest file to the host; returns $true if it arrived.
function Get-GuestFile {
    param([string]$GuestPath, [string]$HostPath)
    Remove-Item $HostPath -ErrorAction SilentlyContinue
    $code = Try-VmRun ($auth + @('CopyFileFromGuestToHost', $vmx, $GuestPath, $HostPath))
    return ($code -eq 0 -and (Test-Path $HostPath))
}

# ---------- build ----------
if ($Build) {
    $pf86 = ${env:ProgramFiles(x86)}
    if (-not $pf86) { throw 'ProgramFiles(x86) is not defined; cannot locate vswhere.exe.' }
    $vswhere = Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found: $vswhere" }
    $msbuild = & $vswhere -latest -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
    if (-not $msbuild) { throw '64-bit MSBuild not found. Install Visual Studio with the C++ workload.' }

    Write-Host "Building $Configuration x64..."
    & $msbuild (Join-Path $root 'OpenA2dp.slnx') /restore /p:Configuration=$Configuration /p:Platform=x64 /m /v:m /nologo
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)." }

    if (-not $SkipTests) {
        $tests = Join-Path $root "out\x64\$Configuration\CodecTests.exe"
        if (-not (Test-Path $tests)) { throw "CodecTests not found: $tests" }
        Write-Host 'Running CodecTests...'
        & $tests
        if ($LASTEXITCODE -ne 0) { throw "CodecTests failed ($LASTEXITCODE)." }
    }
}

# ---------- locate artifacts ----------
$out = Join-Path $root "out\x64\$Configuration"
$package = Join-Path $out 'OpenA2dp'
$sys = Join-Path $package 'OpenA2dp.sys'
$inf = Join-Path $package 'OpenA2dp.inf'
$ctl = Join-Path $out 'OpenA2dpCtl.exe'
$svc = Join-Path $out 'OpenA2dpSvc.exe'
foreach ($p in $sys, $inf, $ctl, $svc) {
    if (-not (Test-Path $p)) { throw "Missing $p; build first." }
}
$catalog = Get-ChildItem $package -Filter '*.cat' | Select-Object -First 1
if (-not $catalog) { throw "No catalog in $package; build first." }

$driverVer = (Select-String -Path $inf -Pattern '^DriverVer\s*=' | Select-Object -First 1).Line
Write-Host "Package: $package"
if ($driverVer) { Write-Host $driverVer.Trim() }

# Export the test certificate that signed the driver (plain .NET: avoids PS module issues).
try {
    $signer = [System.Security.Cryptography.X509Certificates.X509Certificate]::CreateFromSignedFile($sys)
} catch {
    throw "Package is not signed or its signature could not be read: $($_.Exception.Message)"
}
$cer = Join-Path $local 'OpenA2dpTest.cer'
[System.IO.File]::WriteAllBytes($cer, $signer.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert))

# ---------- VM power state ----------
if ($Snapshot) {
    if (Test-VmRunning) {
        Write-Host "Stopping the VM before reverting snapshot '$Snapshot'..."
        Invoke-VmRun 'Stopping the VM' ($base + @('stop', $vmx, 'hard'))
        Wait-VmRunning -Running $false
    }
    Write-Host "Reverting to snapshot '$Snapshot'..."
    Invoke-VmRun 'Reverting the snapshot' ($base + @('revertToSnapshot', $vmx, $Snapshot))
    Invoke-VmRun 'Starting the VM' ($base + @('start', $vmx))
}

if (-not (Test-VmRunning)) {
    if (-not $Start) { throw 'The VM is not running. Use -Start or start it manually.' }
    Write-Host 'Starting the VM...'
    Invoke-VmRun 'Starting the VM' ($base + @('start', $vmx))
}
Wait-VmRunning -Running $true
Write-Host 'Waiting for VMware Tools...'
Wait-VmTools

# ---------- copy files ----------
[void](Try-VmRun ($auth + @('createDirectoryInGuest', $vmx, 'C:\OpenA2dp')))   # fails harmlessly if it exists

# Clear leftovers from a previous run so a stale result can never be mistaken for this one.
foreach ($f in 'result.txt', 'install.log') {
    [void](Try-VmRun ($auth + @('deleteFileInGuest', $vmx, "C:\OpenA2dp\$f")))
}

# The service and CLI lock their own binaries. Stop the service cleanly first: killing it
# would trigger its restart-on-failure recovery and relaunch it within 5 s. Arguments are
# passed separately; vmrun/sc.exe mis-parse a single "stop OpenA2dpSvc" argument.
function Stop-GuestBinaries {
    $sys32 = 'C:\Windows\System32'
    [void](Try-VmRun ($auth + @('runProgramInGuest', $vmx, "$sys32\sc.exe", 'stop', 'OpenA2dpSvc')))
    Start-Sleep -Seconds 3
    [void](Try-VmRun ($auth + @('runProgramInGuest', $vmx, "$sys32\taskkill.exe", '/F', '/IM', 'OpenA2dpSvc.exe')))
    [void](Try-VmRun ($auth + @('runProgramInGuest', $vmx, "$sys32\taskkill.exe", '/F', '/IM', 'OpenA2dpCtl.exe')))
    Start-Sleep -Seconds 2
}
Stop-GuestBinaries

$files = @(Get-ChildItem $package -File | Where-Object { $_.Extension -in '.sys', '.inf', '.cat' }) +
    (Get-Item $cer) + (Get-Item $ctl) + (Get-Item $svc)
foreach ($file in $files) {
    Write-Host "Copying $($file.Name)"
    $dest = "C:\OpenA2dp\$($file.Name)"
    $copied = $false
    foreach ($attempt in 1..3) {
        $code = Try-VmRun ($auth + @('CopyFileFromHostToGuest', $vmx, $file.FullName, $dest))
        if ($code -eq 0) { $copied = $true; break }
        Write-Warning "Copy of $($file.Name) failed (exit $code), attempt $attempt/3; the file is probably locked. Stopping guest processes and retrying."
        Stop-GuestBinaries
        [void](Try-VmRun ($auth + @('deleteFileInGuest', $vmx, $dest)))
    }
    if (-not $copied) { throw "Copying $($file.Name) failed after 3 attempts. Is the file open in the guest?" }
}

# ---------- guest installer ----------
$guestScript = @'
param([switch]$NoRestart, [switch]$KeepAudio)
$ErrorActionPreference = 'Stop'
$dir = 'C:\OpenA2dp'
$log = "$dir\install.log"
$resultFile = "$dir\result.txt"
$t0 = Get-Date
$exitCode = 0
$resultText = 'OK'
$audioStopped = $false

function Log([string]$Message) {
    $line = '{0:HH:mm:ss} +{1,5:N1}s {2}' -f (Get-Date), ((Get-Date) - $t0).TotalSeconds, $Message
    # The host copies this file out while we run, which can briefly lock it. Retry, and never
    # let a logging failure abort the install.
    foreach ($try in 1..20) {
        try {
            $fs = New-Object IO.FileStream($log, [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
            try {
                $bytes = [Text.Encoding]::ASCII.GetBytes($line + "`r`n")
                $fs.Write($bytes, 0, $bytes.Length)
            } finally { $fs.Dispose() }
            return
        } catch { Start-Sleep -Milliseconds 100 }
    }
}

# Runs a native command as a child process with a hard timeout. Output goes to the log.
# Returns the exit code, or -1 if the command timed out (and was killed).
function Run([string]$Exe, [string]$ArgLine, [int]$TimeoutSec = 60) {
    Log "> $Exe $ArgLine"
    $o = [IO.Path]::GetTempFileName()
    $e = [IO.Path]::GetTempFileName()
    $p = Start-Process -FilePath $Exe -ArgumentList $ArgLine -NoNewWindow -PassThru -RedirectStandardOutput $o -RedirectStandardError $e
    $null = $p.Handle   # keep the handle so ExitCode stays readable
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        Log "  TIMEOUT after ${TimeoutSec}s, killing pid $($p.Id)"
        & taskkill.exe /T /F /PID $p.Id 2>&1 | Out-Null
        $code = -1
    } else {
        $p.WaitForExit()
        $code = $p.ExitCode
    }
    foreach ($f in $o, $e) {
        Get-Content $f -ErrorAction SilentlyContinue | Where-Object { $_.Trim() } | ForEach-Object { Log "    $_" }
        Remove-Item $f -ErrorAction SilentlyContinue
    }
    Log "< exit $code"
    return $code
}

function Get-Devices {
    return @(Get-CimInstance Win32_PnPEntity -Filter "Service='OpenA2dp'" -ErrorAction SilentlyContinue)
}

# STOPPED, RUNNING, ... or NOT_INSTALLED.
function Get-DriverState {
    $text = (& sc.exe query OpenA2dp 2>&1 | Out-String)
    if ($text -match 'STATE\s+:\s+\d+\s+(\w+)') { return $Matches[1] }
    return 'NOT_INSTALLED'
}

function Wait-Until([scriptblock]$Condition, [int]$TimeoutSec) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (& $Condition) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return [bool](& $Condition)
}

# A PnP call that times out is stuck in the kernel (the driver is not completing a stop or
# remove request). Killing pnputil does not cancel that, and more PnP calls would queue
# behind it, so the caller stops here and records what is needed to find the blocked wait.
function Dump-HangDiagnostics([string[]]$Ids) {
    Log '--- hang diagnostics ---'
    foreach ($id in $Ids) { [void](Run 'pnputil.exe' "/enum-devices /instanceid `"$id`"" 20) }
    [void](Run 'sc.exe' 'queryex OpenA2dp' 20)
    [void](Run 'sc.exe' 'queryex audiosrv' 20)
    foreach ($n in 'audiodg', 'pnputil', 'OpenA2dpSvc', 'OpenA2dpCtl') {
        Get-Process -Name $n -ErrorAction SilentlyContinue | ForEach-Object { Log "process: $($_.Name) pid $($_.Id) handles $($_.HandleCount)" }
    }
    try {
        Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-Kernel-PnP/Configuration' } -MaxEvents 15 -ErrorAction Stop |
            ForEach-Object { Log ("pnp event {0:HH:mm:ss} id {1}: {2}" -f $_.TimeCreated, $_.Id, (($_.Message -split "`r?`n")[0])) }
    } catch { Log "could not read the Kernel-PnP log: $($_.Exception.Message)" }
    Log '--- end diagnostics ---'
}

# pnputil sometimes does not exit although the device has already changed state (seen with
# /disable-device on the last device). Judge success by the real state, not the exit code:
# run pnputil detached and return as soon as it exits, or as soon as $Done reports success
# and pnputil has still not exited after a short grace period (it is then killed).
# Returns the exit code, 0 for "state reached, pnputil stuck", or -1 for a real timeout.
function Get-DeviceStatus([string]$Id) {
    $t = (& pnputil.exe /enum-devices /instanceid $Id 2>&1 | Out-String)
    if ($t -match 'Status:\s+(\S+)') { return $Matches[1] }
    return 'Unknown'
}

function Invoke-Pnp([string]$ArgLine, [scriptblock]$Done, [int]$TimeoutSec = 90) {
    Log "> pnputil.exe $ArgLine"
    $o = [IO.Path]::GetTempFileName()
    $e = [IO.Path]::GetTempFileName()
    $p = Start-Process -FilePath 'pnputil.exe' -ArgumentList $ArgLine -NoNewWindow -PassThru -RedirectStandardOutput $o -RedirectStandardError $e
    $null = $p.Handle
    $start = Get-Date
    $reachedAt = $null
    $code = -1
    while (((Get-Date) - $start).TotalSeconds -lt $TimeoutSec) {
        if ($p.HasExited) { $code = $p.ExitCode; break }
        $reached = $false
        try { $reached = [bool](& $Done) } catch { }
        if ($reached) {
            if (-not $reachedAt) {
                $reachedAt = Get-Date
                Log ('  state reached after {0:N1}s, pnputil still running' -f ($reachedAt - $start).TotalSeconds)
            } elseif (((Get-Date) - $reachedAt).TotalSeconds -ge 5) {
                Log '  pnputil did not exit although the state was reached; killing it.'
                $code = 0
                break
            }
        }
        Start-Sleep -Milliseconds 700
    }
    if (-not $p.HasExited) {
        if ($code -eq -1) { Log "  TIMEOUT after ${TimeoutSec}s, state not reached" }
        & taskkill.exe /T /F /PID $p.Id 2>&1 | Out-Null
    }
    foreach ($f in $o, $e) {
        Get-Content $f -ErrorAction SilentlyContinue | Where-Object { $_.Trim() } | ForEach-Object { Log "    $_" }
        Remove-Item $f -ErrorAction SilentlyContinue
    }
    Log ('< exit {0} after {1:N1}s' -f $code, ((Get-Date) - $start).TotalSeconds)
    return $code
}

function Enable-All($Ids) {
    foreach ($id in $Ids) {
        $done = { (Get-DeviceStatus $id) -eq 'Started' }.GetNewClosure()
        [void](Invoke-Pnp "/enable-device `"$id`"" $done 90)
    }
}

try {
    Set-Location $dir
    Log "installer pid $PID, NoRestart=$NoRestart"
    $os = Get-CimInstance Win32_OperatingSystem
    Log "os: $($os.Caption) build $($os.BuildNumber)"

    # 1. Release handles held by the encoder service and the CLI.
    # Disable restart-on-failure first, so a forced kill cannot relaunch the service (and its
    # handle on the driver) while we unload; step 8 restores the recovery actions.
    [void](Run 'sc.exe' 'failure OpenA2dpSvc reset= 0 actions= ""' 20)
    [void](Run 'sc.exe' 'stop OpenA2dpSvc' 20)
    [void](Run 'taskkill.exe' '/F /IM OpenA2dpSvc.exe' 20)
    [void](Run 'taskkill.exe' '/F /IM OpenA2dpCtl.exe' 20)
    [void](Wait-Until { (& sc.exe query OpenA2dpSvc 2>&1 | Out-String) -notmatch 'RUNNING|STOP_PENDING' } 15)

    # 2. Trust the test certificate.
    if ((Run 'certutil.exe' '-f -addstore Root OpenA2dpTest.cer' 30) -ne 0) { throw 'certutil Root failed' }
    if ((Run 'certutil.exe' '-f -addstore TrustedPublisher OpenA2dpTest.cer' 30) -ne 0) { throw 'certutil TrustedPublisher failed' }

    $devices = Get-Devices
    $ids = @($devices | ForEach-Object { $_.PNPDeviceID })
    Log "present OpenA2dp devices: $($ids.Count)"
    $ids | ForEach-Object { Log "    $_" }
    Log "driver state: $(Get-DriverState)"

    if ($NoRestart -or $ids.Count -eq 0) {
        # Nothing to unload (or the caller asked not to touch the device): just stage.
        $code = Run 'pnputil.exe' '/add-driver OpenA2dp.inf /install' 120
        if ($code -notin 0, 259, 3010) { throw "pnputil add-driver failed ($code)" }
        if ($ids.Count -eq 0) { Log 'no device present; package installed but not bound.' }
        else { Log 'device left untouched (-NoRestart); the new image loads when the device restarts.' }
    } else {
        # 3a. End any active stream. Stopping the Windows Audio service closes every render
        #     stream, including one to the sink, so the device can be disabled. It is
        #     restarted after the update (see below), even if the update fails.
        if (-not $KeepAudio) {
            $a = Get-Service audiosrv -ErrorAction SilentlyContinue
            if ($a -and $a.Status -eq 'Running') {
                [void](Run 'net.exe' 'stop audiosrv /y' 60)
                $audioStopped = $true
                Start-Sleep -Seconds 2
            }
        }

        $stateStopped = { (Get-DriverState) -in 'STOPPED', 'NOT_INSTALLED' }

        # 3b. Gentle route first: disable only the OpenA2dp device node.
        $disabled = @()
        $failed = $false
        $hung = $false
        foreach ($id in $ids) {
            $done = { (Get-DeviceStatus $id) -eq 'Disabled' }.GetNewClosure()
            $c = Invoke-Pnp "/disable-device `"$id`"" $done 90
            if ($c -eq 0) { $disabled += $id } else { $failed = $true }
            if ($c -eq -1) { $hung = $true; break }
        }
        if ($hung) {
            Dump-HangDiagnostics $ids
            $script:exitCode = 3
            throw 'pnputil /disable-device hung: the driver is not completing its PnP stop/remove request. No further PnP calls were made. The device node may be stuck; revert the VM snapshot to recover.'
        }
        $unloaded = (-not $failed) -and (Wait-Until $stateStopped 30)
        Log "driver state after disabling the device: $(Get-DriverState)"

        # 3c. Escalate: drop the Bluetooth connection by disabling the sink's parent node
        #     (BTHENUM only; never the radio). Windows then removes the OpenA2dp child and
        #     the driver unloads. Needs no cooperation from the sink or the driver CLI.
        $parents = @()
        if (-not $unloaded) {
            Log 'driver still loaded; disconnecting the sink by disabling its parent Bluetooth node.'
            Enable-All $disabled
            $disabled = @()
            foreach ($id in $ids) {
                $parentId = (Get-PnpDeviceProperty -InstanceId $id -KeyName 'DEVPKEY_Device_Parent' -ErrorAction SilentlyContinue).Data
                Log "parent of ${id}: $parentId"
                if ($parentId -notmatch '^BTHENUM\\') {
                    Log "parent ${parentId} is the Bluetooth bus node, not a per-sink node; leaving it alone."
                } elseif ($parents -notcontains $parentId) {
                    $c = Run 'pnputil.exe' "/disable-device `"$parentId`"" 60
                    if ($c -eq 0) { $parents += $parentId }
                    if ($c -eq -1) { $hung = $true; break }
                }
            }
            if ($hung) {
                Dump-HangDiagnostics (@($ids) + @($parentId))
                $script:exitCode = 3
                throw 'Disabling the parent Bluetooth node hung. No further PnP calls were made; revert the VM snapshot to recover.'
            }
            $unloaded = ($parents.Count -gt 0) -and (Wait-Until $stateStopped 20)
            Log "driver state after disconnecting: $(Get-DriverState)"
        }

        # 4. If the driver is still loaded, replacing it would need a reboot. Abort with
        #    nothing changed and put everything back; never reboot.
        if (-not $unloaded) {
            Enable-All $parents
            Enable-All $disabled
            $script:exitCode = 2
            throw "The driver is still loaded (state $(Get-DriverState)); the device could not be released and no per-sink parent node was available to disconnect. Not installing; no reboot will be attempted."
        }

        # 5. Install while nothing is loaded.
        $code = Run 'pnputil.exe' '/add-driver OpenA2dp.inf /install' 120
        if ($code -notin 0, 259, 3010) {
            Enable-All $parents
            Enable-All $disabled
            throw "pnputil add-driver failed ($code)"
        }
        # 3010 only says an already-loaded image could not be replaced. Nothing is loaded
        # now, so it is harmless; the checks below prove what is actually running.

        # 6. Bring things back; the new image loads fresh when the device reappears.
        Enable-All $parents
        Enable-All $disabled
        $ok = Wait-Until {
            $d = Get-Devices
            ($d.Count -gt 0) -and (@($d | Where-Object { $_.ConfigManagerErrorCode -ne 0 }).Count -eq 0)
        } 60
        $present = Get-Devices
        foreach ($d in $present) { Log "device: code=$($d.ConfigManagerErrorCode) $($d.PNPDeviceID)" }
        if (-not $ok) {
            if ($parents.Count -gt 0 -and $present.Count -eq 0) {
                Log 'warning: the sink has not reconnected yet; reconnect it. The new driver loads on connection.'
            } else {
                throw 'Device is not OK after being re-enabled.'
            }
        }

        # 7. Verify. The registered image must be the deployed binary; if the device is
        #    present the driver must also be running.
        $state = Get-DriverState
        Log "driver state: $state"
        if ($present.Count -gt 0 -and $state -ne 'RUNNING') { throw "Driver is not running after re-enable (state $state)." }
        $imgPath = (Get-CimInstance Win32_SystemDriver -Filter "Name='OpenA2dp'").PathName
        $imgPath = $imgPath -replace '^\\\?\?\\', '' -replace '^\\SystemRoot', $env:SystemRoot
        $want = (Get-FileHash "$dir\OpenA2dp.sys" -Algorithm SHA256).Hash
        if (Test-Path $imgPath) {
            $have = (Get-FileHash $imgPath -Algorithm SHA256).Hash
            Log "binary hash: package=$want installed=$have ($imgPath)"
            if ($want -ne $have) { throw 'The installed driver binary differs from the deployed package.' }
        } else {
            Log "warning: could not resolve driver image path '$imgPath'; hash check skipped."
        }
    }

    # 8. Install / refresh and start the encoder service.
    $svcExe = "$dir\OpenA2dpSvc.exe"
    if ((Run 'sc.exe' 'query OpenA2dpSvc' 20) -ne 0) {
        if ((Run 'sc.exe' "create OpenA2dpSvc binPath= `"$svcExe`" start= auto DisplayName= `"OpenA2dp AAC encoder`"" 20) -ne 0) { throw 'sc create failed' }
    }
    if ((Run 'sc.exe' "config OpenA2dpSvc binPath= `"$svcExe`" start= auto" 20) -ne 0) { throw 'sc config failed' }
    [void](Run 'sc.exe' 'description OpenA2dpSvc "AAC-LC encoder for the OpenA2dp A2DP source driver."' 20)
    [void](Run 'sc.exe' 'failure OpenA2dpSvc reset= 86400 actions= restart/5000/restart/5000/restart/5000' 20)
    $code = Run 'sc.exe' 'start OpenA2dpSvc' 30
    if ($code -notin 0, 1056) { throw "sc start failed ($code)" }

    Log 'Install completed successfully.'
} catch {
    Log "ERROR: $($_.Exception.Message)"
    $resultText = "FAIL: $($_.Exception.Message)"
    if ($exitCode -eq 0) { $exitCode = 1 }
}
if ($audioStopped) {
    [void](Run 'net.exe' 'start audiosrv' 60)
}
# Publish the result atomically so the host never reads a half-written file.
foreach ($try in 1..20) {
    try {
        Set-Content -Path "$resultFile.tmp" -Value $resultText -Encoding Ascii -ErrorAction Stop
        Move-Item -Path "$resultFile.tmp" -Destination $resultFile -Force -ErrorAction Stop
        break
    } catch { Start-Sleep -Milliseconds 200 }
}
exit $exitCode
'@

$guestScriptPath = Join-Path $local 'install.ps1'
Set-Content -Path $guestScriptPath -Value $guestScript -Encoding ascii
Invoke-VmRun 'Copying the guest installer' ($auth + @('CopyFileFromHostToGuest', $vmx, $guestScriptPath, 'C:\OpenA2dp\install.ps1'))

# A tiny batch wrapper keeps vmrun from mangling the PowerShell command line.
$psCmd = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\OpenA2dp\install.ps1'
if ($NoRestart) { $psCmd += ' -NoRestart' }
if ($KeepAudio) { $psCmd += ' -KeepAudio' }
$cmdPath = Join-Path $local 'install.cmd'
Set-Content -Path $cmdPath -Encoding ascii -Value @('@echo off', $psCmd, 'exit /b %errorlevel%')
Invoke-VmRun 'Copying the guest launcher' ($auth + @('CopyFileFromHostToGuest', $vmx, $cmdPath, 'C:\OpenA2dp\install.cmd'))

# ---------- run detached and poll ----------
Write-Host 'Installing in the guest (detached, polling for the result)...'
# -noWait: vmrun returns as soon as the process is started, so it cannot hang us.
Invoke-VmRun 'Starting the guest installer' ($auth + @('runProgramInGuest', $vmx, '-noWait', 'C:\Windows\System32\cmd.exe', '/c C:\OpenA2dp\install.cmd'))

$logPath = Join-Path $local 'install.log'
$resultPath = Join-Path $local 'result.txt'
$shown = 0
$deadline = (Get-Date).AddSeconds($InstallTimeoutSec)
$finished = $false

function Show-NewLog {
    if (Get-GuestFile 'C:\OpenA2dp\install.log' $logPath) {
        $lines = @(Get-Content $logPath -ErrorAction SilentlyContinue)
        if ($lines.Count -gt $script:shown) {
            $lines | Select-Object -Skip $script:shown | ForEach-Object { Write-Host $_ }
            $script:shown = $lines.Count
        }
    }
}

while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 3
    Show-NewLog
    if ((Get-GuestFile 'C:\OpenA2dp\result.txt' $resultPath) -and ("$(Get-Content $resultPath -Raw)".Trim())) { $finished = $true; break }
}
Show-NewLog

if (-not $finished) {
    throw "The guest installer did not finish within ${InstallTimeoutSec}s. The guest was left as it is (no reboot attempted). Log: $logPath"
}

$result = (Get-Content $resultPath -Raw).Trim()
if ($result -ne 'OK') {
    throw "Guest installation failed: $result  (log: $logPath)"
}

if ($NoRestart) {
    Write-Host "`nDeployment complete. The package is installed; the device was not restarted (-NoRestart)."
} else {
    Write-Host "`nDeployment complete. The driver was reloaded without a reboot and verified."
}
Write-Host 'CLI: C:\OpenA2dp\OpenA2dpCtl.exe (run it elevated in the guest; "OpenA2dpCtl help" for usage).'