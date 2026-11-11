# T15 - The service starts after the driver (S). docs/HARDWARE-TESTS.md, "Encoder service".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T15' -Title 'Service started after the device exists' -Level 'full' -Repeat 3 `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        Assert-Status -Describe 'the device answers before the service starts' -Where { param($s) $s.AVDTP } | Out-Null
        Stop-EncoderService
        $status = Wait-Status -TimeoutSec 10 -Describe 'the encoder registration to be gone' -Where { param($s) $s.Encoder -eq 'not registered' }
        Assert-True ($status.Encoder -eq 'not registered') "the encoder is still $($status.Encoder) with the service stopped"

        $started = Get-Date
        Start-EncoderService
        $lines = Assert-LogMatch -Pattern '\[svc\] opening' -Describe 'the service opened the device on start' -Kind Service -TimeoutSec 10
        $attached = ((Get-Date) - $started).TotalSeconds
        Assert-True ($attached -le 5) "the service needed $([math]::Round($attached,1)) s to attach to the device"

        Wait-Status -TimeoutSec 10 -Describe 'the encoder to be registered' -Where { param($s) $s.Encoder -eq 'registered' } | Out-Null

        # An attach by interface arrival, not by polling: a periodic re-open would show up here.
        Start-Sleep -Seconds 10
        $reopens = @(Get-NewLog -Kind Service | Where-Object { $_ -match '\[svc\] opening' })
        Assert-True ($reopens.Count -le 2) "the service opened the device $($reopens.Count) times in 10 s: it is polling"
    }
