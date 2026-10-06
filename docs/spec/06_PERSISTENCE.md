# 06 PERSISTENCE

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
Implement saving of the `.nesrec` package: manifest, timeline segments/index, states, bookmarks, and journal. Store formatVersion, ROM SHA-256, core compatibility ID/build, and serialization version. Implement atomic temp-write + rename, checksums, autosave, and crash recovery. Write an integration test that quits the app -> relaunches -> reopens as if a day later -> restores the active head -> and can rewind further into the past. Do not save the ROM itself.
