# T13 - Unload leak check (S). docs/HARDWARE-TESTS.md, "Power and PnP".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T13' -Title 'Driver unloads with no leaked pool or handles' -Level 'assisted' `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        Write-Step 'a stream, then an unload: the cycles that have to leave nothing behind'
        Start-Audio
        Assert-Streaming -Codec 'SBC' | Out-Null
        Assert-PacketsAdvancing | Out-Null
        Stop-Audio
        Wait-Status -TimeoutSec 20 -Describe 'the stream to stop' -Where { param($s) $s.Media -ne 'Streaming' } | Out-Null

        $node = Get-OpenA2dpNode
        Assert-True ($node -ne $null) 'no OpenA2dp device node: is the sink connected and on this driver?'
        Set-NodeState -InstanceId $node.PNPDeviceID -Action disable -Expect 'Disabled' -TimeoutSec 60

        $deadline = (Get-Date).AddSeconds(30)
        while ((Get-Date) -lt $deadline -and (Get-DriverServiceState) -notin 'STOPPED', 'NOT_INSTALLED') { Start-Sleep -Milliseconds 500 }
        $state = Get-DriverServiceState
        Assert-True ($state -in 'STOPPED', 'NOT_INSTALLED') "the driver did not unload (service state $state) with the device disabled"
        Assert-LogMatch -Pattern 'Unload' -Describe 'the driver reported its unload' -Kernel

        Confirm-Operator 'poolmon shows no OAdp allocation, and !wdfdriverinfo OpenA2dp lists no leaked handles with TrackHandles.'

        Set-NodeState -InstanceId $node.PNPDeviceID -Action enable -Expect 'Started' -TimeoutSec 90
        Start-Audio
        Assert-Streaming -Codec 'SBC' -TimeoutSec 45 | Out-Null
    }
