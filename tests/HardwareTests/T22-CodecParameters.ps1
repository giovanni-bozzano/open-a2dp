# T22 - Codec parameters. docs/HARDWARE-TESTS.md, "AAC (sink A)".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T22' -Title 'SBC bitpool and frame parameters, AAC bitrate' -Level 'full' `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        Require-Codec 'sbc' | Out-Null
        Assert-CtlOk (Invoke-Ctl 'codec' 'sbc') 'storing the SBC preference' | Out-Null
        Start-Audio
        Assert-Streaming -Codec 'SBC' -TimeoutSec 45 | Out-Null

        Write-Step 'SBC bitpool 35 in place'
        Assert-CtlOk (Invoke-Ctl 'codec' 'sbc' '--bitpool' '35') 'setting the SBC bitpool' | Out-Null
        Assert-LogMatch -Pattern 'SBC parameters changed; suspending to reconfigure' -Describe 'the stream suspended to reconfigure' -Kernel
        Assert-LogMatch -Pattern 'SBC parameters changed; sending RECONFIGURE' -Describe 'the driver sent RECONFIGURE' -Kernel
        $status = Assert-Streaming -Codec 'SBC' -TimeoutSec 30
        Assert-True ($status.Fields['SBC bitpool'] -eq '35') "status reports SBC bitpool '$($status.Fields['SBC bitpool'])', not 35"
        $closed = @(Get-NewLog | Where-Object { $_ -match 'stream closed; rediscovering' })
        Assert-True ($closed.Count -eq 0) ('the bitpool change closed the stream instead of reconfiguring:' + "`n  " + ($closed -join "`n  "))

        Write-Step 'SBC frame shape: 8 blocks, 4 subbands, SNR'
        Assert-CtlOk (Invoke-Ctl 'codec' 'sbc' '--blocks' '8' '--subbands' '4' '--allocation' 'snr') 'setting the SBC frame shape' | Out-Null
        Wait-Status -TimeoutSec 30 -Describe 'the SBC frame shape to be applied' -Where { param($s) $s.Fields['SBC frame'] -match '8 blocks, 4 subbands, SNR' } | Out-Null

        Write-Step 'driver defaults are restored by zero'
        Assert-CtlOk (Invoke-Ctl 'codec' 'auto' '--bitpool' '0' '--blocks' '0' '--subbands' '0' '--allocation' 'default') 'restoring the SBC defaults' | Out-Null
        $status = Get-Status
        Assert-True (-not $status.Fields['SBC bitpool']) 'the bitpool is still stored after it was reset to the default'
        Assert-True (-not $status.Fields['SBC frame']) 'the frame shape is still stored after it was reset to the default'

        if ($status.SinkCodecs -contains 'AAC' -and $status.Encoder -eq 'registered') {
            Write-Step 'AAC bitrate cap and the media-MTU ceiling'
            Assert-CtlOk (Invoke-Ctl 'codec' 'aac') 'forcing AAC' | Out-Null
            $status = Assert-Streaming -Codec 'AAC' -TimeoutSec 45
            Assert-CtlOk (Invoke-Ctl 'codec' 'aac' '--bitrate' '128000') 'setting the AAC bitrate' | Out-Null
            $status = Wait-Status -TimeoutSec 30 -Describe 'the AAC bitrate to become 128000' -Where { param($s) $s.Bitrate -and ($s.Bitrate -le 128000) }
            Assert-True ($status.Bitrate -gt 0) 'status reports no AAC bitrate after the RECONFIGURE'
            Assert-Match $status.MtuText 'AAC up to (256000|320000) bps' 'the media MTU line does not report the bitrate ceiling'

            if ($status.Rate -eq 48000) {
                Assert-CtlOk (Invoke-Ctl 'codec' 'aac' '--bitrate' '320000') 'setting 320 kbps at 48 kHz' | Out-Null
                Start-Sleep -Seconds 3
                $fits = @(Get-NewLog | Where-Object { $_ -match 'does not fit the media MTU' })
                Assert-True ($fits.Count -eq 0) 'a 320 kbps 48 kHz stream was refused by the MTU'
            } else {
                Write-Note "the stream runs at $($status.Rate) Hz, so the 48 kHz ceiling case does not apply here"
            }
        } else {
            Write-Note 'AAC is not offered with an encoder here, so the AAC bitrate cases were not run'
        }

        Assert-CtlOk (Invoke-Ctl 'reset') 'resetting the stored settings' | Out-Null
        Stop-Audio
    }
