# Shared half of the hardware tests: one device, one log mark, the assertions, and the result
# line. Each T*.ps1 is one procedure from docs/HARDWARE-TESTS.md and only says what that
# procedure does; everything a test repeats lives here.
#
# The script is dot-sourced, so its functions run in the test script's own scope, and
# Start-HwTest can invoke the test body there.

$ErrorActionPreference = 'Stop'

# The §4 log check of the procedure: anything a test may not explain away.
$script:HwLogAuditPattern = 'WARN|ERROR|stop: still waiting|BRB did not complete|send did not complete|read still pending|no response within'

function Write-Step {
    param([Parameter(Mandatory)][string]$Text)
    Write-Host ("[{0}] {1}" -f $script:Hw.Id, $Text) -ForegroundColor Cyan
}

function Write-Note {
    param([Parameter(Mandatory)][string]$Text)
    Write-Host ("[{0}]   {1}" -f $script:Hw.Id, $Text) -ForegroundColor DarkGray
    $script:Hw.Notes.Add($Text) | Out-Null
}

function Fail-Hw {
    # An assertion that did not hold: the test cannot continue meaningfully.
    param([Parameter(Mandatory)][string]$Message)
    throw [System.Exception]::new("ASSERT: $Message")
}

function Skip-Test {
    # A precondition of the procedure is not met (the sink does not offer the codec, the
    # machine has no encoder service): not a failure of the driver.
    param([Parameter(Mandatory)][string]$Reason)
    throw [System.Exception]::new("SKIP: $Reason")
}

# ---------- context ----------

function Resolve-Ctl {
    param([string]$Path)
    $candidates = @()
    if ($Path) { $candidates += $Path }
    if ($env:OPENA2DP_CTL) { $candidates += $env:OPENA2DP_CTL }
    $candidates += (Join-Path 'C:\OpenA2dp' 'OpenA2dpCtl.exe')
    $candidates += (Join-Path $PSScriptRoot '..\..\out\x64\Debug\OpenA2dpCtl.exe')
    $candidates += (Join-Path $PSScriptRoot '..\..\out\x64\Release\OpenA2dpCtl.exe')
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    throw "OpenA2dpCtl.exe not found. Deploy first (scripts\deploy.ps1), or pass -Ctl / set OPENA2DP_CTL."
}

function Test-Elevated {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# ---------- the CLI ----------

function Invoke-Ctl {
    <#
        Runs OpenA2dpCtl and returns its exit code, its output and the error stream. Every
        call names the device when the caller was given one, so a machine with two sinks
        never picks the wrong one.
    #>
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)
    $argumentList = @()
    if ($Arguments) { $argumentList += $Arguments }
    if ($script:Hw.Device -and ($Arguments -notcontains '--device')) {
        $argumentList += @('--device', $script:Hw.Device)
    }
    $stdout = [IO.Path]::GetTempFileName()
    $stderr = [IO.Path]::GetTempFileName()
    $started = Get-Date
    try {
        $process = Start-Process -FilePath $script:Hw.Ctl -ArgumentList (($argumentList | ForEach-Object {
            if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
        }) -join ' ') -NoNewWindow -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        $null = $process.Handle
        $output = ''
        # A ctl call that blocks is the hang this suite exists to catch: bounded, never infinite.
        if (-not $process.WaitForExit(($script:Hw.CtlTimeoutSec) * 1000)) {
            & taskkill.exe /T /F /PID $process.Id 2>&1 | Out-Null
            throw "OpenA2dpCtl $($argumentList -join ' ') did not finish within $($script:Hw.CtlTimeoutSec) s (the driver is not answering)."
        }
        $process.WaitForExit()
        $output = (Get-Content -LiteralPath $stdout -Raw -ErrorAction SilentlyContinue)
        $errorText = (Get-Content -LiteralPath $stderr -Raw -ErrorAction SilentlyContinue)
        if ($null -eq $output) { $output = '' }
        if ($null -eq $errorText) { $errorText = '' }
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Output   = $output.TrimEnd()
            Error    = $errorText.TrimEnd()
            Text     = ($output.TrimEnd() + "`n" + $errorText.TrimEnd()).Trim()
            Seconds  = ((Get-Date) - $started).TotalSeconds
            Arguments = ($argumentList -join ' ')
        }
    } finally {
        Remove-Item -LiteralPath $stdout, $stderr -ErrorAction SilentlyContinue
    }
}

function Assert-CtlOk {
    param([Parameter(Mandatory)]$Result, [string]$Because)
    if ($Result.ExitCode -ne 0) {
        $why = if ($Because) { $Because } else { "OpenA2dpCtl $($Result.Arguments)" }
        Fail-Hw "$why failed (exit $($Result.ExitCode)): $($Result.Text)"
    }
    return $Result
}

# ---------- the status report ----------

