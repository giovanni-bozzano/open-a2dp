# T18 - Late registration renegotiates to AAC. docs/HARDWARE-TESTS.md, "AAC (sink A)".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T18' -Title 'Service starting mid-stream renegotiates to AAC' -Level 'assisted' `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        Write-Step 'phase 1: SBC first, the service arrives late'
        Require-Codec 'aac' | Out-Null
        Assert-CtlOk (Invoke-Ctl 'codec' 'auto') 'setting the codec preference to auto' | Out-Null
        Stop-EncoderService
        Prompt-Operator 'Connect the sink, then start playback to the OpenA2DP endpoint.'
        Start-Audio
        Assert-Streaming -Codec 'SBC' -TimeoutSec 45 | Out-Null

        Start-EncoderService
        Assert-LogMatch -Pattern 'stream is SBC; renegotiating if the sink offered AAC' -Describe 'the late registration was noticed' -Kernel -TimeoutSec 15
        Assert-LogMatch -Pattern 'SBC stream selected but the preference wants AAC; closing to renegotiate' -Describe 'the SBC stream closed to renegotiate' -Kernel
        Assert-LogMatch -Pattern 'stream closed; rediscovering to negotiate AAC' -Describe 'the driver rediscovered for AAC' -Kernel
        Assert-Streaming -Codec 'AAC' -TimeoutSec 45 | Out-Null
        Confirm-Operator 'Was the audio gap short, with AAC playing afterwards?'

        Write-Step 'phase 2: with SBC stored, a late registration must not renegotiate'
        Assert-CtlOk (Invoke-Ctl 'codec' 'sbc') 'storing the SBC preference' | Out-Null
        Stop-EncoderService
        Stop-Audio
        Start-Audio
        Assert-Streaming -Codec 'SBC' -TimeoutSec 45 | Out-Null
        Start-EncoderService
        Start-Sleep -Seconds 5
        $closing = @(Get-NewLog | Where-Object { $_ -match 'closing to renegotiate' })
        Assert-True ($closing.Count -eq 0) ('the stream renegotiated although SBC was pinned:' + "`n  " + ($closing -join "`n  "))
        Assert-Streaming -Codec 'SBC' -TimeoutSec 10 | Out-Null
        Assert-CtlOk (Invoke-Ctl 'codec' 'auto') 'restoring the automatic preference' | Out-Null
        Stop-Audio
    }
