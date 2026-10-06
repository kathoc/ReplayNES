# 05 REWIND BRANCH

# Prompt Pack
These are prompts for implementing the app step by step with Codex CLI, Claude Code, or similar tools. Run them one at a time, in order starting from 00. Do not hand over all the prompts at once.

Common rules for every prompt:
- First read the repo, README, MASTER_SPEC, and the existing tests.
- Before starting, briefly state what will change, what will not change, and how it will be tested.
- Do not implement large amounts of out-of-scope work ahead of time.
- Always run the build and tests.
- No fallbacks that hide errors.
- Do not let host time, nondeterministic RNG, or races leak into emulation results.
- Do not add ROMs or copyrighted material to the repo.
- On completion, report what changed, the test results, and remaining issues.

## Task for This Step
Implement checkpoints + rewind + branching. Periodic savestates and bookmark states; seek by fast-replaying from the nearest state at or before the target frame. If input is made after a rewind, do not overwrite the old future: create a new branch/segment. The active take must be playable as a single path. Build a minimal API that lets undo return to a previous branch. For the UI, implement only pause / rewind / scrubber / bookmark first.
