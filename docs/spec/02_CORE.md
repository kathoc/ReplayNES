# 02 CORE

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
Integrate the NES core chosen in the ADR. Through the wrapper, implement ROM SHA-256, power-on, 1-frame step, P1/P2 input, framebuffer, PCM, soft reset, and state serialize/deserialize. Record the third-party licenses. Done when: save state -> run 100 frames -> restore -> run the same 100 frames of input, and the final hashes match. Reset must also reproduce at the same frame with matching hashes.
