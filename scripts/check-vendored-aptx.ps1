<#
.SYNOPSIS
Verifies the vendored aptX HD encoder against codecs\aptx\SHA256SUMS.

.DESCRIPTION
The AOSP sources under codecs\aptx are vendored unmodified, so a change there is either a
mistake or a deliberate re-vendor. Hashes cover the LF form of each file, so a CRLF checkout
verifies the same as an LF one. -Update rewrites the manifest after a deliberate re-vendor.

Exits 0 when every file matches, 1 otherwise.
#>
[CmdletBinding()]
param(
    [switch]$Update
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$vendorRoot = Join-Path $repoRoot 'codecs\aptx'
$manifestPath = Join-Path $vendorRoot 'SHA256SUMS'

function Get-VendoredHash([string]$Path) {
    # Latin1 maps every byte to one char, so the round trip preserves the file exactly.
    $latin1 = [Text.Encoding]::GetEncoding(28591)
    $text = $latin1.GetString([IO.File]::ReadAllBytes($Path)) -replace "`r`n", "`n"
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $hash = $sha.ComputeHash($latin1.GetBytes($text))
    } finally {
        $sha.Dispose()
    }
    return ($hash | ForEach-Object { $_.ToString('x2') }) -join ''
}

$files = Get-ChildItem -Path (Join-Path $vendorRoot 'include'), (Join-Path $vendorRoot 'src') -File -Recurse |
    Sort-Object FullName |
    ForEach-Object { $_.FullName.Substring($vendorRoot.Length + 1) -replace '\\', '/' }

if ($Update) {
    $lines = foreach ($file in $files) {
        "$(Get-VendoredHash (Join-Path $vendorRoot $file))  $file"
    }
    $text = ($lines -join "`r`n") + "`r`n"
    [IO.File]::WriteAllText($manifestPath, $text, (New-Object Text.UTF8Encoding($false)))
    Write-Output "wrote $manifestPath ($($files.Count) files)"
    exit 0
}

if (-not (Test-Path -LiteralPath $manifestPath)) {
    Write-Error "missing manifest: $manifestPath (run with -Update after a re-vendor)"
    exit 1
}

$expected = @{}
foreach ($line in [IO.File]::ReadAllLines($manifestPath)) {
    if ($line.Trim().Length -eq 0) {
        continue
    }
    $parts = $line -split '\s+', 2
    if ($parts.Count -ne 2) {
        Write-Error "bad manifest line: $line"
        exit 1
    }
    $expected[$parts[1]] = $parts[0].ToLowerInvariant()
}

$failed = 0
foreach ($file in $files) {
    if (-not $expected.ContainsKey($file)) {
        Write-Output "not in the manifest: $file"
        $failed = 1
        continue
    }
    $actual = Get-VendoredHash (Join-Path $vendorRoot $file)
    if ($actual -ne $expected[$file]) {
        Write-Output "changed: $file"
        $failed = 1
    }
}
foreach ($file in $expected.Keys) {
    if (-not (Test-Path -LiteralPath (Join-Path $vendorRoot $file))) {
        Write-Output "missing: $file"
        $failed = 1
    }
}

if ($failed -ne 0) {
    Write-Output 'The vendored aptX HD sources differ from their manifest; restore them or re-vendor and run -Update.'
    exit 1
}
Write-Output "vendored aptX HD sources verified ($($files.Count) files)"
exit 0
