# T30 - Audible latency measured against the source (S/A). docs/HARDWARE-TESTS.md, "Latency".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T30' -Title 'Mouth-to-ear delay against the driver-side figure' -Level 'manual' `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Repeat 3 -Body {
        if ($script:Hw.NonInteractive) {
            Skip-Test 'the measurement needs a recorder and a person reading it; run without -NonInteractive'
        }
        Prompt-Operator 'Two-channel recorder ready: channel 1 a microphone at the sink, channel 2 the PC line loopback, wiring offset calibrated and subtracted.'
        Assert-CtlOk (Invoke-Ctl 'codec' 'sbc' '--preroll' '60') 'setting the 60 ms lead' | Out-Null
        Start-Audio
        $status = Assert-Streaming -Codec 'SBC' -TimeoutSec 45
        $driverSide = $status.LatencyMs

        Prompt-Operator 'Play the 30 s one-impulse-per-second file through the endpoint and record 40 s; measure the median onset offset over ~20 impulses.'
        $measured = Read-Number 'measured mouth-to-ear delay at the 60 ms lead (ms)' 0
        Assert-True ($measured -gt 0) 'no measurement was entered'
        Assert-True ($measured -ge ($driverSide - 5)) "measured $measured ms is below the driver-side $driverSide ms, which cannot be"

        Write-Step 'the spread across repetitions has to stay inside 20 ms'
        $samples = @($measured)
        for ($run = 2; $run -le 3; $run++) {
            Prompt-Operator "Run $run of 3 at the same lead: record and measure again."
            $samples += (Read-Number "measured delay, run $run (ms)" $measured)
        }
        $spread = ($samples | Measure-Object -Maximum).Maximum - ($samples | Measure-Object -Minimum).Minimum
        Assert-True ($spread -le 20) "the three measurements spread over $spread ms"

        Write-Step 'a 120 ms lead moves the ear by 60 ms, the sink term stays put'
        Assert-CtlOk (Invoke-Ctl 'codec' 'sbc' '--preroll' '120') 'setting the 120 ms lead' | Out-Null
        Stop-Audio
        Start-Audio
        Assert-Streaming -Codec 'SBC' -TimeoutSec 45 | Out-Null
        Prompt-Operator 'Repeat the measurement at the 120 ms lead.'
        $raised = Read-Number 'measured delay at the 120 ms lead (ms)' ($measured + 60)
        $rise = $raised - $measured
        Assert-True ([math]::Abs($rise - 60) -le 10) "the lead moved the measured delay by $rise ms, not 60 +/- 10"

        Write-Note "sink term: $([math]::Round($measured - $driverSide, 0)) ms at the 60 ms lead; record it with the sink model and rate"
        if ((Get-Status).SinkRates -contains 48000) {
            Write-Note 'repeat this measurement at 48 kHz: the driver-side figure scales, the sink term should not'
        }
        Assert-CtlOk (Invoke-Ctl 'reset') 'resetting the stored settings' | Out-Null
        Stop-Audio
    }
