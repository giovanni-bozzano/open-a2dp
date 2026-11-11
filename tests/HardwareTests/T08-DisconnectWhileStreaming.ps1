# T8 - Disconnect while streaming (S). docs/HARDWARE-TESTS.md, "Teardown and recovery".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T8' -Title 'Disconnect while streaming' -Level 'assisted' -Repeat 10 `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -AllowWarn @('still pending at close; abandoning it', 'closed a channel whose open finished after its waiter gave up') `
    -Body {
        Start-Audio
        Assert-Streaming -Codec 'SBC' | Out-Null
        Assert-PacketsAdvancing | Out-Null

        Prompt-Operator 'Power the sink off now.'
        Wait-Status -TimeoutSec 30 -Describe 'the session to return to Idle after the disconnect' -Where { param($s) $s.AVDTP -eq 'Idle' } | Out-Null
        Assert-Teardown

        Prompt-Operator 'Power the sink back on.'
        Start-Audio
        Assert-Streaming -Codec 'SBC' -TimeoutSec 45 | Out-Null
    }
