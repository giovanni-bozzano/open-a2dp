<#
.SYNOPSIS
	Builds (optionally), deploys and verifies the test-signed OpenA2dp driver in the test VM.

.DESCRIPTION
	Uses VMware vmrun (VMware Tools required in the guest). Guest credentials are read
	from .local\vm.json (untracked):
		{ "vmx": "D:\\Virtual Machines\\OpenA2dp\\OpenA2dp.vmx", "user": "...", "password": "...", "vmPassword": "..." }
	vmPassword is the VM encryption password (VMware encrypts VMs that have a vTPM); omit it if the VM is not encrypted.

	The script:
	  1. optionally builds the solution and runs CodecTests;
	  2. optionally reverts a snapshot (stopping a running VM first);
	  3. starts the VM if needed (-Start) and waits for VMware Tools;
	  4. copies the package, the OpenA2dpCtl.exe diagnostic CLI and the test
	     certificate;
	  5. trusts the certificate, installs the package, restarts the OpenA2dp device
	     and verifies the installed binary in the guest;
	  6. copies back and prints the guest install log.

	Binding is automatic when the A2DP device is present. If Windows still prefers the
	inbox BthA2dp driver, bind it manually with "Update driver > Have Disk" (the package
	is already in the driver store).

.PARAMETER Configuration
	Build configuration: Debug (default) or Release.

.PARAMETER Snapshot
	Snapshot to revert to before deploying. A running VM is stopped first.

.PARAMETER Build
	Build the solution and run CodecTests before deploying.

.PARAMETER Start
	Start the VM if it is not already running.

.PARAMETER NoRestart
	Do not restart the OpenA2dp device after installation.

.PARAMETER NoReboot
	Do not reboot the guest when the driver package requires it; fail instead.

.PARAMETER SkipTests
	With -Build, skip CodecTests.

.EXAMPLE
	.\scripts\deploy-vm.ps1 -Build

.EXAMPLE
	.\scripts\deploy-vm.ps1 -Snapshot 'Clean+Debug' -Build
