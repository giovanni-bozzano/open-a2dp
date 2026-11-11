<#
.SYNOPSIS
    Deploys the test-signed OpenA2dp driver on this machine, or into the test VM with -Vm.

.DESCRIPTION
    This machine is the default target: the package, the tools and the test certificate are
    copied to C:\OpenA2dp, the driver is swapped without a reboot and the encoder service is
    started. Needs an elevated prompt.

    With -Vm the same installer runs in the guest, so the two targets cannot drift apart. It
    uses vmrun (VMware Tools required in the guest), found on the host instead of from a
    fixed path. Settings are read from .local\vm.json (untracked) unless -VmConfig names
    another file:
        { "vmx": "test-vm\\OpenA2dp.vmx", "user": "...", "password": "...", "vmPassword": "..." }
    vmx is an absolute path or one relative to the repository root, so a config never has to
    name a drive. vmPassword is the VM encryption password; omit it if the VM is not encrypted.

    Update strategy (no reboot, ever):
      1. stop the encoder service, the CLI and the settings window (they hold handles on the
         driver);
      2. disable the OpenA2dp device node so the driver unloads;
      3. confirm the driver really unloaded; if not, re-enable and ABORT;
      4. install the new package while nothing is loaded;
      5. enable the device so the new image loads fresh;
      6. verify the device and the loaded binary, then (re)start the encoder service.

    The guest installer runs detached (vmrun -noWait). The host polls a result file
    with a timeout, so a stuck guest process can never hang this script.
    The script never reboots or hard-stops the VM, except for the explicit -Snapshot revert.

.PARAMETER Configuration  Debug (default) or Release.
.PARAMETER Vm             Deploy into the test VM instead of this machine. The VM options below
                          need it; without it they are refused rather than ignored.
.PARAMETER Uninstall      Restore check instead of a deploy: remove the OpenA2dp driver package,
                          the encoder service and the per-sink device node, then verify that the
                          driver, its service, its control links and its package are gone and
                          that a sink is bound to BthA2dp again. Leaves the copied binaries in
                          C:\OpenA2dp so a redeploy needs no rebuild.
.PARAMETER Snapshot       Snapshot to revert to before deploying (a running VM is stopped first).
.PARAMETER Build          Build the solution and run CodecTests first.
.PARAMETER Start          Start the VM if it is not running.
.PARAMETER NoRestart      Stage the package only; do not touch the device. Takes effect when it is next restarted.
.PARAMETER SkipTests      With -Build, skip CodecTests.
.PARAMETER KeepAudio      Do not stop the Windows Audio service before updating (by default it is stopped to end any active stream, then restarted).
.PARAMETER InstallTimeoutSec  Overall limit for the guest installer (default 240).
.PARAMETER VmConfig      VM config JSON (default .local\vm.json); a relative path is read from the repository root.
.PARAMETER VmRun         vmrun.exe to use (default: OPENA2DP_VMRUN, PATH, the Workstation and
                         Player install folders, then their registry InstallPath).
#>
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$Vm,
    [string]$Snapshot,
    [switch]$Build,
    [switch]$Start,
    [switch]$NoRestart,
    [switch]$SkipTests,
    [switch]$KeepAudio,
    [switch]$Uninstall,
    [int]$InstallTimeoutSec = 240,
    [string]$VmConfig,
    [string]$VmRun
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$local = Join-Path $root '.local'
New-Item -ItemType Directory -Force -Path $local | Out-Null

# A VM option on a local deploy is a mistake worth reporting: silently ignoring it would look
# like the VM had been updated.
if (-not $Vm -and ($Snapshot -or $Start -or $VmConfig -or $VmRun)) {
    throw 'Snapshot, Start, VmConfig and VmRun are VM options: pass -Vm to deploy into the VM.'
}
if (-not $Vm) {
    # Checked before anything is read or copied, so a plain double-click fails with the one
    # thing that has to change rather than half way through.
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Deploying a driver needs an elevated prompt. Re-run this from an Administrator console, or use -Vm.'
    }
}

