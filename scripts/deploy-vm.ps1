<#
.SYNOPSIS
	Copies the test-signed OpenA2dp driver package into the test VM and installs it.

.DESCRIPTION
	Uses VMware vmrun (requires VMware Tools in the guest). Guest credentials are read
	from .local\vm.json (untracked):
		{ "vmx": "D:\\Virtual Machines\\OpenA2dp\\OpenA2dp.vmx", "user": "...", "password": "..." }

	Steps: optional snapshot revert, copy package + test certificate, trust the
	certificate, add the driver to the store with pnputil.
	Binding the driver to the headset (Update driver > Have Disk) is still manual,
	because Windows ranks the inbox BthA2dp driver higher.
#>
param(
	[ValidateSet('Debug', 'Release')]
	[string]$Configuration = 'Debug',
	[string]$Snapshot,
	[switch]$Build
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vmrun = 'D:\Program Files\VMware\VMware Workstation\vmrun.exe'
$config = Get-Content (Join-Path $root '.local\vm.json') -Raw | ConvertFrom-Json
$auth = @('-T', 'ws', '-gu', $config.user, '-gp', $config.password)

function Invoke-VmRun {
	& $vmrun @args
	if ($LASTEXITCODE -ne 0) {
		throw "vmrun $($args[0..3] -join ' ') failed ($LASTEXITCODE)"
	}
}

if ($Build) {
	$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
	& $msbuild (Join-Path $root 'driver\OpenA2dp.vcxproj') -restore /p:Configuration=$Configuration /p:Platform=x64 /nologo /v:m
	if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
}

$package = Join-Path $root "out\x64\$Configuration\OpenA2dp"
if (-not (Test-Path (Join-Path $package 'OpenA2dp.sys'))) { throw "No package in $package; build first." }

# Export the WDK test certificate that signed the package.
$signer = (Get-AuthenticodeSignature (Join-Path $package 'OpenA2dp.sys')).SignerCertificate
if (-not $signer) { throw 'Package is not signed; check SignMode=TestSign.' }
$cer = Join-Path $root '.local\OpenA2dpTest.cer'
Export-Certificate -Cert $signer -FilePath $cer -Force | Out-Null

if ($Snapshot) {
	Write-Host "Reverting to snapshot '$Snapshot'..."
	Invoke-VmRun -T ws revertToSnapshot $config.vmx $Snapshot
	Invoke-VmRun -T ws start $config.vmx
}

Write-Host 'Waiting for VMware Tools...'
& $vmrun -T ws checkToolsState $config.vmx | Out-Null
Invoke-VmRun @auth createDirectoryInGuest $config.vmx 'C:\OpenA2dp' 2>$null

foreach ($file in (Get-ChildItem $package -File) + (Get-Item $cer)) {
	Write-Host "Copying $($file.Name)"
	Invoke-VmRun @auth CopyFileFromHostToGuest $config.vmx $file.FullName "C:\OpenA2dp\$($file.Name)"
}

Write-Host 'Installing certificate and driver package...'
$install = 'certutil -f -addstore Root C:\OpenA2dp\OpenA2dpTest.cer && ' +
	'certutil -f -addstore TrustedPublisher C:\OpenA2dp\OpenA2dpTest.cer && ' +
	'pnputil /add-driver C:\OpenA2dp\OpenA2dp.inf > C:\OpenA2dp\install.log 2>&1'
Invoke-VmRun @auth runProgramInGuest $config.vmx -activeWindow 'C:\Windows\System32\cmd.exe' "/c $install"
Invoke-VmRun @auth CopyFileFromGuestToHost $config.vmx 'C:\OpenA2dp\install.log' (Join-Path $root '.local\install.log')
Get-Content (Join-Path $root '.local\install.log')

Write-Host "`nNext (in the VM): Device Manager > headset A2DP device (BTHENUM\{0000110b-...}) > Update driver > Have Disk > C:\OpenA2dp\OpenA2dp.inf"
