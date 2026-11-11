# T6 - Disconnect during discovery (S). docs/HARDWARE-TESTS.md, "Teardown and recovery".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T6' -Title 'Disconnect during discovery' -Level 'assisted' -Repeat 10 `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -AllowWarn @('still pending at close; abandoning it', 'closed a channel whose open finished after its waiter gave up') `
    -Body {
        Prompt-Operator 'Connect the sink in Windows Bluetooth settings (disconnect it first if it is connected).'
        Wait-Status -TimeoutSec 30 -Describe 'the link to come up' -Where { param($s) $s.AVDTP -ne 'Idle' } | Out-Null
        Prompt-Operator 'Power the sink off now, while it is still discovering.'

        Wait-Status -TimeoutSec 30 -Describe 'the session to return to Idle after the disconnect' -Where { param($s) $s.AVDTP -eq 'Idle' } | Out-Null
        Assert-Teardown

        Write-Step 'reconnecting after the abort'
        Prompt-Operator 'Power the sink back on.'
        Start-Audio
        Assert-Streaming -Codec 'SBC' -TimeoutSec 45 | Out-Null
    }
