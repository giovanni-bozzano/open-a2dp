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

# createDirectoryInGuest fails harmlessly when the directory already exists.
& $vmrun @auth createDirectoryInGuest $config.vmx 'C:\OpenA2dp' 2>&1 | Out-Null

foreach ($file in (Get-ChildItem $package -File | Where-Object { $_.Extension -in '.sys', '.inf', '.cat' }) + (Get-Item $cer) + (Get-Item $ctl)) {
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
try {
    Set-Location 'C:\OpenA2dp'

    certutil.exe -f -addstore Root OpenA2dpTest.cer
    if ($LASTEXITCODE -ne 0) { throw "certutil Root failed ($LASTEXITCODE)" }

    certutil.exe -f -addstore TrustedPublisher OpenA2dpTest.cer
    if ($LASTEXITCODE -ne 0) { throw "certutil TrustedPublisher failed ($LASTEXITCODE)" }

    pnputil.exe /add-driver OpenA2dp.inf /install
    $pnputilExit = $LASTEXITCODE
    "pnputil exit=$pnputilExit"
    if ($pnputilExit -ne 0 -and $pnputilExit -ne 3010) { throw "pnputil failed ($pnputilExit)" }

    if ($pnputilExit -eq 3010) {
        'Reboot required to complete the driver installation.'
        $exitCode = 3010
    } else {
        $devices = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.Service -eq 'OpenA2dp' })
        if ($devices.Count -eq 0) {
            'No present OpenA2dp device found; the package is installed but not bound.'
        } else {
            if (-not $NoRestart) {
                foreach ($device in $devices) {
                    "Restarting $($device.InstanceId)"
                    pnputil.exe /restart-device $device.InstanceId
                    $restartExit = $LASTEXITCODE
                    if ($restartExit -ne 0 -and $restartExit -ne 3010) { throw "restart-device failed ($restartExit)" }
                }

                $deadline = (Get-Date).AddSeconds(60)
                do {
                    Start-Sleep -Seconds 2
                    $devices = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.Service -eq 'OpenA2dp' })
                    $notOk = @($devices | Where-Object { $_.Status -ne 'OK' })
                } while ($notOk.Count -gt 0 -and (Get-Date) -lt $deadline)
                if ($notOk.Count -gt 0) { throw "Device not OK after restart: $($notOk.InstanceId -join ', ')" }
            } else {
                'Skipping device restart (-NoRestart).'
            }

            if ($devices.Count -gt 0) {
                $driverVersion = (Get-PnpDeviceProperty -InstanceId $devices[0].InstanceId -KeyName 'DEVPKEY_Device_DriverVersion' -ErrorAction SilentlyContinue).Data
                if ($driverVersion) { "driver version: $driverVersion" }
            }
        }

        $source = 'C:\OpenA2dp\OpenA2dp.sys'
        $installed = 'C:\Windows\System32\drivers\OpenA2dp.sys'
        if (Test-Path $installed) {
            $sourceHash = (Get-FileHash $source -Algorithm SHA256).Hash
            $installedHash = (Get-FileHash $installed -Algorithm SHA256).Hash
            "binary hash: source=$sourceHash installed=$installedHash"
            if ($sourceHash -ne $installedHash) { throw 'Installed driver binary does not match the deployed package.' }
        } else {
            "warning: $installed not found; binary verification skipped."
        }

        'Install completed successfully.'
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
	& $vmrun @installArgs
	$installExit = $LASTEXITCODE

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

	throw "Guest installation failed (exit $installExit); see $logPath."
}

Write-Host "`nDeployment complete. The package is installed and the device was restarted and verified when present."
Write-Host 'CLI: C:\OpenA2dp\OpenA2dpCtl.exe (run it elevated in the guest; "OpenA2dpCtl help" for usage).'
