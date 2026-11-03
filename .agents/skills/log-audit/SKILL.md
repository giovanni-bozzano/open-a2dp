# Log Audit

## Description
Find driver log patterns (media stop, BRB timeout, send pending) in one turn.

## Instructions
```
Select-String -Path driver\*.cpp -Pattern 'media stop: still waiting|BRB did not complete|send did not complete|read still pending' | Select-Object LineNumber, Line
```

## Report format
- Pattern found / not found
- File + line number
- Suggest: check `OA2DP_WARN` vs `OA2DP_ERROR` levels