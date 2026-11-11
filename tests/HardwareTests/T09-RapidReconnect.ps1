# T9 - Rapid reconnect, media open retry (S). docs/HARDWARE-TESTS.md, "Teardown and recovery".
param(
    [string]$Device, [string]$Ctl, [string]$DriverLog, [string]$ResultDir, [switch]$NonInteractive,
    [int]$Cycles = 10,
    [int]$StreamWaitSec = 3
)
. (Join-Path $PSScriptRoot 'HwHarness.ps1')

Start-HwTest -Id 'T9' -Title 'Rapid reconnect cycles' -Level 'assisted' `
    -Device $Device -Ctl $Ctl -DriverLog $DriverLog -ResultDir $ResultDir -NonInteractive:$NonInteractive `
    -AllowWarn @('still pending at close; abandoning it', 'closed a channel whose open finished after its waiter gave up') `
    -Body {
        for ($cycle = 1; $cycle -le $Cycles; $cycle++) {
            Write-Step "cycle $cycle of ${Cycles}: connect"
            Prompt-Operator "Cycle ${cycle}: connect the sink in Windows Bluetooth settings."

            $streamed = $false
            $deadline = (Get-Date).AddSeconds($StreamWaitSec)
            while ((Get-Date) -lt $deadline) {
                $status = Get-Status
                if ($status.Media -eq 'Streaming') { $streamed = $true; break }
                Start-Sleep -Milliseconds 300
            }

            Prompt-Operator "Cycle ${cycle}: disconnect the sink in Windows Bluetooth settings."
            # Every cycle must end streaming or in a clean close, and the driver must answer at
            # once: that is the retry path failing and being cleaned up, not a hang.
            if (-not $streamed) {
                Wait-Status -TimeoutSec 15 -Describe "cycle $cycle to end in a clean close" `
                    -Where { param($s) $s.AVDTP -eq 'Idle' } | Out-Null
                Assert-LogMatch -Pattern 'stream aborted; rediscovering|media channel open failed after 3 attempts|signaling channel closed' `
                    -Describe "cycle $cycle ended through the retry path" -Kernel
            } else {
                Wait-Status -TimeoutSec 15 -Describe "cycle $cycle to close" -Where { param($s) $s.AVDTP -eq 'Idle' } | Out-Null
            }
            Assert-Responsive | Out-Null
        }

        $retries = @(Get-NewLog | Where-Object { $_ -match 'media channel open attempt \d+ failed' })
        if ($retries.Count -gt 0) {
            Write-Note "$($retries.Count) retry line(s); the aborted streams were rediscovered"
            Assert-LogMatch -Pattern 'stream aborted; rediscovering|media channel open failed after 3 attempts' `
                -Describe 'each failed media open led to a rediscovery or a clean close' -Kernel
        }
    }
