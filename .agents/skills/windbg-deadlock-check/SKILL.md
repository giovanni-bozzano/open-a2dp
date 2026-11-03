# WinDbg Deadlock Check

## Description
Captures current kernel thread states, FDO power/PnP states, and wait lock owners for OpenA2dp without manual command chaining. Replaces ~10 manual steps with one turn.

## Instructions
1. **Break into target** (if running):
   ```powershell
   tools.windbg.request_break()
   ```
2. **Run diagnostic commands**:
   ```powershell
   !wdfkd.wdfdriverinfo OpenA2dp 0x1f
   !wdfkd.wdfdevice
   ```
3. **Summarize**: Return 3 lines with:
   - Thread ID owning the lock
   - Blocked call stack (first 2 frames)
   - Current state (Idle/Streaming/etc.)

## Cache note
The skill outputs ≤5 lines, so KV‑cache hits stay high across sessions.