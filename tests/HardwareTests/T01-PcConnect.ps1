# T1 - PC-initiated connect (S). docs/HARDWARE-TESTS.md, "Connection".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T1' -Title 'PC-initiated connect, play, audible' -Level 'assisted' -Repeat 3 `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        Prompt-Operator 'Sink on and not connected (disconnect it first if an earlier run left it connected): click Connect in Windows Bluetooth settings.'
        Assert-LogMatch -Pattern 'signaling channel open' -Describe 'the signaling channel opened' -Kernel -TimeoutSec 30

        Write-Step 'playing audio to the OpenA2dp endpoint'
        Start-Audio
        Assert-Streaming -Codec 'SBC' | Out-Null

        Assert-LogMatch -Pattern 'Discovering' -Describe 'discovery ran before the stream' -Kernel
        Assert-LogMatch -Pattern 'media channel open' -Describe 'the media channel opened' -Kernel
        Assert-LogMatch -Pattern 'streaming, media MTU' -Describe 'the stream started with its media MTU' -Kernel
        Assert-PacketsAdvancing | Out-Null
        Confirm-Operator 'Is the tone audible from the sink?'
    }