function ConvertFrom-StatusReport {
    # The report is one "  <label padded to 16>  <value>" line per field, which makes it
    # readable in a console and parseable here without a second format.
    param([string[]]$Lines)
    $fields = @{}
    foreach ($line in $Lines) {
        if (-not $line -or $line.Length -lt 19) { continue }
        if (-not $line.StartsWith('  ')) { continue }
        if ($line.TrimStart().StartsWith('--')) { continue }
        $label = $line.Substring(2, 16).Trim()
        if (-not $label) { continue }
        $fields[$label] = $line.Substring(18).Trim()
    }

    $report = [ordered]@{
        Lines        = $Lines
        Fields       = $fields
        AVDTP        = $fields['AVDTP']
        Media        = $fields['media']
        Endpoint     = $fields['endpoint']
        CodecText    = $fields['codec']
        MtuText      = $fields['media MTU']
        SinkName     = $fields['sink name']
        SinkFormats  = $fields['sink formats']
        EndpointFormat = $fields['endpoint format']
        Preference   = $fields['preference']
        Encoder      = $fields['encoder']
        PacingLead   = $fields['pacing lead']
        Buffered     = $fields['buffered']
        Capacity     = $fields['capacity']
        CountersText = $fields['counters']
        LastError    = $fields['last error']
        Driver       = $fields['driver']
        LatencyText  = $fields['latency']
        LatencyPeakText = $fields['latency peak']
        UnderrunText = $fields['underruns']
        TrimmedText  = $fields['trimmed']
        CodecName    = $null
        Rate         = $null
        Channels     = $null
        Bitrate      = $null
        Underruns    = $null
        Packets      = $null
        Dropped      = $null
        SendFailures = $null
        TrimmedBytes = $null
        LatencyMs    = $null
        LatencyPeakMs = $null
        LeadMs       = $null
        BufferedMs   = $null
        SinkCodecs   = @()
        SinkRates    = @()
        PinnedRate   = $null
        PinnedChannels = $null
    }

    if ($report.CodecText -match '^(?<name>[^,]+), (?<rate>\d+) Hz, (?<ch>\d+) ch, (?<bps>\d+) bps') {
        $report.CodecName = $Matches['name']
        $report.Rate = [int]$Matches['rate']
        $report.Channels = [int]$Matches['ch']
        $report.Bitrate = [int]$Matches['bps']
    }
    if ($report.UnderrunText -match '^(\d+) packets short') { $report.Underruns = [int]$Matches[1] }
    if ($report.TrimmedText -match '^(\d+) B of PCM') { $report.TrimmedBytes = [long]$Matches[1] }
    if ($report.CountersText -match 'packets (\d+), dropped (\d+), underruns (\d+), send failures (\d+), trimmed (\d+) B') {
        $report.Packets = [long]$Matches[1]
        $report.Dropped = [long]$Matches[2]
        $report.SendFailures = [long]$Matches[4]
    }
    if ($report.LatencyText -match '^(\d+) ms \(driver side: lead (\d+) \+ buffered (\d+)') {
        $report.LatencyMs = [int]$Matches[1]
        $report.LeadMs = [int]$Matches[2]
        $report.BufferedMs = [int]$Matches[3]
    }
    if ($report.LatencyPeakText -match '^(\d+) ms') { $report.LatencyPeakMs = [int]$Matches[1] }
    if ($report.PacingLead -match '^(\d+) ms') { $report.PacingLeadMs = [int]$Matches[1] }
    if ($report.SinkFormats -match '\(([^)]*)\)') {
        $report.SinkCodecs = @($Matches[1] -split ',\s*' | ForEach-Object { $_.Trim() })
    }
    if ($report.SinkFormats) {
        $report.SinkRates = @([regex]::Matches($report.SinkFormats, '(\d+) Hz') | ForEach-Object { [int]$_.Groups[1].Value })
    }
    if ($report.EndpointFormat -match '^(\d+) Hz, (mono|stereo) \(pinned') {
        $report.PinnedRate = [int]$Matches[1]
        $report.PinnedChannels = if ($Matches[2] -eq 'mono') { 1 } else { 2 }
    }
    return [pscustomobject]$report
}

function Get-Status {
    # Every assertion below reads this: the same text the window copies out.
    $result = Invoke-Ctl 'status'
    # The check returns the result it passed; without the discard it would be this function's
    # first return value, and every `$status.X` below would enumerate it plus the report.
    $null = Assert-CtlOk $result 'OpenA2dpCtl status'
    $status = ConvertFrom-StatusReport -Lines ($result.Output -split "`r?`n")
    $script:Hw.LastStatus = $status
    return $status
}

