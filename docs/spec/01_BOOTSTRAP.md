# 01 BOOTSTRAP

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
Build the skeleton of a SwiftUI-centered macOS app. Separate AppUI / Emulation / CoreAdapter / Input / Recording / Persistence / Export / Tests. Define loadROM, powerOn, softReset, stepFrame(input), videoFrame, audioFrames, serializeState, and deserializeState on CoreAdapter. First implement, with a MockCore, a 60Hz-equivalent frame counter, pause, and 1-frame step. Do not block the UI thread. Done when: clean build, tests pass, the frame is unchanged while paused, and a step advances by exactly +1.
