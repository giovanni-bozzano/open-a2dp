# Status / Session Handoff

## Description
One-turn context recovery using git log/status instead of reading docs.

## Instructions
1. `git log --oneline -n 5` — last 5 commit summaries
2. `git status -s` — uncommitted changes
3. Return: commit SHAs + file state in ≤6 lines

## Cache note
Output ≤6 lines; kills the need to read SESSION-CONTEXT.md at session start.