# 00 RESEARCH

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
Do a technical survey before the main implementation. Compare Mesen-family, Nestopia UE-family, and FCEUX-family cores, NES cores via libretro, and other strong candidates. Evaluate them on: macOS arm64 embedding, 1-frame step, input injection, full savestates, determinism, reset/power, raw video/audio, license, Swift/C/C++ bridge, and core build pinning with playback of old projects. Produce docs/ARCHITECTURE_DECISION.md. State the recommended core, module boundaries, thread model, the top 5 risks, and the PoC to build first. Do not build out the main implementation at this stage.
