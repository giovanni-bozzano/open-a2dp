# T7 - Disconnect during media channel open (S). docs/HARDWARE-TESTS.md, "Teardown and recovery".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T7' -Title 'Disconnect during media channel open' -Level 'assisted' -Repeat 10 `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -AllowWarn @('still pending at close; abandoning it', 'closed a channel whose open finished after its waiter gave up') `
    -Body {
        Start-Audio
        # Open is the window between configuration and the first packet: the stream reaches it
        # only with audio running, and a person cannot hit it more precisely than this.
        Wait-Status -TimeoutSec 30 -Describe 'the session to reach Open' -Where { param($s) $s.AVDTP -in 'Open', 'Streaming' } | Out-Null
        Prompt-Operator 'Power the sink off now.'

        Wait-Status -TimeoutSec 30 -Describe 'the session to return to Idle after the disconnect' -Where { param($s) $s.AVDTP -eq 'Idle' } | Out-Null
        Assert-Teardown

        Prompt-Operator 'Power the sink back on.'
        Start-Audio
        Assert-Streaming -Codec 'SBC' -TimeoutSec 45 | Out-Null
    }