#>
param(
	[ValidateSet('Debug', 'Release')]
	[string]$Configuration = 'Debug',
	[string]$Snapshot,
	[switch]$Build,
	[switch]$Start,
	[switch]$NoRestart,
	[switch]$NoReboot,
	[switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vmrun = 'D:\Program Files\VMware\VMware Workstation\vmrun.exe'
if (-not (Test-Path $vmrun)) {
	$vmrun = Join-Path $env:ProgramFiles 'VMware\VMware Workstation\vmrun.exe'
}
if (-not (Test-Path $vmrun)) {
	throw "vmrun.exe not found. Set the path at the top of $PSCommandPath."
}

$configPath = Join-Path $root '.local\vm.json'
if (-not (Test-Path $configPath)) {
	throw "Missing $configPath. Expected: { `"vmx`": `"...`", `"user`": `"...`", `"password`": `"...`", `"vmPassword`": `"...`" }"
}
$config = Get-Content $configPath -Raw | ConvertFrom-Json
$vmxFull = [System.IO.Path]::GetFullPath($config.vmx)
$base = @('-T', 'ws')
if ($config.vmPassword) {
	$base += @('-vp', $config.vmPassword)
}
$auth = $base + @('-gu', $config.user, '-gp', $config.password)

function Invoke-VmRun {
	param(
		[Parameter(Mandatory)][string]$What,
		[Parameter(Mandatory)][string[]]$Arguments
	)
	& $vmrun @Arguments
	if ($LASTEXITCODE -ne 0) {
		throw "$What failed (vmrun exit $LASTEXITCODE)."
	}
}

function Test-VmRunning {
	$lines = & $vmrun -T ws list 2>$null |
		Where-Object { $_ -and $_ -notmatch '^Total running VMs:' } |
		ForEach-Object { $_.Trim() }
	return [bool]($lines | Where-Object { [System.IO.Path]::GetFullPath($_) -ieq $vmxFull })
}

function Wait-VmRunning {
	param(
		[Parameter(Mandatory)][bool]$Running,
		[int]$TimeoutSeconds = 180
	)
	$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
	while ((Get-Date) -lt $deadline) {
		if ((Test-VmRunning) -eq $Running) {
			return
		}
		Start-Sleep -Seconds 2
	}
	$state = if ($Running) { 'running' } else { 'stopped' }
	throw "Timed out waiting for the VM to be $state."
}

function Wait-VmTools {
	param([int]$TimeoutSeconds = 180)
	$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
	while ((Get-Date) -lt $deadline) {
		$state = (& $vmrun @base checkToolsState $config.vmx 2>$null | Out-String).Trim()
		if ($LASTEXITCODE -eq 0 -and $state -eq 'running') {
			return
		}
		Start-Sleep -Seconds 3
	}
	throw 'Timed out waiting for VMware Tools. Is the guest fully booted?'
}

if ($Build) {
	$vswhereRoot = ${env:ProgramFiles(x86)}
	if (-not $vswhereRoot) {
		throw 'ProgramFiles(x86) is not defined; cannot locate vswhere.exe.'
	}
	$vswhere = Join-Path $vswhereRoot 'Microsoft Visual Studio\Installer\vswhere.exe'
	if (-not (Test-Path $vswhere)) {
		throw "vswhere.exe not found: $vswhere"
	}
	$msbuild = & $vswhere -latest -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
	if (-not $msbuild) {
		throw '64-bit MSBuild not found. Install Visual Studio with the C++ workload.'
	}

	Write-Host "Building $Configuration x64..."
	& $msbuild (Join-Path $root 'OpenA2dp.slnx') /restore /p:Configuration=$Configuration /p:Platform=x64 /m /v:m /nologo
	if ($LASTEXITCODE -ne 0) {
		throw "Build failed ($LASTEXITCODE)."
	}

	if (-not $SkipTests) {
		$tests = Join-Path $root "out\x64\$Configuration\CodecTests.exe"
		if (-not (Test-Path $tests)) {
			throw "CodecTests not found: $tests"
		}
		Write-Host 'Running CodecTests...'
		& $tests
		if ($LASTEXITCODE -ne 0) {
			throw "CodecTests failed ($LASTEXITCODE)."
		}
	}
}

$package = Join-Path $root "out\x64\$Configuration\OpenA2dp"
if (-not (Test-Path (Join-Path $package 'OpenA2dp.sys'))) {
	throw "No package in $package; build first."
}
if (-not (Test-Path (Join-Path $package 'OpenA2dp.inf'))) {
	throw "No OpenA2dp.inf in $package; build first."
}
$catalog = Get-ChildItem $package -Filter '*.cat' | Select-Object -First 1
if (-not $catalog) {
	throw "No catalog in $package; build first."
}
$ctl = Join-Path $root "out\x64\$Configuration\OpenA2dpCtl.exe"
if (-not (Test-Path $ctl)) {
	throw "OpenA2dpCtl.exe not found: $ctl; build first."
}
$svc = Join-Path $root "out\x64\$Configuration\OpenA2dpSvc.exe"
if (-not (Test-Path $svc)) {
	throw "OpenA2dpSvc.exe not found: $svc; build first."
}
$driverVer = (Select-String -Path (Join-Path $package 'OpenA2dp.inf') -Pattern '^DriverVer\s*=' | Select-Object -First 1).Line
Write-Host "Package: $package"
if ($driverVer) {
	Write-Host $driverVer.Trim()
}

# Export the WDK test certificate that signed the package. Use .NET rather than
# Get-AuthenticodeSignature/Export-Certificate: a PS7 module earlier in PSModulePath
# can make the Windows PowerShell 5.1 Security/PKI modules fail to load.
$sysPath = Join-Path $package 'OpenA2dp.sys'
try {
	$signer = [System.Security.Cryptography.X509Certificates.X509Certificate]::CreateFromSignedFile($sysPath)
} catch {
	throw "Package is not signed or its signature could not be read: $($_.Exception.Message)"
}
if (-not $signer) {
	throw 'Package is not signed; check SignMode=TestSign.'
}
$cer = Join-Path $root '.local\OpenA2dpTest.cer'
[System.IO.File]::WriteAllBytes($cer, $signer.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert))

if ($Snapshot) {
	if (Test-VmRunning) {
		Write-Host "Stopping the VM before reverting snapshot '$Snapshot'..."
		Invoke-VmRun 'Stopping the VM' ($base + @('stop', $config.vmx, 'hard'))
		Wait-VmRunning -Running $false
	}
	Write-Host "Reverting to snapshot '$Snapshot'..."
	Invoke-VmRun 'Reverting the snapshot' ($base + @('revertToSnapshot', $config.vmx, $Snapshot))
	Invoke-VmRun 'Starting the VM' ($base + @('start', $config.vmx))
}

if (-not (Test-VmRunning)) {
	if (-not $Start) {
		throw 'The VM is not running. Use -Start or start it manually.'
	}
	Write-Host 'Starting the VM...'
	Invoke-VmRun 'Starting the VM' ($base + @('start', $config.vmx))
}
Wait-VmRunning -Running $true

Write-Host 'Waiting for VMware Tools...'
Wait-VmTools