# ---------- host setup ----------
# vmrun.exe is found rather than configured, so the host layout does not matter: an explicit
# -VmRun, then OPENA2DP_VMRUN, PATH, the Workstation/Player folders under both Program Files
# roots, then the InstallPath those installs record in the registry.
function Resolve-VmRun {
    $candidates = @()
    if ($VmRun) { $candidates += $VmRun }
    if ($env:OPENA2DP_VMRUN) { $candidates += $env:OPENA2DP_VMRUN }
    $onPath = Get-Command 'vmrun.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($onPath) { $candidates += $onPath.Source }
    $products = @('VMware Workstation', 'VMware Player')
    foreach ($product in $products) {
        foreach ($rootDir in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
            if ($rootDir) { $candidates += Join-Path (Join-Path $rootDir (Join-Path 'VMware' $product)) 'vmrun.exe' }
        }
    }
    # An install somewhere else records its folder here, for either product and registry view.
    foreach ($product in $products) {
        foreach ($hive in @('HKLM:\SOFTWARE', 'HKLM:\SOFTWARE\WOW6432Node')) {
            $key = Join-Path $hive (Join-Path 'VMware, Inc.' $product)
            $install = (Get-ItemProperty -Path $key -Name InstallPath -ErrorAction SilentlyContinue).InstallPath
            if ($install) { $candidates += Join-Path $install.TrimEnd('\') 'vmrun.exe' }
        }
    }
    foreach ($candidate in $candidates) {
        if (-not $candidate) { continue }
        $path = "$candidate".Trim().Trim('"')
        if ($path -and (Test-Path -LiteralPath $path)) { return (Resolve-Path -LiteralPath $path).Path }
    }
    throw 'vmrun.exe not found. Pass -VmRun <path>, set OPENA2DP_VMRUN, or add it to PATH.'
}
if ($Vm) {
    $vmrun = Resolve-VmRun

    $configPath = if ($VmConfig) { $VmConfig } else { Join-Path $local 'vm.json' }
    # A relative -VmConfig is read from the repository, so the same command works from any directory.
    if (-not [System.IO.Path]::IsPathRooted($configPath)) { $configPath = Join-Path $root $configPath }
    $configPath = [System.IO.Path]::GetFullPath($configPath)
    if (-not (Test-Path -LiteralPath $configPath)) {
        throw "Missing $configPath. Expected: { `"vmx`": `"test-vm\OpenA2dp.vmx`", `"user`": `"...`", `"password`": `"...`" } with vmx absolute or relative to $root."
    }
    $config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
    # Quoting and a relative path are both accepted, so a pasted value works as it is.
    $vmxPath = "$($config.vmx)".Trim().Trim('"')
    if (-not $vmxPath) { throw "$configPath does not name a vmx." }
    if (-not [System.IO.Path]::IsPathRooted($vmxPath)) { $vmxPath = Join-Path $root $vmxPath }
    $vmxFull = [System.IO.Path]::GetFullPath($vmxPath)
    if (-not (Test-Path -LiteralPath $vmxFull)) { throw "VMX not found: $vmxFull" }
    $vmx = $vmxFull
    $base = @('-T', 'ws')
    if ($config.vmPassword) { $base += @('-vp', $config.vmPassword) }
    $auth = $base + @('-gu', $config.user, '-gp', $config.password)
}

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
# The restore check only removes what an earlier deploy installed, so it needs no package.
if (-not $Uninstall) {
    $out = Join-Path $root "out\x64\$Configuration"
    $package = Join-Path $out 'OpenA2dp'
    $sys = Join-Path $package 'OpenA2dp.sys'
    $inf = Join-Path $package 'OpenA2dp.inf'
    $ctl = Join-Path $out 'OpenA2dpCtl.exe'
    $svc = Join-Path $out 'OpenA2dpSvc.exe'
    $ui = Join-Path $out 'OpenA2dpConfig.exe'
    foreach ($p in $sys, $inf, $ctl, $svc, $ui) {
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
}

# ---------- VM deploy ----------
# Everything below until the guest script runs only against the VM; a local deploy returns
# from its own branch before reaching it.
if ($Vm) {
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

# The service, the CLI and the settings window lock their own binaries, and an open window
# also holds a handle on the driver. Stop the service cleanly first: killing it would trigger
# its restart-on-failure recovery and relaunch it within 5 s. Arguments are passed separately;
# vmrun/sc.exe mis-parse a single "stop OpenA2dpSvc" argument.
function Stop-GuestBinaries {
    $sys32 = 'C:\Windows\System32'
    [void](Try-VmRun ($auth + @('runProgramInGuest', $vmx, "$sys32\sc.exe", 'stop', 'OpenA2dpSvc')))
    Start-Sleep -Seconds 3
    [void](Try-VmRun ($auth + @('runProgramInGuest', $vmx, "$sys32\taskkill.exe", '/F', '/IM', 'OpenA2dpSvc.exe')))
    [void](Try-VmRun ($auth + @('runProgramInGuest', $vmx, "$sys32\taskkill.exe", '/F', '/IM', 'OpenA2dpCtl.exe')))
    [void](Try-VmRun ($auth + @('runProgramInGuest', $vmx, "$sys32\taskkill.exe", '/F', '/IM', 'OpenA2dpConfig.exe')))
    Start-Sleep -Seconds 2
}
if (-not $Uninstall) {
    Stop-GuestBinaries

    $files = @(Get-ChildItem $package -File | Where-Object { $_.Extension -in '.sys', '.inf', '.cat' }) +
        (Get-Item $cer) + (Get-Item $ctl) + (Get-Item $svc) + (Get-Item $ui)
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
}
} # -Vm

# ---------- guest installer ----------
$guestScript = @'
param([switch]$NoRestart, [switch]$KeepAudio, [switch]$Uninstall, [string]$Directory = 'C:\OpenA2dp')
$ErrorActionPreference = 'Stop'
$dir = $Directory
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

# Like Wait-Until, but reports what it sees every few seconds. Re-enabling the device waits for
# a Bluetooth sink to come back, which is not instant: without a progress line the wait reads as
# a hang, and the device codes are what says whether the node is absent or its driver refused to
# start.
function Wait-Reported([scriptblock]$Condition, [int]$TimeoutSec, [string]$What) {
    $start = Get-Date
    $deadline = $start.AddSeconds($TimeoutSec)
    $nextReport = $start
    while ($true) {
        if (& $Condition) { return $true }
        $now = Get-Date
        if ($now -ge $deadline) { return [bool](& $Condition) }
        if ($now -ge $nextReport) {
            $devices = @(Get-Devices)
            $codes = (@($devices | ForEach-Object { [string]$_.ConfigManagerErrorCode }) -join ',')
            Log ('  waiting for {0}: {1:N0}s, present {2}, codes [{3}]' -f $What,
                ($now - $start).TotalSeconds, $devices.Count, $codes)
            $nextReport = $now.AddSeconds(5)
        }
        Start-Sleep -Milliseconds 500
    }
}

# Ends every process that holds a handle on the driver. Restart-on-failure is disabled first,
# so a forced kill cannot relaunch the service while the caller works on the device.
function Stop-OpenA2dpProcesses {
    [void](Run 'sc.exe' 'failure OpenA2dpSvc reset= 0 actions= ""' 20)
    [void](Run 'sc.exe' 'stop OpenA2dpSvc' 20)
    [void](Run 'taskkill.exe' '/F /IM OpenA2dpSvc.exe' 20)
    [void](Run 'taskkill.exe' '/F /IM OpenA2dpCtl.exe' 20)
    [void](Run 'taskkill.exe' '/F /IM OpenA2dpConfig.exe' 20)
    [void](Wait-Until { (& sc.exe query OpenA2dpSvc 2>&1 | Out-String) -notmatch 'RUNNING|STOP_PENDING' } 15)
}

# Published atomically so the host never reads a half-written file.
function Publish-Result([string]$Text) {
    foreach ($try in 1..20) {
        try {
            Set-Content -Path "$resultFile.tmp" -Value $Text -Encoding Ascii -ErrorAction Stop
            Move-Item -Path "$resultFile.tmp" -Destination $resultFile -Force -ErrorAction Stop
            return
        } catch { Start-Sleep -Milliseconds 200 }
    }
}

# A PnP call that times out is stuck in the kernel (the driver is not completing a stop or
# remove request). Killing pnputil does not cancel that, and more PnP calls would queue
# behind it, so the caller stops here and records what is needed to find the blocked wait.
function Dump-HangDiagnostics([string[]]$Ids) {
    Log '--- hang diagnostics ---'
    foreach ($id in $Ids) { [void](Run 'pnputil.exe' "/enum-devices /instanceid `"$id`"" 20) }
    [void](Run 'sc.exe' 'queryex OpenA2dp' 20)
    [void](Run 'sc.exe' 'queryex audiosrv' 20)
    foreach ($n in 'audiodg', 'pnputil', 'OpenA2dpSvc', 'OpenA2dpCtl', 'OpenA2dpConfig') {
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

# Removes the deployed package and service, then proves the sink is back on Microsoft's A2DP
# source service. Returns 'RESTORED', or 'FAIL: <problems>'.
function Invoke-Restore {
    $problems = @()

    # 1. Remove the package whose INF is ours. /uninstall also removes the device nodes using
    #    it, which hands the sink back to the Bluetooth stack.
    $published = $null
    $packages = @()
    foreach ($line in ((& pnputil.exe /enum-drivers 2>&1 | Out-String) -split "`r?`n")) {
        if ($line -match 'Published Name:\s+(\S+)') { $published = $Matches[1] }
        elseif ($published -and $line -match 'Original Name:\s+opena2dp\.inf') { $packages += $published }
    }
    if ($packages.Count -eq 0) { Log 'no OpenA2dp package in the driver store' }
    foreach ($pkg in $packages) {
        Log "removing driver package $pkg"
        [void](Run 'pnputil.exe' "/delete-driver $pkg /uninstall /force" 120)
    }

    # 2. Remove both services. pnputil uninstalls the device and deletes the package but
    #    leaves the kernel service entry behind, so the restore deletes it explicitly;
    #    otherwise a later install would run against a stale ImagePath.
    [void](Run 'sc.exe' 'delete OpenA2dpSvc' 20)
    [void](Run 'sc.exe' 'delete OpenA2dp' 20)

    # 3. Give Windows a moment to rebind the sink, then check the restore from every side:
    #    the device node, both services, the package, the image, the sink's own binding and
    #    the control-device links the CLI enumerates.
    $bthA2dp = { @(Get-CimInstance Win32_PnPEntity -Filter "Service='BthA2dp'" -ErrorAction SilentlyContinue).Count -gt 0 }
    if (-not (Wait-Until $bthA2dp 30)) { Log 'warning: nothing is bound to BthA2dp yet; is a sink paired and connected?' }

    if (@(Get-Devices).Count -gt 0) { $problems += 'an OpenA2dp device node is still present' }
    if ((& sc.exe query OpenA2dp 2>&1 | Out-String) -notmatch '1060') { $problems += 'the OpenA2dp driver service is still registered' }
    if ((& sc.exe query OpenA2dpSvc 2>&1 | Out-String) -notmatch '1060') { $problems += 'the OpenA2dp encoder service is still registered' }
    if (Get-CimInstance Win32_SystemDriver -Filter "Name='OpenA2dp'" -ErrorAction SilentlyContinue) { $problems += 'Win32_SystemDriver still lists OpenA2dp' }
    if ((& pnputil.exe /enum-drivers 2>&1 | Out-String) -match 'opena2dp\.inf') { $problems += 'the OpenA2dp package is still in the driver store' }
    if (Test-Path (Join-Path $env:SystemRoot 'System32\drivers\OpenA2dp.sys')) { $problems += 'OpenA2dp.sys is still in System32\drivers' }

    $sinks = @(Get-CimInstance Win32_PnPEntity -Filter "Service='BthA2dp'" -ErrorAction SilentlyContinue)
    foreach ($sink in $sinks) { Log "    BthA2dp: $($sink.Name) status=$($sink.Status)" }
    if ($sinks.Count -eq 0) { $problems += 'no device is bound to BthA2dp: pair and connect the sink, then rerun' }

    $ctl = Join-Path $dir 'OpenA2dpCtl.exe'
    if (Test-Path $ctl) {
        $listing = (& $ctl list 2>&1 | Out-String)
        Log ('    OpenA2dpCtl list: ' + (($listing.Trim() -split "`r?`n") -join ' | '))
        if ($listing -notmatch 'No OpenA2dp devices found') { $problems += 'OpenA2dpCtl still finds a control device' }
    }

    # 4. Trusting the test certificate is part of the deploy, so a restore takes it back out of
    #    the machine stores; TrustedPublisher is cleared too, for deploys made before it stopped
    #    using it.
    $testCert = Join-Path $dir 'OpenA2dpTest.cer'
    if (Test-Path -LiteralPath $testCert) {
        $thumbprint = (New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $testCert).Thumbprint
        [void](Run 'certutil.exe' '-delstore Root OpenA2dpTest.cer' 30)
        [void](Run 'certutil.exe' '-delstore TrustedPublisher OpenA2dpTest.cer' 30)
        $remaining = @(Get-ChildItem Cert:\LocalMachine\Root -ErrorAction SilentlyContinue |
            Where-Object { $_.Thumbprint -eq $thumbprint })
        if ($remaining.Count -gt 0) { $problems += 'the test certificate is still trusted as a root' }
        else { Log '    test certificate removed from the machine Root store' }
    } else {
        Log '    no test certificate in the deploy folder; nothing to untrust'
    }

    if ($problems.Count -eq 0) { return 'RESTORED' }
    foreach ($problem in $problems) { Log "    problem: $problem" }
    return "FAIL: $($problems -join '; ')"
}

# ---------- restore check ----------
# Runs alone: remove what a deploy installed, then verify the sink is back on BthA2dp.
if ($Uninstall) {
    try {
        Set-Location $dir
        Log "restore check pid $PID"
        Stop-OpenA2dpProcesses
        $resultText = Invoke-Restore
    } catch {
        Log "ERROR: $($_.Exception.Message)"
        $resultText = "FAIL: $($_.Exception.Message)"
    }
    if ($resultText -ne 'RESTORED') { $exitCode = 1 }
    Log $resultText
    Publish-Result $resultText
    exit $exitCode
}

try {
    Set-Location $dir
    Log "installer pid $PID, NoRestart=$NoRestart"
    $os = Get-CimInstance Win32_OperatingSystem
    Log "os: $($os.Caption) build $($os.BuildNumber)"

    # 1. Release handles held by the encoder service and the CLI.
    Stop-OpenA2dpProcesses

    # 2. Trust the test certificate in the one store that decides it: a signature is trusted
    #    because its chain ends in the machine's root store. TrustedPublisher would be a second
    #    copy of the same decision for no more reach, so it is left alone - and cleared here in
    #    case an earlier deploy put one there.
    if ((Run 'certutil.exe' '-f -addstore Root OpenA2dpTest.cer' 30) -ne 0) { throw 'certutil Root failed' }
    [void](Run 'certutil.exe' '-f -delstore TrustedPublisher OpenA2dpTest.cer' 30)

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
                # The engine can outlive the service by a few seconds, and while it holds the
                # endpoint it vetoes the node's removal (Kernel-PnP 225, seen locally: the device
                # then comes back a minute late). Wait for it before touching PnP.
                if (-not (Wait-Until { -not (Get-Process -Name audiodg -ErrorAction SilentlyContinue) } 15)) {
                    Log 'warning: audiodg still runs after audiosrv stopped; a device removal can be vetoed while it holds the endpoint.'
                }
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
        $ready = {
            $d = Get-Devices
            ($d.Count -gt 0) -and (@($d | Where-Object { $_.ConfigManagerErrorCode -ne 0 }).Count -eq 0)
        }
        # A running Bluetooth sink takes a moment to come back, and the driver refuses to start
        # while the enumerator cannot hand it the device info, so a node can report a problem code
        # for a while and still end up fine (seen locally: code 0 ten seconds after a 60 s wait
        # gave up). Wait longer, and nudge a node that is still failing once before calling it.
        $ok = Wait-Reported $ready 120 'the sink to come back'
        if (-not $ok) {
            $failing = @(Get-Devices | Where-Object { $_.ConfigManagerErrorCode -ne 0 })
            foreach ($d in $failing) {
                Log "restarting $($d.PNPDeviceID) after code $($d.ConfigManagerErrorCode)"
                [void](Run 'pnputil.exe' "/restart-device `"$($d.PNPDeviceID)`"" 60)
            }
            if ($failing.Count -gt 0) { $ok = Wait-Reported $ready 60 'the restarted device' }
        }
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
Publish-Result $resultText
exit $exitCode
'@

$guestScriptPath = Join-Path $local 'install.ps1'
Set-Content -Path $guestScriptPath -Value $guestScript -Encoding ascii

# ---------- local deploy ----------
if (-not $Vm) {
    # The same installer runs here, from the same files, against the same C:\OpenA2dp layout the
    # VM uses: a rebuild is never blocked by a running service in the output folder, and a path
    # in a log or a result file means the same thing on both targets.
    $targetDir = 'C:\OpenA2dp'
    New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
    if (-not $Uninstall) {
        # The service, the CLI and the window lock their own binaries, so they are stopped
        # before the copy, exactly as the VM path does.
        & sc.exe stop OpenA2dpSvc 2>&1 | Out-Null
        Start-Sleep -Seconds 2
        foreach ($name in 'OpenA2dpSvc.exe', 'OpenA2dpCtl.exe', 'OpenA2dpConfig.exe') {
            & taskkill.exe /F /IM $name 2>&1 | Out-Null
        }
        Start-Sleep -Seconds 1
        $files = @(Get-ChildItem $package -File | Where-Object { $_.Extension -in '.sys', '.inf', '.cat' }) +
            (Get-Item $cer) + (Get-Item $ctl) + (Get-Item $svc) + (Get-Item $ui)
        foreach ($file in $files) {
            Write-Host "Copying $($file.Name)"
            Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $targetDir $file.Name) -Force
        }
    }
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $guestScriptPath, '-Directory', $targetDir)
    if ($NoRestart) { $arguments += '-NoRestart' }
    if ($KeepAudio) { $arguments += '-KeepAudio' }
    if ($Uninstall) { $arguments += '-Uninstall' }
    Write-Host "`nInstalling on this machine ($targetDir)..."
    & powershell.exe @arguments
    $installerCode = $LASTEXITCODE
    $logPath = Join-Path $targetDir 'install.log'
    $resultPath = Join-Path $targetDir 'result.txt'
    if (Test-Path -LiteralPath $logPath) { Get-Content -LiteralPath $logPath | ForEach-Object { Write-Host $_ } }
    if (-not (Test-Path -LiteralPath $resultPath)) {
        throw "The installer left no result file (exit $installerCode). Log: $logPath"
    }
    $result = (Get-Content -LiteralPath $resultPath -Raw).Trim()
    $expected = if ($Uninstall) { 'RESTORED' } else { 'OK' }
    $what = if ($Uninstall) { 'restore check' } else { 'installation' }
    if ($result -ne $expected) { throw "Local $what failed: $result  (log: $logPath)" }
    if ($Uninstall) {
        Write-Host "`nRestore check passed. The sink is back on BthA2dp; play audio to it to confirm the Microsoft path works."
    } elseif ($NoRestart) {
        Write-Host "`nDeployment complete. The package is installed; the device was not restarted (-NoRestart)."
    } else {
        Write-Host "`nDeployment complete. The driver was reloaded without a reboot and verified."
    }
    Write-Host "CLI: $targetDir\OpenA2dpCtl.exe (elevated; `"OpenA2dpCtl help`" for usage)."
    Write-Host "UI:  $targetDir\OpenA2dpConfig.exe (also elevated)."
    return
}

# ---------- hand the installer to the guest ----------
Invoke-VmRun 'Copying the guest installer' ($auth + @('CopyFileFromHostToGuest', $vmx, $guestScriptPath, 'C:\OpenA2dp\install.ps1'))

# A tiny batch wrapper keeps vmrun from mangling the PowerShell command line.
$psCmd = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\OpenA2dp\install.ps1'
if ($NoRestart) { $psCmd += ' -NoRestart' }
if ($KeepAudio) { $psCmd += ' -KeepAudio' }
if ($Uninstall) { $psCmd += ' -Uninstall' }
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
$expected = if ($Uninstall) { 'RESTORED' } else { 'OK' }
if ($result -ne $expected) {
    $what = if ($Uninstall) { 'restore check' } else { 'installation' }
    throw "Guest $what failed: $result  (log: $logPath)"
}

if ($Uninstall) {
    Write-Host "`nRestore check passed. The sink is back on BthA2dp; play audio to it to confirm the Microsoft path works."
} elseif ($NoRestart) {
    Write-Host "`nDeployment complete. The package is installed; the device was not restarted (-NoRestart)."
} else {
    Write-Host "`nDeployment complete. The driver was reloaded without a reboot and verified."
}
Write-Host 'CLI: C:\OpenA2dp\OpenA2dpCtl.exe (run it elevated in the guest; "OpenA2dpCtl help" for usage).'
Write-Host 'UI:  C:\OpenA2dp\OpenA2dpConfig.exe (also elevated; the window asks for that itself).'