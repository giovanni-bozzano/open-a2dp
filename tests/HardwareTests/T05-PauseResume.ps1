# T5 - Pause/resume and output switching (S). docs/HARDWARE-TESTS.md, "Connection".
param(
    [string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive,
    [int]$PauseSeconds = 30
)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T5' -Title 'Pause/resume and default-output switching' -Level 'assisted' `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        Start-Audio
        Assert-Streaming -Codec 'SBC' | Out-Null

        Write-Step "pausing for $PauseSeconds s"
        Stop-Audio
        Wait-Status -TimeoutSec 20 -Describe 'the stream to suspend after the pause' -Where { param($s) $s.Media -eq 'Suspended' } | Out-Null
        Assert-LogMatch -Pattern 'stream paused' -Describe 'the driver paused the stream' -Kernel
        Start-Sleep -Seconds $PauseSeconds

        Start-Audio
        Wait-Status -TimeoutSec 30 -Describe 'the stream to run again after the resume' -Where { param($s) $s.Media -eq 'Streaming' } | Out-Null
        Assert-LogMatch -Pattern 'stream run' -Describe 'the driver ran the stream again' -Kernel

        Prompt-Operator 'Switch the default output to the speakers and back to the OpenA2dp endpoint.'
        Wait-Status -TimeoutSec 30 -Describe 'the stream to run on the sink after the switch back' -Where { param($s) $s.Media -eq 'Streaming' } | Out-Null
        Assert-PacketsAdvancing | Out-Null
        Confirm-Operator 'Is the tone audible again from the sink?'
    }