# The encoder service keeps its own binary open; stop it before replacing the
# file. A non-zero exit is expected on the first deploy (service not installed).
& $vmrun @auth runProgramInGuest $config.vmx 'C:\Windows\System32\sc.exe' 'stop' 'OpenA2dpSvc' 2>&1 | Out-Null
# Also clear an instance started by hand for debugging (svc-stdout.txt runs).
& $vmrun @auth runProgramInGuest $config.vmx 'C:\Windows\System32\taskkill.exe' '/F' '/IM' 'OpenA2dpSvc.exe' 2>&1 | Out-Null
Start-Sleep -Seconds 3

# createDirectoryInGuest fails harmlessly when the directory already exists.
& $vmrun @auth createDirectoryInGuest $config.vmx 'C:\OpenA2dp' 2>&1 | Out-Null

foreach ($file in (Get-ChildItem $package -File | Where-Object { $_.Extension -in '.sys', '.inf', '.cat' }) + (Get-Item $cer) + (Get-Item $ctl) + (Get-Item $svc)) {
	Write-Host "Copying $($file.Name)"
	Invoke-VmRun "Copying $($file.Name)" ($auth + @('CopyFileFromHostToGuest', $config.vmx, $file.FullName, "C:\OpenA2dp\$($file.Name)"))
}

# Guest-side installer. Runs elevated (vmrun uses the guest admin token), records a
# transcript, verifies the installed binary and returns a non-zero exit code on failure.
$guestScript = @'
param([switch]$NoRestart)
$ErrorActionPreference = 'Stop'
$log = 'C:\OpenA2dp\install.log'
Start-Transcript -Path $log -Force -ErrorAction SilentlyContinue | Out-Null
$exitCode = 0

# Unloads and reloads the driver for one device so a staged update becomes active.
# /restart-device is refused on some builds while a package update is pending
# ("Device is pending system reboot"); disable/enable goes through PnP instead.
function Restart-DriverDevice([string]$InstanceId) {
    # Diagnostics go to Write-Host: anything written to the output stream would be
    # captured by the caller's `if (-not (...))` and hide the return value.
    Write-Host "Restarting $InstanceId"
    try {
        pnputil.exe /restart-device $InstanceId | Out-Null
        if ($LASTEXITCODE -eq 0) { return $true }
        Write-Host "restart-device exit=$LASTEXITCODE; falling back to disable/enable."
        Disable-PnpDevice -InstanceId $InstanceId -Confirm:$false -ErrorAction Stop
        Start-Sleep -Seconds 2
        Enable-PnpDevice -InstanceId $InstanceId -Confirm:$false -ErrorAction Stop
        return $true
    } catch {
        Write-Host "restart failed: $($_.Exception.Message)"
        return $false
    }
}

