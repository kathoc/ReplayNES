# 07 INPUT TURBO

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
Integrate GameController.framework and keyboard input. Define remapping, hotkey separation, analog -> D-pad threshold, and an opposite-direction policy. Pause safely when a controller disconnects. Implement turbo for A/B etc. with frame period/duty. Important: what is handed to the recorder is the bitfield after turbo conversion. Write a test that the replay is identical regardless of whether a physical controller is present.
