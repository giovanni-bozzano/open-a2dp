# T12 - Disable/enable the device while streaming (S). docs/HARDWARE-TESTS.md, "Power and PnP".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T12' -Title 'Disable and enable the device node while streaming' -Level 'full' -Repeat 3 `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        Start-Audio
        Assert-Streaming -Codec 'SBC' | Out-Null
        $node = Get-OpenA2dpNode
        Assert-True ($node -ne $null) 'no OpenA2dp device node: is the sink connected and on this driver?'

        # The disable is bounded by Set-NodeState: a disable that does not complete is the hang
        # this test exists for, and it fails with the node's state rather than waiting.
        Set-NodeState -InstanceId $node.PNPDeviceID -Action disable -Expect 'Disabled' -TimeoutSec 60
        Assert-LogMatch -Pattern 'control device removed' -Describe 'the control device was removed' -Kernel

        Set-NodeState -InstanceId $node.PNPDeviceID -Action enable -Expect 'Started' -TimeoutSec 90
        $deadline = (Get-Date).AddSeconds(30)
        while ((Get-Date) -lt $deadline -and (Get-DriverServiceState) -ne 'RUNNING') { Start-Sleep -Milliseconds 500 }
        Assert-True ((Get-DriverServiceState) -eq 'RUNNING') "the driver service is $(Get-DriverServiceState) after the enable"

        Stop-Audio
        Start-Audio
        Assert-Streaming -Codec 'SBC' -TimeoutSec 45 | Out-Null
        Assert-PacketsAdvancing | Out-Null
    }
