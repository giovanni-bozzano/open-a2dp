# T29 - Driver-side latency report (S/A). docs/HARDWARE-TESTS.md, "Latency".
param(
    [string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive,
    [int]$SettleSeconds = 10
)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T29' -Title 'Latency, peak, lead, underruns and trims' -Level 'assisted' `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        Assert-CtlOk (Invoke-Ctl 'codec' 'sbc' '--preroll' '60') 'setting the 60 ms lead' | Out-Null
        Prompt-Operator 'Connect the sink and start playback to the OpenA2DP endpoint.'
        Start-Audio
        Assert-Streaming -Codec 'SBC' -TimeoutSec 45 | Out-Null
        # The peak and the loss counters accumulate across streams; this window is this test's.
        Assert-CtlOk (Invoke-Ctl 'reset-counters') 'starting the latency measurement from zero' | Out-Null

        Write-Step "watching the steady state for $SettleSeconds s"
        Start-Sleep -Seconds $SettleSeconds
        $status = Get-Status
        Assert-True ($status.LeadMs -eq 60) "the lead term reads $($status.LeadMs) ms, not 60"
        Assert-True ($status.BufferedMs -lt 30) "the rings hold $($status.BufferedMs) ms after $SettleSeconds s"
        Assert-True ($status.LatencyPeakMs -le ($status.LeadMs + 20 + 5)) `
            "latency peak $($status.LatencyPeakMs) ms is more than a packet above the $($status.LeadMs) ms lead"
        Assert-True ($status.Underruns -eq 0) "$($status.Underruns) underruns at the default lead"
        Assert-True ($status.TrimmedBytes -eq 0) "$($status.TrimmedBytes) B trimmed with no stall"
        $baseline = $status.LatencyMs

        Write-Step 'a 200 ms lead moves the total by about 140 ms'
        Assert-CtlOk (Invoke-Ctl 'codec' 'sbc' '--preroll' '200') 'setting the 200 ms lead' | Out-Null
        Stop-Audio
        Start-Audio
        $status = Wait-Status -TimeoutSec 20 -Describe 'the stream to run with the 200 ms lead' -Where { param($s) $s.LeadMs -eq 200 }
        $delta = $status.LatencyMs - $baseline
        Assert-True ([math]::Abs($delta - 140) -le 40) "the total moved by $delta ms, not about 140"

        Write-Step 'a 20 ms lead drops it by about 180 ms'
        Assert-CtlOk (Invoke-Ctl 'codec' 'sbc' '--preroll' '20') 'setting the 20 ms lead' | Out-Null
        Stop-Audio
        Start-Audio
        $status = Wait-Status -TimeoutSec 20 -Describe 'the stream to run with the 20 ms lead' -Where { param($s) $s.LeadMs -eq 20 }
        $delta = $status.LatencyMs - $baseline
        Write-Note "at a 20 ms lead the total is $($status.LatencyMs) ms and underruns are $($status.Underruns); T31 records the curve"

        Write-Step 'the same shape for AAC or aptX'
        $status = Get-Status
        $codec = if ($status.SinkCodecs -contains 'AAC') { 'aac' } elseif ($status.SinkCodecs -contains 'aptX') { 'aptx' } else { $null }
        if ($codec -and $status.Encoder -eq 'registered') {
            Assert-CtlOk (Invoke-Ctl 'codec' $codec '--preroll' '60') "setting a 60 ms lead for $codec" | Out-Null
            Stop-Audio
            Start-Audio
            $status = Wait-Status -TimeoutSec 30 -Describe "the $codec stream to run" -Where { param($s) $s.Media -eq 'Streaming' }
            Assert-True ($status.LeadMs -eq 60) "the $codec lead reads $($status.LeadMs) ms"
            Assert-True ($status.Underruns -eq 0) "$($status.Underruns) underruns on $codec at a 60 ms lead"
        } else {
            Write-Note 'no service codec is available here, so only SBC was measured'
        }

        Write-Step 'a paused endpoint asks for nothing and counts no underruns'
        Stop-Audio
        Wait-Status -TimeoutSec 20 -Describe 'the stream to suspend' -Where { param($s) $s.Media -ne 'Streaming' } | Out-Null
        $before = (Get-Status).Underruns
        Start-Sleep -Seconds 60
        $after = (Get-Status).Underruns
        Assert-True ($after -eq $before) "underruns moved from $before to $after while the endpoint was paused"
        Start-Audio
        Assert-Streaming -TimeoutSec 30 | Out-Null
        Start-Sleep -Seconds 3
        $later = (Get-Status).Underruns
        Assert-True ($later -eq $before) "underruns moved to $later after the resume"

        Assert-CtlOk (Invoke-Ctl 'reset') 'resetting the stored settings' | Out-Null
        Stop-Audio
    }
