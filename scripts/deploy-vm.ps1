<#
.SYNOPSIS
	Copies the test-signed OpenA2dp driver package into the test VM and installs it.

.DESCRIPTION
	Uses VMware vmrun (requires VMware Tools in the guest). Guest credentials are read
	from .local\vm.json (untracked):
		{ "vmx": "D:\\Virtual Machines\\OpenA2dp\\OpenA2dp.vmx", "user": "...", "password": "...", "vmPassword": "..." }
	vmPassword is the VM encryption password (VMware encrypts VMs that have a vTPM); omit it if the VM is not encrypted.

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
$base = @('-T', 'ws')
if ($config.vmPassword) { $base += @('-vp', $config.vmPassword) }
$auth = $base + @('-gu', $config.user, '-gp', $config.password)

function Invoke-VmRun {
	& $vmrun @args
	if ($LASTEXITCODE -ne 0) {
		throw "vmrun $($args | Where-Object { $_ -cmatch '^[a-zA-Z]{6,}$' } | Select-Object -First 1) failed ($LASTEXITCODE)"
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
	Invoke-VmRun @base revertToSnapshot $config.vmx $Snapshot
	Invoke-VmRun @base start $config.vmx
}

Write-Host 'Waiting for VMware Tools...'
& $vmrun @base checkToolsState $config.vmx | Out-Null
& $vmrun @auth createDirectoryInGuest $config.vmx 'C:\OpenA2dp' 2>&1 | Out-Null

foreach ($file in (Get-ChildItem $package -File) + (Get-Item $cer)) {
	Write-Host "Copying $($file.Name)"
	Invoke-VmRun @auth CopyFileFromHostToGuest $config.vmx $file.FullName "C:\OpenA2dp\$($file.Name)"
}

Write-Host 'Installing certificate and driver package...'
# vmrun mangles complex command lines, so run a batch file. It always exits 0;
# the log reports each step.
$cmd = Join-Path $root '.local\install.cmd'
Set-Content $cmd -Encoding ascii -Value @(
	'@echo off'
	'cd /d C:\OpenA2dp'
	'('
	'certutil -f -addstore Root OpenA2dpTest.cer'
	'certutil -f -addstore TrustedPublisher OpenA2dpTest.cer'
	'pnputil /add-driver OpenA2dp.inf /install'
	'echo pnputil exit=%errorlevel%'
	') > install.log 2>&1'
	'exit /b 0'
)
Invoke-VmRun @auth CopyFileFromHostToGuest $config.vmx $cmd 'C:\OpenA2dp\install.cmd'
Invoke-VmRun @auth runProgramInGuest $config.vmx 'C:\Windows\System32\cmd.exe' '/c C:\OpenA2dp\install.cmd'
Invoke-VmRun @auth CopyFileFromGuestToHost $config.vmx 'C:\OpenA2dp\install.log' (Join-Path $root '.local\install.log')
Get-Content (Join-Path $root '.local\install.log')

Write-Host "`npnputil /install binds the headset A2DP device; exit 3010 means reboot the VM."
