# T24 - Config UI. docs/HARDWARE-TESTS.md, "AAC (sink A)".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

$window = @(
    'C:\OpenA2dp\OpenA2dpConfig.exe',
    (Join-Path $PSScriptRoot '..\..\out\x64\Debug\OpenA2dpConfig.exe'),
    (Join-Path $PSScriptRoot '..\..\out\x64\Release\OpenA2dpConfig.exe')
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1

Start-HwTest -Id 'T24' -Title 'Settings window over a live sink' -Level 'manual' `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        if ($script:Hw.NonInteractive) {
            Skip-Test 'the window checks need someone at the screen; run without -NonInteractive'
        }
        Assert-True ($window -ne $null) 'OpenA2dpConfig.exe not found; deploy first or pass a build'
        Start-Audio
        Assert-Streaming -TimeoutSec 45 | Out-Null
        Write-Step "starting $window"
        $process = Start-Process -FilePath $window -PassThru
        Start-Sleep -Seconds 2

        Confirm-Operator 'Both sinks are listed as "name (address) - connected/playing", and the playing one is selected, with the Session section matching `OpenA2dpCtl status`.'
        Confirm-Operator 'Choose "SBC only" and press Apply: the window reports "Stored. The active stream renegotiates to apply it.", and the status list follows within a few seconds without a sink reconnect.'
        Confirm-Operator 'Choose SBC bitpool 35 and apply: the status list and `OpenA2dpCtl status` report the resulting SBC bitrate.'
        Confirm-Operator 'Choose another PCM format and apply: one "jack unplugged" rebuild, the stream returns at the new format, nothing is rediscovered.'
        Confirm-Operator 'On a 1024x768 screen the whole window fits and the session panel scrolls to its last row.'
        Confirm-Operator 'Tick Details, press Copy, paste into an editor: the text matches `OpenA2dpCtl status --verbose` for the same sink.'
        Confirm-Operator 'Start and stop playback: the sink is relabelled "- playing"/"- suspended" within a second, with no Refresh.'
        Confirm-Operator 'Unplug the selected sink: the window reports it as gone instead of retrying.'

        Write-Step 'close the window'
        if (-not $process.HasExited) {
            $null = $process.CloseMainWindow()
            Start-Sleep -Seconds 2
            if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
        }
        Assert-CtlOk (Invoke-Ctl 'reset') 'resetting the stored settings' | Out-Null
        Stop-Audio
    }
