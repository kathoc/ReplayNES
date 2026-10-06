# 08 TAS ASSIST

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
Implement production aids: pause, 1-frame advance, arbitrary N-frame advance, 1/2 and 1/4 slow, timeline scrub, bookmark, soft reset, and power cycle. Pause time and slow speed during production must not count toward the recorded game time. Slow only lowers the emulation frame cadence and must not break input frame numbers. Record reset/power as system events and test that the replay matches.