function Wait-Status {
    param(
        [Parameter(Mandatory)][scriptblock]$Where,
        [Parameter(Mandatory)][string]$Describe,
        [int]$TimeoutSec = 30
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $last = $null
    while ($true) {
        $last = Get-Status
        if (& $Where $last) { return $last }
        if ((Get-Date) -ge $deadline) {
            Fail-Hw "$Describe did not happen within $TimeoutSec s. Last status: media $($last.Media), AVDTP $($last.AVDTP), codec $($last.CodecText), endpoint $($last.Endpoint)."
        }
        Start-Sleep -Milliseconds 500
    }
}

function Assert-Status {
    param([Parameter(Mandatory)][scriptblock]$Where, [Parameter(Mandatory)][string]$Describe)
    $status = Get-Status
    if (-not (& $Where $status)) {
        Fail-Hw "$Describe. Status: media $($status.Media), AVDTP $($status.AVDTP), codec $($status.CodecText), endpoint $($status.Endpoint)."
    }
    return $status
}

function Assert-Streaming {
    param([string]$Codec = $null, [int]$TimeoutSec = 30)
    $describe = if ($Codec) { "the stream to run $Codec" } else { 'the stream to run' }
    $status = Wait-Status -TimeoutSec $TimeoutSec -Describe $describe -Where {
        param($s)
        ($s.Media -eq 'Streaming') -and (-not $Codec -or $s.CodecName -eq $Codec)
    }
    return $status
}

function Assert-True {
    param([Parameter(Mandatory)][bool]$Condition, [Parameter(Mandatory)][string]$Message)
    if (-not $Condition) { Fail-Hw $Message }
}

function Assert-Match {
    param([string]$Text, [Parameter(Mandatory)][string]$Pattern, [Parameter(Mandatory)][string]$Message)
    if (-not ($Text -match $Pattern)) { Fail-Hw "$Message (saw: '$Text')" }
}

function Assert-PacketsAdvancing {
    # A stream can show Streaming while the endpoint sends nothing: the counter separates them.
    param([int]$TimeoutSec = 15)
    $first = Get-Status
    if ($null -eq $first.Packets) { Fail-Hw "status reports no packet counter (media $($first.Media))." }
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 750
        $next = Get-Status
        if ($next.Packets -gt $first.Packets) {
            if ($null -ne $next.SendFailures -and $next.SendFailures -gt 0) {
                Fail-Hw "packets advance but 'send failures' is $($next.SendFailures)."
            }
            return $next
        }
    }
    Fail-Hw "the packet counter did not advance in $TimeoutSec s (stuck at $($first.Packets))."
}

function Assert-Responsive {
    # The hang check of the teardown tests: the driver answers, or it does not.
    param([int]$TimeoutSec = 5)
    $result = Invoke-Ctl 'status'
    Assert-True ($result.ExitCode -eq 0) "status did not answer after the event (exit $($result.ExitCode)): $($result.Text)"
    Assert-True ($result.Seconds -le $TimeoutSec) "status took $([math]::Round($result.Seconds,1)) s, more than the $TimeoutSec s bound."
    return $result
}

function Require-Codec {
    # A procedure that only makes sense on a sink offering a codec skips, not fails.
    param([Parameter(Mandatory)][ValidateSet('sbc', 'aac', 'aptx', 'aptxhd')][string]$Codec)
    $name = switch ($Codec) { 'aptxhd' { 'aptX HD' } 'aptx' { 'aptX' } 'aac' { 'AAC' } 'sbc' { 'SBC' } }
    $status = Get-Status
    if ($status.SinkFormats -eq 'not discovered yet') {
        $status = Wait-Status -TimeoutSec 20 -Describe 'the sink formats to be discovered' -Where { param($s) $s.SinkFormats -ne 'not discovered yet' }
    }
    if ($status.SinkCodecs -notcontains $name) {
        Skip-Test "the sink does not advertise $name (it offers: $($status.SinkCodecs -join ', '))."
    }
    return $status
}

# ---------- audio ----------

