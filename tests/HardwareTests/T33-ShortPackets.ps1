# T33 - Short-packet profile (S). docs/HARDWARE-TESTS.md, "Latency".
param(
    [string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive,
    [int]$MinutesEach = 5
)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T33' -Title 'SBC frames per packet: 4, 8 and 1' -Level 'full' `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -AllowWarn @('underrun', 'dropped \d+ ms of buffered') `
    -Body {
        $status = Require-Codec 'sbc'
        Start-Audio
        $seconds = [Math]::Max(30, $MinutesEach * 60)

        Write-Step "4 frames per packet for $seconds s at 44.1 kHz"
        Assert-CtlOk (Invoke-Ctl 'codec' 'sbc' '--frames' '4') 'setting 4 frames per packet' | Out-Null
        Stop-Audio
        Start-Audio
        Assert-Streaming -Codec 'SBC' -TimeoutSec 45 | Out-Null
        Start-Sleep -Seconds $seconds
        $status = Get-Status
        Assert-True ($status.Underruns -eq 0) "$($status.Underruns) underruns with 4-frame packets"
        Assert-True ($status.TrimmedBytes -eq 0) "$($status.TrimmedBytes) B trimmed with 4-frame packets"
        Write-Note "4 frames: latency $($status.LatencyMs) ms, packets $($status.Packets)"

        Write-Step '8 frames per packet is the same as the default at these formats'
        Assert-CtlOk (Invoke-Ctl 'codec' 'sbc' '--frames' '8') 'setting 8 frames per packet' | Out-Null
        Stop-Audio
        Start-Audio
        Assert-Streaming -Codec 'SBC' -TimeoutSec 45 | Out-Null
        Start-Sleep -Seconds ([Math]::Max(30, $seconds / 5))
        $status = Get-Status
        Assert-True ($status.Underruns -eq 0) "$($status.Underruns) underruns with 8-frame packets"
        Write-Note "8 frames: latency $($status.LatencyMs) ms (the MTU decides above 7 frames)"

        Write-Step '1 frame per packet: record what the link carries'
        Assert-CtlOk (Invoke-Ctl 'codec' 'sbc' '--frames' '1') 'setting 1 frame per packet' | Out-Null
        Stop-Audio
        Start-Audio
        Start-Sleep -Seconds ([Math]::Max(30, $seconds / 5))
        $status = Get-Status
        # A 2.9 ms packet needs hundreds of sends a second: starving is the documented trade-off.
        Write-Note "1 frame: latency $($status.LatencyMs) ms, underruns $($status.Underruns), trimmed $($status.TrimmedBytes) B"

        Assert-CtlOk (Invoke-Ctl 'reset') 'resetting the stored settings' | Out-Null
        Stop-Audio
    }