try {
    Set-Location 'C:\OpenA2dp'

    # The host prints "Installing certificate and driver package..." before this
    # script starts, so a slow run needs phase timings to be diagnosable at all.
    $scriptStart = Get-Date
    function Step([string]$Name) {
        Write-Host ("{0:HH:mm:ss.fff} +{1,6:N1}s {2}" -f (Get-Date), ((Get-Date) - $scriptStart).TotalSeconds, $Name)
    }
    Step 'start'

    certutil.exe -f -addstore Root OpenA2dpTest.cer
    if ($LASTEXITCODE -ne 0) { throw "certutil Root failed ($LASTEXITCODE)" }
    Step 'root certificate trusted'

    certutil.exe -f -addstore TrustedPublisher OpenA2dpTest.cer
    if ($LASTEXITCODE -ne 0) { throw "certutil TrustedPublisher failed ($LASTEXITCODE)" }
    Step 'publisher certificate trusted'

    pnputil.exe /add-driver OpenA2dp.inf /install
    $pnputilExit = $LASTEXITCODE
    Step "pnputil finished (exit $pnputilExit)"
    "pnputil exit=$pnputilExit"
    # 259 (ERROR_NO_MORE_ITEMS) means the package is already installed and up to
    # date, which is a normal redeploy, not a failure.
    if ($pnputilExit -ne 0 -and $pnputilExit -ne 3010 -and $pnputilExit -ne 259) { throw "pnputil failed ($pnputilExit)" }
    $alreadyCurrent = ($pnputilExit -eq 259)

    # 3010 means the package was staged, not that the update failed: the new image
    # only becomes active once the device is restarted. Try that before asking for
    # a reboot, which is the expensive path on a test VM.
    $devices = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.Service -eq 'OpenA2dp' })
    if ($devices.Count -eq 0) {
        'No present OpenA2dp device found; the package is installed but not bound.'
        if ($pnputilExit -eq 3010) {
            'Reboot required to complete the driver installation.'
            $exitCode = 3010
        }
    } else {
        $activated = ($pnputilExit -eq 0 -or $alreadyCurrent)
        if ($NoRestart) {
            'Skipping device restart (-NoRestart).'
        } else {
            $activated = $true
            foreach ($device in $devices) {
                if (-not (Restart-DriverDevice $device.InstanceId)) { $activated = $false }
            }
            Step 'device restart issued'

            $deadline = (Get-Date).AddSeconds(60)
            do {
                Start-Sleep -Seconds 2
                $devices = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.Service -eq 'OpenA2dp' })
                $notOk = @($devices | Where-Object { $_.Status -ne 'OK' })
            } while ($notOk.Count -gt 0 -and (Get-Date) -lt $deadline)
            if ($notOk.Count -gt 0) { throw "Device not OK after restart: $($notOk.InstanceId -join ', ')" }
            Step 'device is OK'
            if ($devices.Count -eq 0) {
                'warning: no OpenA2dp device is present after the restart; the headset may need to reconnect.'
            }
        }

        # Verify what the device is actually bound to. The published INF (oemNN.inf)
        # is a copy of the package INF, and its DriverVer identifies the DriverStore
        # folder the device loads from. System32\drivers only exists for old-style
        # installs, so the previous check silently skipped verification.
        function Get-InfDriverVer([string]$Path) {
            if (-not (Test-Path $Path)) { return $null }
            $line = Select-String -Path $Path -Pattern '^\s*DriverVer\s*=' -ErrorAction SilentlyContinue | Select-Object -First 1
            if (-not $line) { return $null }
            return ($line.Line -replace '^\s*DriverVer\s*=\s*', '').Trim()
        }

        $infPath = $null
        if ($devices.Count -gt 0) {
            $infPath = (Get-PnpDeviceProperty -InstanceId $devices[0].InstanceId -KeyName 'DEVPKEY_Device_DriverInfPath' -ErrorAction SilentlyContinue).Data
        }

        $sourceHash = (Get-FileHash 'C:\OpenA2dp\OpenA2dp.sys' -Algorithm SHA256).Hash
        $sourceVer = Get-InfDriverVer 'C:\OpenA2dp\OpenA2dp.inf'
        $boundVer = $null
        if ($infPath) { $boundVer = Get-InfDriverVer "C:\Windows\INF\$infPath" }
        "driver version: bound=$boundVer package=$sourceVer"
        if ($boundVer -and $sourceVer -and $boundVer -ne $sourceVer) {
            throw "The device is bound to a different driver version ($boundVer) than the deployed package ($sourceVer)."
        }

        $bound = $null
        if ($boundVer) {
            $bound = Get-ChildItem 'C:\Windows\System32\DriverStore\FileRepository' -Directory -Filter 'opena2dp.inf_*' -ErrorAction SilentlyContinue |
                Where-Object { (Get-InfDriverVer (Join-Path $_.FullName 'opena2dp.inf')) -eq $boundVer } |
                Select-Object -First 1
        }
        if ($bound) {
            $installedHash = (Get-FileHash (Join-Path $bound.FullName 'OpenA2dp.sys') -Algorithm SHA256).Hash
            "binary hash: package=$sourceHash bound=$installedHash ($($bound.Name))"
            if ($sourceHash -ne $installedHash) { throw 'Bound driver binary does not match the deployed package.' }
        } else {
            "warning: could not resolve the DriverStore folder for '$infPath'; binary verification skipped."
        }
        Step 'binding verified'

        # Install or refresh the encoder service. It runs as LocalSystem (the control
        # device grants access to SYSTEM and Administrators only) and must be
        # registered before a headset connects: AAC is offered per negotiation.
        $svcExe = 'C:\OpenA2dp\OpenA2dpSvc.exe'
        if (Test-Path $svcExe) {
            & sc.exe query OpenA2dpSvc > $null 2>&1
            if ($LASTEXITCODE -ne 0) {
                'installing OpenA2dpSvc'
                & sc.exe create OpenA2dpSvc binPath= $svcExe start= auto DisplayName= 'OpenA2dp AAC encoder' > $null
                if ($LASTEXITCODE -ne 0) { throw "sc create failed ($LASTEXITCODE)" }
            }
            & sc.exe config OpenA2dpSvc binPath= $svcExe start= auto > $null
            if ($LASTEXITCODE -ne 0) { throw "sc config failed ($LASTEXITCODE)" }
            & sc.exe description OpenA2dpSvc 'AAC-LC encoder for the OpenA2dp A2DP source driver.' > $null
            # Restart on failure: 5 s, then 5 s, then 5 s, counter reset daily.
            & sc.exe failure OpenA2dpSvc reset= 86400 actions= restart/5000/restart/5000/restart/5000 > $null
            & sc.exe stop OpenA2dpSvc > $null 2>&1
            Start-Sleep -Seconds 2
            & sc.exe start OpenA2dpSvc > $null
            $svcExit = $LASTEXITCODE
            if ($svcExit -ne 0 -and $svcExit -ne 1056) { throw "sc start failed ($svcExit)" }
            (& sc.exe query OpenA2dpSvc | Select-String 'STATE' | Select-Object -First 1).Line.Trim()
            Step 'service started'
        } else {
            'warning: OpenA2dpSvc.exe is not in C:\OpenA2dp; the encoder service was not installed.'
        }

        # Activation is the only thing that makes a staged update real: if the device
        # could not be restarted, a reboot is required no matter what pnputil said.
        if (-not $activated) {
            'Reboot required to complete the driver installation.'
            $exitCode = 3010
        } else {
            'Install completed successfully.'
            Step 'done'
        }
    }
} catch {
    "ERROR: $($_.Exception.Message)"
    $exitCode = 1
} finally {
    Stop-Transcript -ErrorAction SilentlyContinue | Out-Null
}
exit $exitCode
'@