function New-ToneWav {
    # One second of PCM repeated, so a long file is cheap to build and a dropout is audible.
    param([int]$Rate = 44100, [int]$Channels = 2, [int]$Seconds = 30)
    $path = Join-Path $env:TEMP ("OpenA2dp-hwtest-tone-{0}-{1}-{2}.wav" -f $Rate, $Channels, $Seconds)
    if (Test-Path -LiteralPath $path) { return $path }

    $frames = $Rate
    $blockAlign = 2 * $Channels
    $dataBytes = $frames * $blockAlign
    $second = New-Object byte[] $dataBytes
    for ($i = 0; $i -lt $frames; $i++) {
        # Two tones either side of the middle of the range: a dropped half second is obvious.
        $frequency = if (($i -lt ($frames / 2))) { 440 } else { 660 }
        $sample = [int16](12000 * [math]::Sin(2 * [math]::PI * $frequency * $i / $Rate))
        for ($c = 0; $c -lt $Channels; $c++) {
            $offset = $i * $blockAlign + 2 * $c
            $second[$offset] = [byte]($sample -band 0xFF)
            $second[$offset + 1] = [byte](($sample -shr 8) -band 0xFF)
        }
    }

    $stream = [IO.File]::Create($path)
    try {
        $writer = New-Object IO.BinaryWriter($stream)
        $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'))
        $writer.Write([int](36 + $dataBytes * $Seconds))
        $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
        $writer.Write([int]16)              # PCM chunk size
        $writer.Write([int16]1)             # PCM
        $writer.Write([int16]$Channels)
        $writer.Write([int]$Rate)
        $writer.Write([int]($Rate * $blockAlign))
        $writer.Write([int16]$blockAlign)
        $writer.Write([int16]16)
        $writer.Write([Text.Encoding]::ASCII.GetBytes('data'))
        $writer.Write([int]($dataBytes * $Seconds))
        for ($n = 0; $n -lt $Seconds; $n++) { $writer.Write($second) }
        $writer.Flush()
    } finally {
        $stream.Dispose()
    }
    return $path
}

function Start-Audio {
    <#
        Plays to the default output, which the procedure makes the sink under test. A real
        player can be used instead by setting OPENA2DP_HWTEST_PLAYER (a command line), which
        the tests that need a player to survive an endpoint flap require.
    #>
    if ($script:Hw.Audio) { return }
    $player = $env:OPENA2DP_HWTEST_PLAYER
    if ($player) {
        $process = Start-Process -FilePath 'cmd.exe' -ArgumentList ('/c ' + $player) -PassThru -WindowStyle Minimized
        $script:Hw.Audio = @{ Kind = 'player'; Process = $process }
        Write-Note "playing through the configured player: $player"
        Start-Sleep -Seconds 2
        return
    }
    $wav = New-ToneWav
    $sound = New-Object System.Media.SoundPlayer $wav
    $sound.PlayLooping()
    $script:Hw.Audio = @{ Kind = 'tone'; Player = $sound; Path = $wav }
    Write-Note 'playing a tone track to the default output'
    Start-Sleep -Seconds 1
}

function Stop-Audio {
    if (-not $script:Hw.Audio) { return }
    if ($script:Hw.Audio.Kind -eq 'tone') {
        try { $script:Hw.Audio.Player.Stop() } catch { }
    } elseif ($script:Hw.Audio.Process -and -not $script:Hw.Audio.Process.HasExited) {
        try { $script:Hw.Audio.Process.Kill() } catch { }
    }
    $script:Hw.Audio = $null
}

# ---------- the log files ----------

function Get-LogLength {
    param([string]$Path)
    if (-not $Path) { return 0 }
    try { return (Get-Item -LiteralPath $Path -ErrorAction Stop).Length } catch { return 0 }
}

function Read-LogWindow {
    <#
        Reads what was appended since $Offset. WinDbg and DbgView hold these files open, so
        the read is shared and a failure means "nothing readable yet", not a test failure.
    #>
    param([string]$Path, [long]$Offset)
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return @() }
    $stream = $null
    try {
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
            ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        $length = $stream.Length
        if ($Offset -gt $length) { $Offset = 0 }   # the log was truncated or reopened
        $null = $stream.Seek($Offset, [IO.SeekOrigin]::Begin)
        $bytes = New-Object byte[] ($length - $Offset)
        $read = $stream.Read($bytes, 0, $bytes.Length)
        if ($read -le 0) { return @() }
        $text = [Text.Encoding]::UTF8.GetString($bytes, 0, $read)
        return @($text -split "`r?`n" | Where-Object { $_ -and $_.Trim() })
    } catch {
        return @()
    } finally {
        if ($stream) { $stream.Dispose() }
    }
}

function New-LogMark {
    # Taken before every repetition: what a test asserts on is only what happened during it.
    $script:Hw.LogMark = @{
        Driver = Get-LogLength $script:Hw.DriverLog
    }
    $script:Hw.LogNote = $null
    if (-not $script:Hw.DriverLog) {
        $script:Hw.LogNote = 'no debugger capture: pass -DriverLog <file> (WinDbg .logopen or DbgView save) to check the driver and service lines.'
    }
}

function Get-NewLog {
    # The service writes its `[svc]` lines to the debugger, so they arrive in the same capture
    # as the driver's lines; there is no service log file.
    param([ValidateSet('Both', 'Service', 'Driver')][string]$Kind = 'Both')
    return @(Read-LogWindow $script:Hw.DriverLog $script:Hw.LogMark.Driver)
}

