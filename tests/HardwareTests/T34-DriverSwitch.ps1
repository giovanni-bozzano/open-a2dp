# T34 - Per-sink driver switch (S/A). docs/HARDWARE-TESTS.md, "Driver switching".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T34' -Title 'Move one sink between BthA2dp and OpenA2dp' -Level 'assisted' `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        $address = Get-DeviceAddress
        Write-Step 'the node list names every A2DP sink and its driver'
        $listing = Assert-CtlOk (Invoke-Ctl 'driver') 'listing the A2DP nodes'
        Assert-Match $listing.Output ([regex]::Escape($address)) 'the sink under test is not in the node list'

        $current = Invoke-Ctl 'driver' '--device' $address
        Assert-CtlOk $current "reading the driver of $address"
        $onWindows = $current.Output -match 'BthA2dp'
        $target = if ($onWindows) { 'opena2dp' } else { 'windows' }
        $targetName = if ($target -eq 'opena2dp') { 'OpenA2dp' } else { 'BthA2dp' }

        if ($env:OPENA2DP_HWTEST_PLAYER) {
            Write-Step 'switching with a player streaming'
            Start-Audio
            $before = (Get-Status).AVDTP
            if ($before -ne 'Idle') {
                # The node cannot restart under an open stream: the tool pauses the holder and
                # names it, or refuses with exit 3 and changes nothing.
                $switch = Invoke-Ctl 'driver' $target
                if ($switch.ExitCode -eq 3) {
                    Skip-Test "the sink is held by a program that cannot be paused: $($switch.Text)"
                }
                Assert-CtlOk $switch "switching to $targetName with a player streaming"
                if ($switch.Output -notmatch 'paused .*(which was rendering to the sink)') {
                    Write-Note 'no player was paused: nothing was rendering to the sink at the time'
                }
            }
        }

        Write-Step "moving to $targetName"
        $switch = Assert-CtlOk (Invoke-Ctl 'driver' $target) "switching to $targetName"
        Assert-Match $switch.Output ([regex]::Escape($targetName) + ' installed on the sink''s A2DP node') 'the switch did not report the install'
        Start-Sleep -Seconds 3

        $state = Invoke-Ctl 'driver' '--device' $address
        Assert-CtlOk $state "reading the driver of $address after the switch"
        Assert-Match $state.Output $targetName "the sink is not on $targetName after the switch"
        $node = Get-OpenA2dpNode
        if ($target -eq 'windows') {
            Assert-True ($node -eq $null) 'an OpenA2dp node is still present on the Windows driver'
        } else {
            Assert-True ($node -ne $null) 'no OpenA2dp node came back after the switch'
        }

        if ($target -eq 'windows') {
            Write-Step 'the OpenA2dp control device is gone on the Windows driver'
            $status = Invoke-Ctl 'status'
            Assert-True ($status.ExitCode -ne 0) 'OpenA2dpCtl still reaches a control device on BthA2dp'
            $list = Invoke-Ctl 'list'
            Assert-Match $list.Output 'No OpenA2dp devices found' 'OpenA2dpCtl still lists a device on BthA2dp'
        }

        Write-Step 'and back again'
        $back = if ($onWindows) { 'windows' } else { 'opena2dp' }
        $backName = if ($back -eq 'windows') { 'BthA2dp' } else { 'OpenA2dp' }
        $switch = Assert-CtlOk (Invoke-Ctl 'driver' $back) "switching back to $backName"
        Assert-Match $switch.Output ([regex]::Escape($backName) + ' installed on the sink''s A2DP node') 'the way back did not report the install'
        Start-Sleep -Seconds 3

        $state = Invoke-Ctl 'driver' '--device' $address
        Assert-Match $state.Output $backName "the sink is not on $backName after the way back"

        if ($back -eq 'opena2dp') {
            Start-Audio
            Assert-Streaming -Codec 'SBC' -TimeoutSec 45 | Out-Null
            Assert-PacketsAdvancing | Out-Null
            Confirm-Operator 'Is the tone audible again after the way back?'
        }
        Write-Note 'the second sink, if any, keeps its driver and settings: compare the node list with the one above'
        Stop-Audio
    }
