# Build and Test

## Description
Builds the solution with MSBuild x64 Debug and runs CodecTests, returning a concise summary. This skill replaces ~15 manual tool calls with one turn.

## Instructions
1. **Find MSBuild**: Resolve the latest VS Build path using vswhere.
   ```powershell
   $ms = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -find "MSBuild\**\Bin\amd64\MSBuild.exe" | Select-Object -First 1
   ```
2. **Build OpenA2dp**: Run the MSBuild command for the solution file.
   ```powershell
   & $ms OpenA2dp.slnx /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
   ```
3. **Run CodecTests**: If the build succeeds (exit code 0), execute the test binary.
   ```powershell
   .\out\x64\Debug\CodecTests.exe
   ```
4. **Report result**: Return a single line with:
   - Build status (success / failed at line X)
   - Test count and failures (e.g., "76/76 pass" or "24 fail at SbcTests.cpp:42")
   - Any unrecoverable errors

## Cache note
The skill always runs the full build+test cycle, but its *output* is short (≈3 lines) so KV‑cache hits remain high across sessions.