function Wait-Log {
    <#
        Waits for a line of the captured log. The driver's lines are the ones that show the
        session's own decisions, so a test that expects one waits for it rather than sleeping.
    #>
    param(
        [Parameter(Mandatory)][string]$Pattern,
        [int]$TimeoutSec = 15,
        [ValidateSet('Both', 'Service', 'Driver')][string]$Kind = 'Both',
        [switch]$Optional
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ($true) {
        $matchesInLog = @(Get-NewLog -Kind $Kind | Where-Object { $_ -match $Pattern })
        if ($matchesInLog.Count -gt 0) { return $matchesInLog }
        if ((Get-Date) -ge $deadline) {
            if ($Optional) { return @() }
            $hint = if ($Kind -ne 'Service' -and -not $script:Hw.DriverLog) { " (the kernel log was not captured, so driver lines cannot be checked)" } else { '' }
            $tail = @(Get-NewLog -Kind $Kind | Select-Object -Last 8)
            Fail-Hw "no log line matched '$Pattern' in $TimeoutSec s$hint. Last lines:`n  " + ($tail -join "`n  ")
        }
        Start-Sleep -Milliseconds 300
    }
}

function Assert-LogMatch {
    <#
        -Kernel marks a line only the driver's DbgPrintEx output carries. Without a captured
        kernel log that expectation is recorded as unchecked, never as a pass; with one it is
        checked like any other.
    #>
    param(
        [Parameter(Mandatory)][string]$Pattern,
        [Parameter(Mandatory)][string]$Describe,
        [int]$TimeoutSec = 15,
        [ValidateSet('Both', 'Service', 'Driver')][string]$Kind = 'Both',
        [switch]$Kernel
    )
    if ($Kernel -and -not $script:Hw.DriverLog) {
        Write-Note "${Describe}: unchecked, the kernel log was not captured"
        return @()
    }
    $lines = Wait-Log -Pattern $Pattern -TimeoutSec $TimeoutSec -Kind $(if ($Kernel) { 'Driver' } else { $Kind })
    Write-Note "$Describe"
    return $lines
}

function Assert-LogClean {
    param([string[]]$Allow = @(), [int]$SettleSec = 0)
    if ($SettleSec -gt 0) { Start-Sleep -Seconds $SettleSec }
    $lines = @(Get-NewLog | Where-Object { $_ -match $script:HwLogAuditPattern })
    $unexplained = @()
    foreach ($line in $lines) {
        $allowed = $false
        foreach ($pattern in $Allow) {
            if ($line -match $pattern) { $allowed = $true; break }
        }
        if (-not $allowed) { $unexplained += $line }
    }
    if ($unexplained.Count -gt 0) {
        Fail-Hw ("the log has warnings or errors this test does not allow:`n  " + ($unexplained -join "`n  "))
    }
    if (-not $script:Hw.DriverLog) {
        $script:Hw.LogAuditPartial = $true
        Write-Note 'the log check covered the service log only (no kernel log captured).'
    }
}

# ---------- services and PnP ----------

function Get-EncoderServiceState {
    $text = (& sc.exe query OpenA2dpSvc 2>&1 | Out-String)
    if ($text -match 'STATE\s+:\s+\d+\s+(\w+)') { return $Matches[1] }
    return 'NOT_INSTALLED'
}

function Stop-EncoderService {
    param([int]$TimeoutSec = 20)
    Write-Step 'stopping the encoder service'
    & sc.exe stop OpenA2dpSvc 2>&1 | Out-Null
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ((Get-EncoderServiceState) -in 'STOPPED', 'NOT_INSTALLED') { return }
        Start-Sleep -Milliseconds 300
    }
    Fail-Hw "the encoder service did not stop within $TimeoutSec s (state $(Get-EncoderServiceState))."
}

function Start-EncoderService {
    param([int]$TimeoutSec = 20)
    Write-Step 'starting the encoder service'
    & sc.exe start OpenA2dpSvc 2>&1 | Out-Null
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ((Get-EncoderServiceState) -eq 'RUNNING') { return }
        Start-Sleep -Milliseconds 300
    }
    Fail-Hw "the encoder service did not start within $TimeoutSec s (state $(Get-EncoderServiceState))."
}

function Get-OpenA2dpNode {
    $nodes = @(Get-CimInstance Win32_PnPEntity -Filter "Service='OpenA2dp'" -ErrorAction SilentlyContinue)
    if ($script:Hw.Device) {
        # The node's instance id carries the address, so a machine with two sinks stays unambiguous.
        $digits = ($script:Hw.Device -replace '[:-]', '').ToLower()
        $matching = @($nodes | Where-Object { $_.PNPDeviceID.ToLower() -match $digits })
        if ($matching.Count -gt 0) { return $matching[0] }
    }
    if ($nodes.Count -eq 0) { return $null }
    return $nodes[0]
}

function Get-DeviceNodeStatus {
    param([Parameter(Mandatory)][string]$InstanceId)
    $text = (& pnputil.exe /enum-devices /instanceid $InstanceId 2>&1 | Out-String)
    if ($text -match 'Status:\s+(\S+)') { return $Matches[1] }
    return 'Unknown'
}

