# T2 - Sink-initiated connect, the sink configures (S). docs/HARDWARE-TESTS.md, "Connection".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T2' -Title 'Sink-initiated connect, silent for 10 s' -Level 'assisted' -Repeat 3 `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        Prompt-Operator 'Disconnect the sink in Windows Bluetooth settings, then power-cycle the sink so it reconnects by itself.'
        Assert-LogMatch -Pattern 'sink-initiated' -Describe 'the link was sink-initiated' -Kernel -TimeoutSec 30

        Write-Step 'staying silent for 10 s: no audio may start discovery'
        Start-Sleep -Seconds 10
        $status = Get-Status
        Assert-True ($status.Media -ne 'Streaming') "the stream started without audio (media $($status.Media))"

        if ($status.AVDTP -in 'Open', 'Streaming') {
            # The sink configured the stream itself: that path must not have discovered for it.
            Assert-LogMatch -Pattern 'incoming media channel open' -Describe 'the sink opened the media channel' -Kernel
        } else {
            Write-Note 'the sink stayed Idle in silence, so audio starts discovery'
        }

        Start-Audio
        Assert-Streaming -Codec 'SBC' | Out-Null
        Assert-PacketsAdvancing | Out-Null
        Confirm-Operator 'Is the tone audible from the sink?'
    }
