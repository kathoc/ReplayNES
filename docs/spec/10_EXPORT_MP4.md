# 10 EXPORT MP4

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
Build an offline renderer for the active take. Using a fresh core, re-run the input/event log from the initial state in normal-speed logical time, producing video frames and PCM. Write an H.264/HEVC + AAC MP4 with AVAssetWriter. Timestamps derive from frame/sample counts. Provide nearest-neighbor scaling, presets such as 1280x960, and overscan / pixel aspect settings. Do not modify the project/timeline before or after export. Write a test that the hashes of the export run match a normal replay.
