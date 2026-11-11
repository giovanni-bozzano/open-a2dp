# T17 - AAC with the service running before connect. docs/HARDWARE-TESTS.md, "AAC (sink A)".
param([string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T17' -Title 'AAC negotiated from the start' -Level 'assisted' -Repeat 3 `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -Body {
        $status = Require-Codec 'aac'
        Assert-True ($status.Encoder -eq 'registered') "no AAC encoder is registered (service $((Get-EncoderServiceState)))"

        Assert-CtlOk (Invoke-Ctl 'codec' 'auto') 'setting the codec preference to auto' | Out-Null
        Prompt-Operator 'Disconnect the sink and connect it again from Windows Bluetooth settings, then press Enter.'

        Start-Audio
        Assert-Streaming -Codec 'AAC' -TimeoutSec 45 | Out-Null
        Assert-LogMatch -Pattern 'sink offers codec AAC' -Describe 'the sink offered AAC' -Kernel
        Assert-LogMatch -Pattern 'configured codec AAC' -Describe 'AAC was configured' -Kernel
        Assert-LogMatch -Pattern '\[svc\] stream start' -Describe 'the service started a stream' -Kind Service
        Assert-PacketsAdvancing | Out-Null

        Start-Sleep -Seconds 5
        $status = Get-Status
        if ($null -ne $status.Underruns) {
            Assert-True ($status.Underruns -le 50) "$($status.Underruns) underruns in the first seconds of AAC playback"
        }
        Confirm-Operator 'Is the tone audible from the sink?'
    }
