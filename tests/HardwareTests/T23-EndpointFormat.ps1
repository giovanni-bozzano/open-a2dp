# T23 - Endpoint format selection. docs/HARDWARE-TESTS.md, "AAC (sink A)".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T23' -Title 'Endpoint format: automatic, pinned, restored and reset' -Level 'assisted' `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        $status = Require-Codec 'sbc'
        Assert-True ($status.EndpointFormat -match '^automatic') "the endpoint format starts pinned: '$($status.EndpointFormat)'"

        Start-Audio
        Assert-Streaming -Codec 'SBC' -TimeoutSec 45 | Out-Null

        if ($status.SinkRates -contains 48000) {
            Write-Step 'pin 48000 Hz 2 ch while streaming'
            Assert-CtlOk (Invoke-Ctl 'format' '48000' '2') 'pinning the endpoint format' | Out-Null
            Assert-LogMatch -Pattern 'jack unplugged for 80 ms so Windows rebuilds the endpoint' `
                -Describe 'the endpoint was rebuilt through a jack flap' -Kernel
            $status = Wait-Status -TimeoutSec 15 -Describe 'the stream to run at 48000 Hz 2 ch' `
                -Where { param($s) $s.Rate -eq 48000 -and $s.Channels -eq 2 }
            Assert-True ($status.PinnedRate -eq 48000) "status does not report the pinned format: '$($status.EndpointFormat)'"
            $closed = @(Get-NewLog | Where-Object { $_ -match 'rediscovering' })
            Assert-True ($closed.Count -eq 0) 'pinning the format rediscovered the stream'

            Write-Step 'the pin comes back before Windows opens anything'
            Prompt-Operator 'Power the sink off and on so it reconnects.'
            Assert-LogMatch -Pattern 'endpoint format 48000 Hz 2 ch restored from the registry' `
                -Describe 'the pinned format was restored at start' -Kernel -TimeoutSec 60
            Assert-Status -Describe 'the endpoint still offers the pinned format' -Where { param($s) $s.PinnedRate -eq 48000 } | Out-Null

            Write-Step 'mono pin'
            Assert-CtlOk (Invoke-Ctl 'format' '48000' '1') 'pinning 48000 Hz mono' | Out-Null
            $status = Wait-Status -TimeoutSec 20 -Describe 'the stream to run in mono' -Where { param($s) $s.Channels -eq 1 }
            Confirm-Operator 'Is the mono tone audible from the sink?'
        } else {
            Write-Note 'the sink does not advertise 48000 Hz, so the 48 kHz cases were not run'
            Write-Step '48 kHz on a 44.1-only sink is refused'
            $refused = Invoke-Ctl 'format' '48000' '2'
            Assert-True ($refused.ExitCode -ne 0) "pinning 48 kHz on a 44.1-only sink succeeded (exit 0)"
            Assert-Match $refused.Text 'does not advertise 48000 Hz 2 ch|it offered' 'the refusal does not name what the sink does offer'
        }

        Write-Step 'automatic restores every sink format'
        Assert-CtlOk (Invoke-Ctl 'format' 'auto') 'making the endpoint automatic again' | Out-Null
        Wait-Status -TimeoutSec 15 -Describe 'the endpoint to offer every sink format' -Where { param($s) $s.EndpointFormat -match '^automatic' } | Out-Null

        if ($status.SinkCodecs -notcontains 'AAC') {
            $refused = Invoke-Ctl 'codec' 'aac'
            Assert-True ($refused.ExitCode -ne 0) 'forcing AAC on an SBC-only sink succeeded'
            Assert-Match $refused.Text 'the sink does not advertise AAC \(it offers SBC\)' 'the refusal does not name the sink''s codecs'
        }

        Write-Step 'reset clears the stored preference and the format'
        $reset = Assert-CtlOk (Invoke-Ctl 'reset') 'resetting the stored settings'
        Assert-Match $reset.Output 'codec preference reset to auto' 'reset did not report the codec preference'
        Assert-Match $reset.Output 'endpoint format reset to automatic' 'reset did not report the endpoint format'
        Assert-Status -Describe 'the endpoint to be automatic after the reset' -Where { param($s) $s.EndpointFormat -match '^automatic' } | Out-Null
        Confirm-Operator 'Is the stream still audible after the reset?'
        Stop-Audio
    }
