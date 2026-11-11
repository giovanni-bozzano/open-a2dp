# T19 - Live codec switch and SBC fallback. docs/HARDWARE-TESTS.md, "AAC (sink A)".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T19' -Title 'Live codec switch in one connection' -Level 'assisted' `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        Require-Codec 'aac' | Out-Null
        Assert-CtlOk (Invoke-Ctl 'codec' 'auto') 'setting the codec preference to auto' | Out-Null
        Prompt-Operator 'Disconnect the sink if it is still connected, connect it again, and start playback to the OpenA2dp endpoint.'
        Start-Audio
        Assert-Streaming -Codec 'AAC' -TimeoutSec 45 | Out-Null

        Write-Step 'AAC -> SBC while playing'
        Assert-CtlOk (Invoke-Ctl 'codec' 'sbc') 'forcing SBC mid-stream' | Out-Null
        Assert-LogMatch -Pattern 'AAC stream selected but the preference wants SBC; closing to renegotiate' `
            -Describe 'the AAC stream closed to renegotiate' -Kernel
        Assert-LogMatch -Pattern 'stream closed; rediscovering to negotiate SBC' -Describe 'the driver rediscovered for SBC' -Kernel
        Assert-Streaming -Codec 'SBC' -TimeoutSec 45 | Out-Null
        Assert-PacketsAdvancing | Out-Null
        Confirm-Operator 'Was the gap short, with SBC playing afterwards?'

        Write-Step 'SBC -> AAC while playing'
        Assert-CtlOk (Invoke-Ctl 'codec' 'aac') 'forcing AAC mid-stream' | Out-Null
        Assert-LogMatch -Pattern 'SBC stream selected but the preference wants AAC; closing to renegotiate' `
            -Describe 'the SBC stream closed to renegotiate' -Kernel
        Assert-Streaming -Codec 'AAC' -TimeoutSec 45 | Out-Null

        Write-Step 'SBC stays when AAC cannot be selected'
        Stop-EncoderService
        Assert-Streaming -Codec 'AAC' -TimeoutSec 10 | Out-Null
        $renegotiated = @(Get-NewLog | Where-Object { $_ -match 'closing to renegotiate' })
        # The stream was AAC and its encoder is gone: the driver has to fall back, not renegotiate.
        Wait-Status -TimeoutSec 20 -Describe 'the stream to fall back without a renegotiation' -Where { param($s) $s.Media -ne 'Streaming' -or $s.CodecName -eq 'SBC' } | Out-Null
        $after = @(Get-NewLog | Where-Object { $_ -match 'closing to renegotiate' })
        Assert-True ($after.Count -eq $renegotiated.Count) 'the driver tried to renegotiate for an encoder it does not have'
        Start-EncoderService
        Assert-CtlOk (Invoke-Ctl 'codec' 'auto') 'restoring the automatic preference' | Out-Null
        Stop-Audio
    }
