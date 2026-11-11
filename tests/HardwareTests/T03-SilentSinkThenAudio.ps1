# T3 - Sink-initiated connect, silent sink, then audio (S). docs/HARDWARE-TESTS.md, "Connection".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T3' -Title 'Audio starts discovery on a silent sink link' -Level 'assisted' -Repeat 3 `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        Prompt-Operator 'Disconnect the sink in Windows Bluetooth settings, then power-cycle it so it reconnects by itself.'
        Assert-LogMatch -Pattern 'sink-initiated' -Describe 'the link was sink-initiated' -Kernel -TimeoutSec 30

        Start-Sleep -Seconds 3
        $status = Get-Status
        if ($status.Media -eq 'Streaming' -or $status.AVDTP -eq 'Open') {
            Skip-Test 'the sink configured the stream itself; T2 covers that path'
        }
        $discovery = @(Get-NewLog | Where-Object { $_ -match 'Discovering' })
        Assert-True ($discovery.Count -eq 0) 'discovery ran without audio: a timer still starts it'

        Start-Audio
        Assert-LogMatch -Pattern 'audio started on a silent sink-initiated link; starting discovery' `
            -Describe 'audio triggered discovery on the silent link' -Kernel -TimeoutSec 15
        Assert-Streaming -Codec 'SBC' | Out-Null
        Assert-PacketsAdvancing | Out-Null
        Confirm-Operator 'Is the tone audible from the sink?'
    }
