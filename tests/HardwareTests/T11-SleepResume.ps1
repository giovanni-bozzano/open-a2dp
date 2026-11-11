# T11 - Sleep/resume (S). docs/HARDWARE-TESTS.md, "Power and PnP".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T11' -Title 'Sleep and wake, idle and streaming' -Level 'assisted' `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        Write-Step 'phase 1: sleep while connected and idle'
        Stop-Audio
        Wait-Status -TimeoutSec 20 -Describe 'the stream to stop before sleeping' -Where { param($s) $s.Media -ne 'Streaming' } | Out-Null
        Suspend-WithWake -WakeAfterSec 90

        Assert-Status -Describe 'the device still answers after the wake' -Where { param($s) $s.AVDTP } | Out-Null
        Assert-LogMatch -Pattern '\[svc\] opening' -Describe 'the service re-attached to the device after the wake' -Kind Service -TimeoutSec 20

        Start-Audio
        Assert-Streaming -Codec 'SBC' -TimeoutSec 45 | Out-Null

        Write-Step 'phase 2: sleep while streaming'
        Suspend-WithWake -WakeAfterSec 90
        Assert-Status -Describe 'the device still answers after the wake' -Where { param($s) $s.AVDTP } | Out-Null
        Assert-LogMatch -Pattern '\[svc\] opening' -Describe 'the service re-attached after the second wake' -Kind Service -TimeoutSec 20

        Stop-Audio
        Start-Audio
        Assert-Streaming -Codec 'SBC' -TimeoutSec 60 | Out-Null
        Assert-PacketsAdvancing | Out-Null
    }
