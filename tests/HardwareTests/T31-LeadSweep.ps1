# T31 - Lowest safe lead for this sink (S/A). docs/HARDWARE-TESTS.md, "Latency".
param(
    [string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive,
    [int]$MinutesEach = 2,
    [int[]]$Leads = @(200, 120, 100, 80, 60, 40, 30, 20)
)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T31' -Title 'Lead sweep down to the first underrun' -Level 'assisted' `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -AllowWarn @('underrun', 'dropped \d+ ms of buffered') `
    -Body {
        Require-Codec 'sbc' | Out-Null
        Prompt-Operator 'Connect the sink and start playback to the OpenA2DP endpoint.'
        Start-Audio
        Assert-Streaming -Codec 'SBC' -TimeoutSec 45 | Out-Null

        $seconds = [Math]::Max(30, $MinutesEach * 60)
        $lowestSafe = $null
        foreach ($lead in $Leads) {
            Write-Step "lead ${lead} ms for $seconds s"
            Assert-CtlOk (Invoke-Ctl 'codec' 'sbc' '--preroll' "$lead") "setting the $lead ms lead" | Out-Null
            Stop-Audio
            Start-Audio
            $status = Wait-Status -TimeoutSec 20 -Describe "the stream to run with a $lead ms lead" -Where { param($s) $s.LeadMs -eq $lead }
            $start = $status.Underruns
            Start-Sleep -Seconds $seconds
            $end = (Get-Status).Underruns
            $growth = $end - $start
            Write-Note "lead ${lead} ms: $growth underruns over $seconds s, trimmed $((Get-Status).TrimmedBytes) B"
            if ($growth -eq 0) {
                $lowestSafe = $lead
            } else {
                Assert-LogMatch -Pattern 'underrun' -Describe "the underrun at the ${lead} ms lead is logged" -Kernel
                break
            }
        }
        Assert-True ($null -ne $lowestSafe) 'every lead in the sweep underran: the sweep did not start high enough'
        Write-Note "lowest lead with no growth: $lowestSafe ms; record it as this sink model's pacing lead"
        Confirm-Operator "Listen for 30 s at a $lowestSafe ms lead: is it clean?"

        Write-Step 'the same sweep with 4-frame packets, where the safe lead can differ'
        $frames4 = @($Leads | Where-Object { $_ -ge 40 }) | Select-Object -First 3
        foreach ($lead in $frames4) {
            Assert-CtlOk (Invoke-Ctl 'codec' 'sbc' '--frames' '4' '--preroll' "$lead") "setting 4 frames and a $lead ms lead" | Out-Null
            Stop-Audio
            Start-Audio
            $status = Wait-Status -TimeoutSec 20 -Describe 'the short-packet stream to run' -Where { param($s) $s.LeadMs -eq $lead }
            $start = $status.Underruns
            Start-Sleep -Seconds ([Math]::Max(30, $seconds / 4))
            Write-Note "frames 4, lead ${lead} ms: $((Get-Status).Underruns - $start) underruns"
        }
        Assert-CtlOk (Invoke-Ctl 'reset') 'resetting the stored settings' | Out-Null
        Stop-Audio
    }