function Set-NodeState {
    # pnputil can report success while the node has not moved yet, so the state is the verdict.
    param(
        [Parameter(Mandatory)][string]$InstanceId,
        [Parameter(Mandatory)][ValidateSet('enable', 'disable', 'restart')][string]$Action,
        [string]$Expect,
        [int]$TimeoutSec = 60
    )
    Write-Step "$Action $InstanceId"
    $arguments = @("/$Action-device", $InstanceId)
    $process = Start-Process -FilePath 'pnputil.exe' -ArgumentList $arguments -NoNewWindow -PassThru
    $null = $process.Handle
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ($Expect -and (Get-DeviceNodeStatus $InstanceId) -eq $Expect) { break }
        if ($process.HasExited -and -not $Expect) { break }
        Start-Sleep -Milliseconds 400
    }
    if (-not $process.HasExited) {
        # Seen when the node has moved but pnputil has not returned: do not wait forever.
        if ($Expect -and (Get-DeviceNodeStatus $InstanceId) -eq $Expect) {
            & taskkill.exe /F /PID $process.Id 2>&1 | Out-Null
            return
        }
        & taskkill.exe /F /PID $process.Id 2>&1 | Out-Null
        Fail-Hw "pnputil /$Action-device timed out after $TimeoutSec s; the node is $(Get-DeviceNodeStatus $InstanceId). The driver is not completing its PnP request - break in with the kernel debugger."
    }
    if ($Expect -and (Get-DeviceNodeStatus $InstanceId) -ne $Expect) {
        Fail-Hw "the node is $(Get-DeviceNodeStatus $InstanceId) after /$Action-device, not $Expect."
    }
}

function Get-SinkParentNode {
    param([Parameter(Mandatory)][string]$InstanceId)
    $parent = (Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName 'DEVPKEY_Device_Parent' -ErrorAction SilentlyContinue).Data
    return $parent
}

# ---------- operator ----------

function Prompt-Operator {
    <#
        A step only a person can perform: power the sink, walk out of range, plug the adapter
        back in. The script keeps asserting; the operator supplies the physical event.
    #>
    param([Parameter(Mandatory)][string]$Text, [switch]$NoWait)
    $script:Hw.ManualSteps.Add($Text) | Out-Null
    Write-Host ("[{0}] >>> {1}" -f $script:Hw.Id, $Text) -ForegroundColor Yellow
    if ($NoWait -or $script:Hw.NonInteractive) {
        Write-Note 'continuing without confirmation (-NonInteractive)'
        return
    }
    $null = Read-Host 'Press Enter when done'
}

function Read-Number {
    <#
        A measurement only an instrument produces (the audible latency of T30). The scripted
        part is the arithmetic: what the number has to satisfy is checked here.
    #>
    param([Parameter(Mandatory)][string]$Text, [Parameter(Mandatory)][double]$Default)
    if ($script:Hw.NonInteractive) {
        Write-Note "$Text = $Default (default used because -NonInteractive)"
        return $Default
    }
    $answer = Read-Host ("[{0}] {1} [{2}]" -f $script:Hw.Id, $Text, $Default)
    if (-not $answer) { return $Default }
    return [double]$answer
}

# ---------- sleep ----------

function Suspend-WithWake {
    # Sleeps now and wakes itself: a scheduled task with wake-to-run is the only documented
    # way to bring a machine back without touching it.
    param([int]$WakeAfterSec = 90)
    $taskName = 'OpenA2dpHwTestWake'
    $wakeAt = (Get-Date).AddSeconds($WakeAfterSec)
    & schtasks.exe /Create /TN $taskName /SC ONCE /ST $wakeAt.ToString('HH:mm:ss') /TR 'cmd /c exit' /SC ONCE /F /RL HIGHEST 2>&1 | Out-Null
    # /WakeToRun is not a schtasks switch: it is set on the task after creation.
    $settings = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    if ($settings) {
        $settings.Settings.WakeToRun = $true
        Set-ScheduledTask -TaskName $taskName -Settings $settings.Settings -ErrorAction SilentlyContinue | Out-Null
    }
    $before = (Get-Date)
    Write-Step "sleeping for $WakeAfterSec s (wake to run at $($wakeAt.ToString('HH:mm:ss')))"
    & rundll32.exe powrprof.dll,SetSuspendState 0,1,0
    # A resume shows as the process being suspended: the wall clock jumps far more than the
    # script's own sleeps ever do. Wait for the gap, then for the scheduled task to exist.
    $deadline = $before.AddSeconds($WakeAfterSec + 120)
    Start-Sleep -Seconds 5
    while ((Get-Date) -lt $deadline) {
        $gap = ((Get-Date) - $before).TotalSeconds
        if ($gap -ge $WakeAfterSec) { break }
        Start-Sleep -Seconds 2
    }
    $slept = ((Get-Date) - $before).TotalSeconds
    & schtasks.exe /Delete /TN $taskName /F 2>&1 | Out-Null
    if ($slept -lt ($WakeAfterSec - 10)) {
        Fail-Hw "the machine did not sleep (or did not wake): only $([math]::Round($slept,0)) s passed. In a VM, put the guest to sleep from the Start menu and wake it, then rerun with -NonInteractive."
    }
    Write-Note "slept and woke ($([math]::Round($slept,0)) s wall clock)"
}

