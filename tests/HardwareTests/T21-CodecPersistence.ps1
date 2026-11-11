# T21 - Codec preference persistence. docs/HARDWARE-TESTS.md, "AAC (sink A)".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T21' -Title 'Stored preference survives a device restart' -Level 'full' `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        $node = Get-OpenA2dpNode
        Assert-True ($node -ne $null) 'no OpenA2dp device node: is the sink connected and on this driver?'

        Write-Step 'store SBC and restart the device'
        Assert-CtlOk (Invoke-Ctl 'codec' 'sbc') 'storing the SBC preference' | Out-Null
        $values = Get-DeviceRegistryValues
        if ($values) {
            Assert-True ($values.CodecPreferenceFlags -ne 0) 'the forced-codec flag was not stored in the registry'
        }

        Set-NodeState -InstanceId $node.PNPDeviceID -Action disable -Expect 'Disabled' -TimeoutSec 60
        Set-NodeState -InstanceId $node.PNPDeviceID -Action enable -Expect 'Started' -TimeoutSec 90
        Assert-LogMatch -Pattern 'codec preference SBC restored from the registry' -Describe 'the stored SBC preference was restored' -Kernel -TimeoutSec 30

        Start-Audio
        Assert-Streaming -Codec 'SBC' -TimeoutSec 45 | Out-Null
        Confirm-Operator 'Is SBC audible without any further codec command?'

        Write-Step 'back to auto and the same restart'
        Assert-CtlOk (Invoke-Ctl 'codec' 'auto') 'restoring the automatic preference' | Out-Null
        Set-NodeState -InstanceId $node.PNPDeviceID -Action disable -Expect 'Disabled' -TimeoutSec 60
        Set-NodeState -InstanceId $node.PNPDeviceID -Action enable -Expect 'Started' -TimeoutSec 90
        Assert-LogMatch -Pattern 'codec preference auto restored from the registry' `
            -Describe 'the stored auto preference was restored' -Kernel -TimeoutSec 30

        Stop-Audio
        Start-Audio
        $status = Assert-Streaming -TimeoutSec 45
        $expected = if ($status.SinkCodecs -contains 'AAC') { @('AAC', 'SBC') } else { @('SBC') }
        Assert-True ($expected -contains $status.CodecName) "auto selected $($status.CodecName) with the sink offering $($status.SinkCodecs -join ', ')"
        Stop-Audio
    }
