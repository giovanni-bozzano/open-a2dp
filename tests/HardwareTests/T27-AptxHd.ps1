# T27 - aptX HD negotiation and the 24-bit path (S/A). docs/HARDWARE-TESTS.md, "Handover checks".
param(
    [string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive,
    [int]$SettleSeconds = 60
)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T27' -Title 'aptX HD end to end and back to SBC' -Level 'assisted' -Repeat 3 `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        Require-Codec 'aptxhd' | Out-Null
        Assert-True ((Get-Status).Encoder -eq 'registered') 'no aptX HD encoder is registered (is OpenA2dpSvc running?)'
        Assert-CtlOk (Invoke-Ctl 'codec' 'auto') 'setting the codec preference to auto' | Out-Null

        Prompt-Operator 'Disconnect the sink if it is still connected, connect it again, and start playback to the OpenA2DP endpoint.'
        Start-Audio
        $status = Assert-Streaming -Codec 'aptX HD' -TimeoutSec 45

        Assert-LogMatch -Pattern 'sink offers aptX HD' -Describe 'the sink offered aptX HD' -Kernel
        Assert-LogMatch -Pattern 'endpoint formats.*24-bit' -Describe 'the endpoint listed a 24-bit format' -Kernel
        Assert-LogMatch -Pattern 'configured codec aptX HD IE D7 00 00 00' -Describe 'aptX HD was configured with the vendor IE' -Kernel
        Assert-LogMatch -Pattern 'aptX HD \d+ Hz stereo 24-bit' -Describe 'the encoder received 24-bit frames from the service' -Kernel

        Write-Step "playing for $SettleSeconds s and checking the counters"
        Start-Sleep -Seconds $SettleSeconds
        $status = Get-Status
        Assert-True ($status.SendFailures -eq 0) "$($status.SendFailures) send failures after $SettleSeconds s"
        Assert-True ($status.Underruns -eq 0) "$($status.Underruns) underruns after $SettleSeconds s"
        Confirm-Operator 'Is the aptX HD stream audible from the sink?'

        Write-Step 'live switch to SBC over the same connection'
        Assert-CtlOk (Invoke-Ctl 'codec' 'sbc') 'forcing SBC mid-stream' | Out-Null
        Assert-Streaming -Codec 'SBC' -TimeoutSec 45 | Out-Null
        Assert-PacketsAdvancing | Out-Null
        Confirm-Operator 'Did playback continue through the 24-bit to 16-bit change, without a silence and without a reconnect?'
        Write-Note 'forcing aptX HD on a sink that does not advertise it needs a second sink; not run here'
        Assert-CtlOk (Invoke-Ctl 'codec' 'auto') 'restoring the automatic preference' | Out-Null
        Stop-Audio
    }
