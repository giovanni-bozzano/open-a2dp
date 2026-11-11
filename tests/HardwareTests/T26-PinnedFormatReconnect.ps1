# T26 - Pinned format survives a reconnect (S/A). docs/HARDWARE-TESTS.md, "Handover checks".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T26' -Title 'Pinned format restored on reconnect' -Level 'assisted' -Repeat 3 `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        $status = Require-Codec 'sbc'
        $rate = if ($status.SinkRates -contains 48000) { 48000 } else { 44100 }
        Write-Step "pinning ${rate} Hz stereo"
        Assert-CtlOk (Invoke-Ctl 'format' "$rate" '2') 'pinning the endpoint format' | Out-Null
        Wait-Status -TimeoutSec 20 -Describe 'the endpoint to offer the pinned format' -Where { param($s) $s.PinnedRate -eq $rate } | Out-Null

        Prompt-Operator 'Power the sink off and on so it reconnects.'
        Assert-LogMatch -Pattern "Config: endpoint format $rate Hz 2 ch restored from the registry" `
            -Describe 'the pinned format was restored before Windows opened anything' -Kernel -TimeoutSec 60
        Assert-LogMatch -Pattern 'endpoint formats' -Describe 'the endpoint listed its formats at start' -Kernel

        Start-Audio
        $status = Assert-Streaming -TimeoutSec 60
        Assert-True ($status.Rate -eq $rate) "the stream runs at $($status.Rate) Hz, not the pinned $rate Hz"
        Confirm-Operator "Is the tone audible at $rate Hz without applying anything in the window?"
        Assert-CtlOk (Invoke-Ctl 'format' 'auto') 'making the endpoint automatic again' | Out-Null
        Stop-Audio
    }
