# T20 - Long run (soak). docs/HARDWARE-TESTS.md, "Soak".
param(
    [string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive,
    [double]$Hours = 8,
    [int]$SampleSeconds = 60
)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T20' -Title 'Long run (soak)' -Level 'full' `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        Require-Codec 'sbc' | Out-Null
        Assert-CtlOk (Invoke-Ctl 'codec' 'auto') 'setting the codec preference to auto' | Out-Null
        Start-Audio
        $status = Assert-Streaming -TimeoutSec 45
        Write-Step "streaming $($status.CodecName); sampling every $SampleSeconds s for $Hours h"

        $sendFailures = $status.SendFailures
        $memory = (Get-Process -Name OpenA2dpSvc -ErrorAction SilentlyContinue | Select-Object -First 1).WorkingSet64
        $deadline = (Get-Date).AddHours($Hours)
        $samples = 0
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Seconds $SampleSeconds
            $samples++
            $service = Get-Process -Name OpenA2dpSvc -ErrorAction SilentlyContinue | Select-Object -First 1
            Assert-True ($null -ne $service) 'the encoder service is gone: it crashed during the soak'
            $status = Get-Status
            Assert-True ($status.Media -eq 'Streaming') "the stream left Streaming at sample $samples (media $($status.Media))"
            Assert-True ($null -ne $status.SendFailures -and ($status.SendFailures - $sendFailures) -le 5) `
                "send failures grew from $sendFailures to $($status.SendFailures) by sample $samples"
            if ($memory -gt 0) {
                $growth = ($service.WorkingSet64 - $memory) / $memory
                Assert-True ($growth -lt 0.5) "the service working set grew $([math]::Round($growth * 100,0))% since the start"
            }
            if ($samples % 10 -eq 0) {
                Write-Note "sample ${samples}: packets $($status.Packets), dropped $($status.Dropped), underruns $($status.Underruns), send failures $($status.SendFailures)"
            }
        }
        Write-Note "$samples samples taken"
        Stop-Audio
    }