$guestScriptPath = Join-Path $root '.local\install.ps1'
Set-Content -Path $guestScriptPath -Value $guestScript -Encoding ascii
Invoke-VmRun 'Copying the guest installer' ($auth + @('CopyFileFromHostToGuest', $config.vmx, $guestScriptPath, 'C:\OpenA2dp\install.ps1'))

# A batch wrapper keeps vmrun from mangling the command line and propagates the exit code.
$guestCommand = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\OpenA2dp\install.ps1'
if ($NoRestart) {
	$guestCommand += ' -NoRestart'
}
$cmdPath = Join-Path $root '.local\install.cmd'
Set-Content -Path $cmdPath -Encoding ascii -Value @(
	'@echo off',
	$guestCommand,
	'exit /b %errorlevel%'
)
Invoke-VmRun 'Copying the guest launcher' ($auth + @('CopyFileFromHostToGuest', $config.vmx, $cmdPath, 'C:\OpenA2dp\install.cmd'))

$logPath = Join-Path $root '.local\install.log'
$rebooted = $false
while ($true) {
	Write-Host 'Installing certificate and driver package...'
	$installArgs = $auth + @('runProgramInGuest', $config.vmx, 'C:\Windows\System32\cmd.exe', '/c C:\OpenA2dp\install.cmd')
	$installStart = Get-Date
	& $vmrun @installArgs
	$installExit = $LASTEXITCODE
	Write-Host ("guest installer returned $installExit after {0:N1}s" -f ((Get-Date) - $installStart).TotalSeconds)

	try {
		Invoke-VmRun 'Copying the install log' ($auth + @('CopyFileFromGuestToHost', $config.vmx, 'C:\OpenA2dp\install.log', $logPath))
	} catch {
		Write-Warning $_.Exception.Message
	}
	if (Test-Path $logPath) {
		Get-Content $logPath
	}

	if ($installExit -eq 0) {
		break
	}

	if ($installExit -eq 3010 -and -not $NoReboot -and -not $rebooted) {
		Write-Host 'The guest needs a reboot to finish the driver update; rebooting...'
		# runProgramInGuest shutdown.exe is unreliable here (it can return 1 and leave
		# the guest running); a hard stop/start is deterministic for this test VM.
		Invoke-VmRun 'Stopping the VM for reboot' ($base + @('stop', $config.vmx, 'hard'))
		Wait-VmRunning -Running $false
		Invoke-VmRun 'Starting the VM after reboot' ($base + @('start', $config.vmx))
		Wait-VmRunning -Running $true
		Wait-VmTools -TimeoutSeconds 300
		$rebooted = $true
		continue
	}

	if ($installExit -eq 3010) {
		throw "The driver package was staged but the running driver could not be restarted, so the guest is still on the previous image. Reboot the guest to activate it (or rerun without -NoReboot). See $logPath."
	}
	throw "Guest installation failed (exit $installExit); see $logPath."
}

if ($NoRestart) {
	Write-Host "`nDeployment complete. The package is installed; the device was not restarted (-NoRestart)."
} else {
	Write-Host "`nDeployment complete. The package is installed and the device was restarted and verified when present."
}
Write-Host 'CLI: C:\OpenA2dp\OpenA2dpCtl.exe (run it elevated in the guest; "OpenA2dpCtl help" for usage).'
