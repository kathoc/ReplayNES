# 03 DETERMINISM

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
Build the determinism harness before adding any UI. Feed an input script frame by frame, and compare machine state / video / audio hashes across multiple runs at regular intervals. Also compare a run resumed from a mid-point savestate against a run from the start. On a mismatch, report the first frame and component that diverged. Make it possible to run a scale of at least 10,000 frames as an automated test. Identify and eliminate dependencies on host clock, RNG, and threads.
