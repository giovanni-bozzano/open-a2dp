# T4 - Sink-initiated connect with audio started at once (S). docs/HARDWARE-TESTS.md, "Connection".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T4' -Title 'Sink-initiated connect with audio already running' -Level 'assisted' -Repeat 3 `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        Prompt-Operator 'Have a player ready with the OpenA2DP endpoint as output: power the sink on and start playback as soon as the endpoint appears.'
        Start-Audio

        Assert-LogMatch -Pattern 'sink-initiated with audio running; starting discovery|audio started on a silent sink-initiated link; starting discovery|incoming media channel open' `
            -Describe 'the connect resolved to one start-up path' -Kernel -TimeoutSec 30
        Assert-Streaming -Codec 'SBC' -TimeoutSec 30 | Out-Null
        Assert-PacketsAdvancing | Out-Null

        $bad = @(Get-NewLog | Where-Object { $_ -match 'SEP in use|no resources' })
        Assert-True ($bad.Count -eq 0) ("the log has resource errors:`n  " + ($bad -join "`n  "))
        $starts = @(Get-NewLog -Kind Service | Where-Object { $_ -match 'stream start' })
        Assert-True ($starts.Count -le 1) "the stream started $($starts.Count) times: it was configured twice"

        Confirm-Operator 'Is the tone audible from the sink?'
    }
