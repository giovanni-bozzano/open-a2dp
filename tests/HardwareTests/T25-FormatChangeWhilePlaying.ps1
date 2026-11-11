# T25 - Player resumes after the endpoint flap (S/A). docs/HARDWARE-TESTS.md, "Handover checks".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T25' -Title 'Format change under a real player' -Level 'assisted' -Repeat 3 `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        # A tone does not reopen its stream after a rebuild; the point of the test is the
        # player that does, so it runs only with a real one.
        if (-not $env:OPENA2DP_HWTEST_PLAYER) {
            Skip-Test 'set OPENA2DP_HWTEST_PLAYER to a real player so playback can survive the flap'
        }
        $status = Require-Codec 'sbc'
        Start-Audio
        Assert-Streaming -TimeoutSec 45 | Out-Null

        $rate = if ($status.SinkRates -contains 48000) { 48000 } else { 44100 }
        $channels = if ($status.SinkChannels) { 2 } else { 2 }
        Write-Step "pinning ${rate} Hz to rebuild the endpoint under the player"
        Assert-CtlOk (Invoke-Ctl 'format' "$rate" "$channels") 'pinning the endpoint format' | Out-Null
        Assert-LogMatch -Pattern 'jack unplugged for 80 ms so Windows rebuilds the endpoint' `
            -Describe 'the endpoint was rebuilt' -Kernel
        $status = Wait-Status -TimeoutSec 20 -Describe "the stream to run at $rate Hz" -Where { param($s) $s.Rate -eq $rate }
        $closed = @(Get-NewLog | Where-Object { $_ -match 'rediscovering' })
        Assert-True ($closed.Count -eq 0) 'the format change rediscovered the stream'

        Confirm-Operator "The player kept playing and the sink is audible at $rate Hz."
        Assert-CtlOk (Invoke-Ctl 'format' 'auto') 'making the endpoint automatic again' | Out-Null
        Stop-Audio
    }