function Get-DriverServiceState {
    $text = (& sc.exe query OpenA2dp 2>&1 | Out-String)
    if ($text -match 'STATE\s+:\s+\d+\s+(\w+)') { return $Matches[1] }
    return 'NOT_INSTALLED'
}

function Confirm-Operator {
    # The one thing no script can see: whether the sink is audible, and whether a real player
    # kept playing. A "no" fails the test; -NonInteractive assumes yes and says so.
    param([Parameter(Mandatory)][string]$Text)
    $script:Hw.ManualSteps.Add($Text) | Out-Null
    if ($script:Hw.NonInteractive) { Write-Note "$Text -> assumed yes (-NonInteractive)"; return }
    $answer = Read-Host ("[{0}] {1} [Y/n]" -f $script:Hw.Id, $Text)
    if ($answer -match '^(n|no)$') { Fail-Hw "the operator answered no: $Text" }
}

function Get-DeviceAddress {
    # The registry key and the driver switch name a sink by address, and the status report does
    # not carry one: `list` does.
    if ($script:Hw.Device -match '^([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}$') {
        return ($script:Hw.Device -replace '-', ':').ToUpper()
    }
    $result = Invoke-Ctl 'list'
    $null = Assert-CtlOk $result 'OpenA2dpCtl list'
    $addresses = @([regex]::Matches($result.Output, '([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}') |
        ForEach-Object { $_.Value.ToUpper() } | Select-Object -Unique)
    if ($addresses.Count -eq 0) { Skip-Test 'no sink address in `OpenA2dpCtl list`; pass -Device <address>.' }
    if ($script:Hw.Device -match '^\d+$') {
        $index = [int]$script:Hw.Device
        if ($index -ge 1 -and $index -le $addresses.Count) { return $addresses[$index - 1] }
    }
    if ($addresses.Count -gt 1) { Skip-Test "several sinks ($($addresses -join ', ')); pass -Device <address>." }
    return $addresses[0]
}

function Get-DeviceRegistryKey {
    <#
        The driver names the subkey after the sink address as a 64-bit value, so the digits can
        be in either order. Match both, and accept a lone subkey; anything else is unknown
        rather than guessed.
    #>
    $parent = 'HKLM:\SYSTEM\CurrentControlSet\Services\OpenA2dp\Parameters\Devices'
    if (-not (Test-Path -LiteralPath $parent)) { return $null }
    $keys = @(Get-ChildItem -LiteralPath $parent | Select-Object -ExpandProperty PSChildName)
    if ($keys.Count -eq 0) { return $null }
    $address = (Get-DeviceAddress) -replace '[:.-]', ''
    $pairs = @([regex]::Matches($address, '..') | ForEach-Object { $_.Value })
    [array]::Reverse($pairs)
    $reversed = -join $pairs
    foreach ($key in $keys) {
        if ($key -like "*$address*" -or $key -like "*$reversed*") { return (Join-Path $parent $key) }
    }
    if ($keys.Count -eq 1) { return (Join-Path $parent $keys[0]) }
    return $null
}

function Get-DeviceRegistryValues {
    $key = Get-DeviceRegistryKey
    if (-not $key) {
        Write-Note "no per-sink configuration key found; the registry values were not read"
        return $null
    }
    return (Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue)
}

