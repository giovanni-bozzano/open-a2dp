---
name: build-and-test
description: Build Debug x64 and run CodecTests after code changes.
---

Run from the repository root in PowerShell:
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\check-vendored-encoders.ps1
if ($LASTEXITCODE -ne 0) { throw 'Vendored encoder sources changed; re-vendor or run the check with -Update' }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$ms = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
if (-not $ms) { throw 'MSBuild x64 not found' }
& $ms OpenA2dp.slnx /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE); tests not run" }
& .\out\x64\Debug\CodecTests.exe
if ($LASTEXITCODE -ne 0) { throw "CodecTests failed ($LASTEXITCODE)" }
```

Report build result, observed test count/failures and validation limits in ≤3 lines; expand only for actionable failures. Never run stale tests after a failed build or imply hardware validation. No deployment/reboot. For docs-only changes, check diff whitespace and affected local links instead.
