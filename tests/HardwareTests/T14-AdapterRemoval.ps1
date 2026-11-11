# T14 - Bluetooth adapter removal (S). docs/HARDWARE-TESTS.md, "Power and PnP".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T14' -Title 'Adapter unplugged and reattached while streaming' -Level 'assisted' `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        Start-Audio
        Assert-Streaming -Codec 'SBC' | Out-Null
        Assert-PacketsAdvancing | Out-Null

        Prompt-Operator 'Unplug the USB Bluetooth adapter (in the VM: detach it from the virtual machine).'

        # The node goes with the adapter: its absence, not a status line, is the proof that the
        # removal reached the driver.
        $deadline = (Get-Date).AddSeconds(60)
        while ((Get-Date) -lt $deadline -and (Get-OpenA2dpNode) -ne $null) { Start-Sleep -Milliseconds 500 }
        Assert-True ((Get-OpenA2dpNode) -eq $null) 'the OpenA2dp node is still present after the adapter was removed'
        Assert-LogMatch -Pattern 'signaling channel closed|link down' -Describe 'the link closed on the removal' -Kernel
        # Nothing may hang: the machine itself has to stay responsive while the stack tears down.
        Assert-True ($null -ne (Get-Process -Id $PID -ErrorAction SilentlyContinue)) 'the test process did not survive the removal'

        Prompt-Operator 'Reattach the adapter.'
        $deadline = (Get-Date).AddSeconds(120)
        while ((Get-Date) -lt $deadline -and (Get-OpenA2dpNode) -eq $null) { Start-Sleep -Milliseconds 500 }
        Assert-True ((Get-OpenA2dpNode) -ne $null) 'the OpenA2dp node did not come back after the adapter was reattached'

        $deadline = (Get-Date).AddSeconds(60)
        while ((Get-Date) -lt $deadline -and (Get-DriverServiceState) -ne 'RUNNING') { Start-Sleep -Milliseconds 500 }
        Stop-Audio
        Start-Audio
        Assert-Streaming -Codec 'SBC' -TimeoutSec 60 | Out-Null
    }