function Assert-Teardown {
    # The shared expectation of T6-T8 and T10: the channel closes, the driver still answers and
    # the sink can come back.
    param([int]$TimeoutSec = 30)
    Assert-LogMatch -Pattern 'signaling channel remote disconnect|signaling channel closed|link down' `
        -Describe 'the channel closed or the link went down' -TimeoutSec $TimeoutSec -Kernel
    Assert-Responsive
}

# ---------- the test itself ----------

function Start-HwTest {
    <#
        Runs one procedure: prints its steps, repeats it, audits the log at the end (the §4
        check of the procedure) and writes the reporting line of §5. Exit code 0 pass, 1 fail,
        2 skip - so the runner and a hand-run test agree.
    #>
    param(
        [Parameter(Mandatory)][string]$Id,
        [Parameter(Mandatory)][string]$Title,
        [Parameter(Mandatory)][ValidateSet('full', 'assisted', 'manual')][string]$Level,
        [Parameter(Mandatory)][scriptblock]$Body,
        [string]$Device,
        [string]$Ctl,
        [string]$DriverLog,
        [string]$ResultDir,
        [switch]$NonInteractive,
        [int]$Repeat = 1,
        [int]$CtlTimeoutSec = 60,
        [string[]]$AllowWarn = @()
    )

    $script:Hw = @{
        Id          = $Id
        Title       = $Title
        Level       = $Level
        Device      = $Device
        DriverLog   = $DriverLog
        Ctl         = $null
        CtlTimeoutSec = $CtlTimeoutSec
        NonInteractive = [bool]$NonInteractive
        Repeat      = [Math]::Max(1, $Repeat)
        Notes       = New-Object System.Collections.ArrayList
        ManualSteps = New-Object System.Collections.ArrayList
        Failures    = New-Object System.Collections.ArrayList
        LogMark     = @{ Service = 0; Driver = 0 }
        LogAuditPartial = $false
        Audio       = $null
        LastStatus  = $null
        Started     = Get-Date
        Commit      = (& git -C (Split-Path $PSScriptRoot -Parent) rev-parse --short HEAD 2>$null)
    }

    $outcome = 'PASS'
    $reason = ''
    Write-Host ''
    Write-Host ("=== {0} {1} ({2}) ===" -f $Id, $Title, $Level) -ForegroundColor White

    try {
        Assert-True (Test-Elevated) 'the tools and the device control need an elevated prompt; re-run this from an Administrator console.'
        $script:Hw.Ctl = Resolve-Ctl $Ctl
        Assert-True (Test-Path -LiteralPath $script:Hw.Ctl) "OpenA2dpCtl not found at $($script:Hw.Ctl)"
        Write-Note "device: $(if ($Device) { $Device } else { 'the single OpenA2dp sink' }); ctl: $($script:Hw.Ctl)"
        if ($script:Hw.DriverLog) {
            Assert-True (Test-Path -LiteralPath $script:Hw.DriverLog) "the kernel log $DriverLog does not exist"
        } else {
            Write-Note 'no kernel log: driver-side lines are recorded as unchecked, not as passes.'
        }

        for ($run = 1; $run -le $script:Hw.Repeat; $run++) {
            New-LogMark
            if ($script:Hw.Repeat -gt 1) { Write-Step "repetition $run of $($script:Hw.Repeat)" }
            & $Body $run
            Assert-LogClean -Allow $AllowWarn -SettleSec 1
            Stop-Audio
        }
    } catch {
        $message = $_.Exception.Message
        if ($message.StartsWith('SKIP: ')) {
            $outcome = 'SKIP'
            $reason = $message.Substring(6)
        } else {
            $outcome = 'FAIL'
            $reason = $message -replace '^ASSERT: ', ''
            $script:Hw.Failures.Add($reason) | Out-Null
            # The log often says why; keep it with the failure rather than making the operator
            # re-read the file.
            $tail = @(Get-NewLog | Select-Object -Last 12)
            if ($tail.Count -gt 0) { Write-Host ("  recent log:`n    " + ($tail -join "`n    ")) }
        }
    } finally {
        Stop-Audio
    }

    $seconds = [math]::Round(((Get-Date) - $script:Hw.Started).TotalSeconds, 1)
    $sink = if ($script:Hw.LastStatus) { $script:Hw.LastStatus.SinkName } else { 'n/a' }
    $result = [pscustomobject]@{
        id       = $Id
        title    = $Title
        level    = $Level
        outcome  = $outcome
        reason   = $reason
        sink     = $sink
        device   = $Device
        commit   = $script:Hw.Commit
        seconds  = $seconds
        manual   = @($script:Hw.ManualSteps)
        notes    = @($script:Hw.Notes)
        partialLog = $script:Hw.LogAuditPartial
        when     = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')
    }

    $color = switch ($outcome) { 'PASS' { 'Green' } 'SKIP' { 'Yellow' } default { 'Red' } }
    Write-Host ''
    Write-Host ("{0}: {1} - {2} ({3:N1} s)" -f $outcome, $Id, $Title, $seconds) -ForegroundColor $color
    if ($reason) { Write-Host "  $reason" -ForegroundColor $color }
    if ($script:Hw.LogAuditPartial) { Write-Host '  partial log check: no kernel log was captured' -ForegroundColor Yellow }
    $script:Hw.Notes | ForEach-Object { Write-Host "  - $_" -ForegroundColor DarkGray }

    if ($ResultDir) {
        $path = Join-Path $ResultDir ("{0}.json" -f $Id)
        New-Item -ItemType Directory -Force -Path $ResultDir | Out-Null
        $result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $path -Encoding UTF8
    }

    exit $(switch ($outcome) { 'PASS' { 0 } 'SKIP' { 2 } default { 1 } })
}
