# 12 HARDEN

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
Harden the app for long-running use. Test hours' worth of record/replay, hundreds of rewinds/branches, save/reopen, crash recovery, corrupt state/log, disk full, controller disconnect, a moved ROM, and core mismatch. On a core mismatch, never open silently. Document the migration / legacy-core policy. Profile memory/disk usage and optimize the checkpoint interval / segment compaction only as far as necessary.
