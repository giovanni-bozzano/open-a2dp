<#
.SYNOPSIS
Verifies the vendored aptX and aptX HD encoders against their SHA256SUMS.

.DESCRIPTION
The AOSP sources under codecs\aptx and codecs\aptx\classic are vendored unmodified, so a
change there is either a mistake or a deliberate re-vendor. Hashes cover the LF form of each
file, so a CRLF checkout verifies the same as an LF one. -Update rewrites both manifests after
a deliberate re-vendor.

Exits 0 when every file matches, 1 otherwise.
#>
[CmdletBinding()]
param(
    [switch]$Update
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
# Each drop is a directory with include\ and src\ and its own SHA256SUMS next to them.
$drops = @(
    @{ Name = 'aptX HD'; Root = Join-Path $repoRoot 'codecs\aptx' },
    @{ Name = 'aptX'; Root = Join-Path $repoRoot 'codecs\aptx\classic' }
)

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

function Get-VendoredFiles([string]$Root) {
    return Get-ChildItem -Path (Join-Path $Root 'include'), (Join-Path $Root 'src') -File -Recurse |
        Sort-Object FullName |
        ForEach-Object { $_.FullName.Substring($Root.Length + 1) -replace '\\', '/' }
}

$failed = 0
$verified = 0
foreach ($drop in $drops) {
    $root = $drop.Root
    $manifestPath = Join-Path $root 'SHA256SUMS'
    $files = Get-VendoredFiles $root

    if ($Update) {
        $lines = foreach ($file in $files) {
            "$(Get-VendoredHash (Join-Path $root $file))  $file"
        }
        $text = ($lines -join "`r`n") + "`r`n"
        [IO.File]::WriteAllText($manifestPath, $text, (New-Object Text.UTF8Encoding($false)))
        Write-Output "wrote $manifestPath ($($files.Count) files)"
        continue
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
            Write-Error "bad manifest line in $manifestPath`: $line"
            exit 1
        }
        $expected[$parts[1]] = $parts[0].ToLowerInvariant()
    }

    foreach ($file in $files) {
        if (-not $expected.ContainsKey($file)) {
            Write-Output "not in the manifest: $(Join-Path $drop.Name $file)"
            $failed = 1
            continue
        }
        $actual = Get-VendoredHash (Join-Path $root $file)
        if ($actual -ne $expected[$file]) {
            Write-Output "changed: $(Join-Path $drop.Name $file)"
            $failed = 1
        }
    }
    foreach ($file in $expected.Keys) {
        if (-not (Test-Path -LiteralPath (Join-Path $root $file))) {
            Write-Output "missing: $(Join-Path $drop.Name $file)"
            $failed = 1
        }
    }
    $verified += $files.Count
}

if ($Update) {
    exit 0
}
if ($failed -ne 0) {
    Write-Output 'A vendored aptX source differs from its manifest; restore it or re-vendor and run -Update.'
    exit 1
}
Write-Output "vendored aptX and aptX HD sources verified ($verified files)"
exit 0
