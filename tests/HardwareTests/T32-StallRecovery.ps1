# T32 - Stall recovery and the trims (S/A). docs/HARDWARE-TESTS.md, "Latency".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T32' -Title 'A transport stall: trim once, back to live' -Level 'assisted' `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -AllowWarn @('underrun', 'dropped \d+ ms of buffered', 'encoder lost while streaming') `
    -Body {
        Require-Codec 'sbc' | Out-Null
        Assert-CtlOk (Invoke-Ctl 'codec' 'sbc' '--preroll' '60') 'setting the 60 ms lead' | Out-Null
        Start-Audio
        Assert-Streaming -Codec 'SBC' -TimeoutSec 45 | Out-Null
        Start-Sleep -Seconds 30
        # The peak and the loss counters accumulate across streams; this window is this test's.
        Assert-CtlOk (Invoke-Ctl 'reset-counters') 'starting the stall measurement from zero' | Out-Null

        Write-Step 'attenuate the link for about half a second'
        Prompt-Operator 'Attenuate the link for about half a second (sink at the edge of range, or shielded by hand).'
        $status = Wait-Status -TimeoutSec 90 -Describe 'the stream to trim the backlog after the stall' `
            -Where { param($s) $s.TrimmedBytes -gt 0 }
        $trimmed = $status.TrimmedBytes
        Assert-True ($status.LatencyPeakMs -le ($status.LeadMs + 20 + 20)) `
            "latency peak $($status.LatencyPeakMs) ms is far above the $($status.LeadMs) ms lead after the stall"

        Write-Step 'the backlog returns to live audio, not to a permanent delay'
        $status = Wait-Status -TimeoutSec 15 -Describe 'the buffered audio to settle under 30 ms' -Where { param($s) $s.BufferedMs -lt 30 }
        Start-Sleep -Seconds 5
        $status = Get-Status
        Assert-True ($status.TrimmedBytes -eq $trimmed) "trimming continued after the stall ($trimmed -> $($status.TrimmedBytes) B)"
        Assert-True ($status.LatencyMs -le ($status.LeadMs + 40)) "the stream stayed $($status.LatencyMs) ms behind after the stall"

        Write-Step 'the same through a service restart'
        $status = Get-Status
        if ($status.SinkCodecs -contains 'AAC' -and $status.Encoder -eq 'registered') {
            Assert-CtlOk (Invoke-Ctl 'codec' 'aac') 'forcing AAC' | Out-Null
            Assert-Streaming -Codec 'AAC' -TimeoutSec 45 | Out-Null
            $before = (Get-Status).TrimmedBytes
            Stop-EncoderService
            Start-Sleep -Seconds 1
            Start-EncoderService
            Assert-LogMatch -Pattern 'underrun \(\d+ ms without a frame\)' -Describe 'the missing encoder showed as an underrun' -Kernel
            $after = (Get-Status).TrimmedBytes
            Assert-True ($after -gt $before) "the PCM backlog was not trimmed while the service was away ($before B)"
            Assert-Streaming -Codec 'AAC' -TimeoutSec 45 | Out-Null
            Write-Note 'a stop longer than the 2 s provider-loss grace falls back to SBC by design; keep this one short'
        } else {
            Write-Note 'no service codec is available here, so the service-stall case was not run'
        }
        Assert-CtlOk (Invoke-Ctl 'reset') 'resetting the stored settings' | Out-Null
        Stop-Audio
    }
