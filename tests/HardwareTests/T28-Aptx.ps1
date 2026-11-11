# T28 - Classic aptX negotiation and the 16-bit path (S/A). docs/HARDWARE-TESTS.md, "Handover checks".
param(
    [string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive,
    [int]$SettleSeconds = 60
)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T28' -Title 'Classic aptX end to end' -Level 'assisted' -Repeat 3 `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        $status = Require-Codec 'aptx'
        $both = $status.SinkCodecs -contains 'aptX HD'
        Assert-True ($status.Encoder -eq 'registered') 'no aptX encoder is registered (is OpenA2dpSvc running?)'
        Assert-CtlOk (Invoke-Ctl 'codec' 'auto') 'setting the codec preference to auto' | Out-Null

        Prompt-Operator 'Disconnect the sink if it is still connected, connect it again, and start playback to the OpenA2dp endpoint.'
        Start-Audio
        $status = Assert-Streaming -TimeoutSec 45
        if ($both) {
            # auto prefers HD, so classic aptX needs its own request before it can be checked.
            Assert-True ($status.CodecName -eq 'aptX HD') "auto selected $($status.CodecName) although the sink offers aptX HD"
            Write-Step 'forcing classic aptX on a sink that offers both'
            Assert-CtlOk (Invoke-Ctl 'codec' 'aptx') 'forcing classic aptX' | Out-Null
            $status = Assert-Streaming -Codec 'aptX' -TimeoutSec 45
        } else {
            Assert-True ($status.CodecName -eq 'aptX') "auto selected $($status.CodecName), not aptX"
        }

        Assert-LogMatch -Pattern 'sink offers aptX' -Describe 'the sink offered aptX' -Kernel
        Assert-LogMatch -Pattern 'configured codec aptX IE 4F 00 00 00' -Describe 'aptX was configured with the vendor IE' -Kernel
        Assert-LogMatch -Pattern 'aptX \d+ Hz stereo 16-bit' -Describe 'the encoder received 16-bit frames' -Kernel
        Assert-True ($status.Rate -in 44100, 48000) "the stream runs at $($status.Rate) Hz"
        $expected = if ($status.Rate -eq 44100) { 352800 } else { 384000 }
        Assert-True ($status.Bitrate -eq $expected) "aptX runs at $($status.Bitrate) bps, not $expected"

        Write-Step "playing for $SettleSeconds s and checking the counters"
        Start-Sleep -Seconds $SettleSeconds
        $status = Get-Status
        Assert-True ($status.SendFailures -eq 0) "$($status.SendFailures) send failures after $SettleSeconds s"
        Assert-True ($status.Underruns -eq 0) "$($status.Underruns) underruns after $SettleSeconds s"

        if ($status.Rate -eq 44100) {
            Confirm-Operator 'After several minutes of 44.1 kHz playback, is the sink still in sync with the PC?'
        }
        Confirm-Operator 'Is the aptX stream audible from the sink?'
        Write-Note 'forcing aptX on a sink that does not advertise it needs a second sink; not run here'
        Assert-CtlOk (Invoke-Ctl 'codec' 'auto') 'restoring the automatic preference' | Out-Null
        Stop-Audio
    }
