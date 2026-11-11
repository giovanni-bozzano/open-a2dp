# T10 - Out of range (S). docs/HARDWARE-TESTS.md, "Teardown and recovery".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T10' -Title 'Link drops out of range, then reconnects' -Level 'assisted' -Repeat 3 `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -AllowWarn @('still pending at close; abandoning it', 'closed a channel whose open finished after its waiter gave up') `
    -Body {
        Start-Audio
        Assert-Streaming -Codec 'SBC' | Out-Null
        Assert-PacketsAdvancing | Out-Null

        Prompt-Operator 'Walk away while the tone plays until the link drops, then come back.'
        Wait-Status -TimeoutSec 180 -Describe 'the session to end when the link drops' -Where { param($s) $s.AVDTP -eq 'Idle' } | Out-Null
        Assert-Teardown

        Prompt-Operator 'Reconnect the sink (from Windows Bluetooth settings, or power-cycle it).'
        Start-Audio
        Assert-Streaming -Codec 'SBC' -TimeoutSec 60 | Out-Null
        Assert-PacketsAdvancing | Out-Null
    }
