# Agent Efficiency Guide

## 1. Minimal Context Loading
- **Never read entire files** - use `grep`/`glob` to find exact symbols
- **Read only needed sections**: 
  - `grep -n "function_name" file.cpp | head -5` to find first 5 lines
  - `sed -n '200,250p' file.cpp` to read specific lines
  - `offset:line_number limit:line_count` in `read` tool

## 2. Build & Test Workflow
- **Single-turn build**: `build-and-test` skill runs:
  ```powershell
  $ms = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -find "MSBuild\**\Bin\amd64\MSBuild.exe" | Select-Object -First 1
  & $ms OpenA2dp.slnx /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo
  .\out\x64\Debug\CodecTests.exe
  ```
- **Output**: "76/76 pass" or "24 fail at SbcTests.cpp:42"

## 3. Deadlock Diagnosis
- **One-turn check**: `windbg-deadlock-check` skill:
  1. `tools.windbg.request_break()`
  2. `!wdfkd.wdfdriverinfo OpenA2dp 0x1f`
  3. `!wdfkd.wdfdevice`
  4. Return 3-line summary of owning thread + blocked stack

## 4. Log Analysis Best Practices
- **Search patterns**:
  - `grep "media stop: still waiting" driver/*.cpp` → find WARN messages
  - `grep "no response within" driver/*.cpp` → find state-specific timeouts
- **Key messages to watch**:
  - `WARN Media: stop: still waiting for the media work item to release the lock`
  - `WARN L2CAP: BRB did not complete (timeout); abandoning it`
  - `ERROR L2CAP: send did not complete within`

## 5. Codebase Structure
- **File locations**:
  - `driver/Driver.cpp` - entry points
  - `driver/L2cap*.cpp` - L2CAP logic
  - `driver/Signaling*.cpp` - AVDTP state machine
  - `driver/Media*.cpp` - media pacing
  - `driver/Service*.cpp` - IOCTLs and rings
- **Each file ≤ 250 lines** - split only when necessary

## 6. Cache Optimization
- **Immutable AGENTS.md** - never change top 80%
- **Modular docs** - keep volatile notes in separate files
- **Skill files** - encapsulate multi-step workflows
- **Short outputs** - all skills return ≤ 5 lines