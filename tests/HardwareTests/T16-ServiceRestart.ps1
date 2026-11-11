# T16 - Service stop/restart while streaming. docs/HARDWARE-TESTS.md, "Encoder service".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T16' -Title 'Stop and restart the service under a live stream' -Level 'full' `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        Write-Step 'phase 1: SBC keeps playing while the service stops and starts'
        Start-Audio
        Assert-Streaming -Codec 'SBC' | Out-Null
        Stop-EncoderService
        Assert-Streaming -Codec 'SBC' -TimeoutSec 10 | Out-Null
        Assert-PacketsAdvancing | Out-Null
        $renotiate = @(Get-NewLog | Where-Object { $_ -match 'preference wants AAC' })
        Assert-True ($renotiate.Count -eq 0) 'SBC renegotiated for a codec the sink does not offer'
        Start-EncoderService
        Wait-Status -TimeoutSec 15 -Describe 'the encoder to register again' -Where { param($s) $s.Encoder -eq 'registered' } | Out-Null

        Write-Step 'phase 2: AAC suspends when its encoder disappears'
        Require-Codec 'aac' | Out-Null
        Assert-CtlOk (Invoke-Ctl 'codec' 'aac') 'forcing AAC' | Out-Null
        Assert-Streaming -Codec 'AAC' -TimeoutSec 45 | Out-Null
        Stop-EncoderService
        Assert-LogMatch -Pattern 'encoder lost while streaming AAC; suspending' -Describe 'the driver suspended the AAC stream' -Kernel -TimeoutSec 15
        Assert-Responsive | Out-Null
        Start-EncoderService
        Stop-Audio
    }
