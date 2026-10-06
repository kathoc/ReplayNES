# 04 RECORD REPLAY

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
Implement FrameInputLog. Associate the final P1/P2 bitfields and system events with frameIndex. Use a simple, verifiable scheme for change-point / RLE compression. Add a test that replays a recorded session from the start on a new core instance and checks that the hashes match. Do not make turbo or physical mapping information the source of truth. The final input actually passed to the core is the source of truth.
