# The cost of the rewind record for large states

User request 2026-11-01: Rad Mobile stopped briefly at a regular interval; "not only Rad Mobile:
remove every way the rewind record can become heavy, for all the ROMs, not only the ones on the Deck".

## Measured

- tools/checkpoint_cost (ReplayArcade) over the whole ROM folder (884 sets): 18 states over 10 MB
  (Sega System 32 / Multi 32, Jaleco MS32: 22-23 MB), 49 over 2 MB, 112 over 1 MB.
- tools/frame_times (rn_step per frame as the app records, background autosave every 300 frames):
  Rad Mobile on a Mac before this work: a dense checkpoint (every 30 frames) 62 ms; after b132d66
  10-14 ms at every sparse checkpoint (every 600 frames), 20-25 ms at every autosave that had a new
  sparse state, up to 48 ms.

## Causes and what was done

| Cause | Change |
| --- | --- |
| CRC32 of every dense checkpoint (never written) | Dense: no checksum; CRC32 slice-by-16 (b132d66) |
| A fresh 22 MB buffer per checkpoint: zero fill + page faults | the buffers of dropped dense checkpoints are reused (CheckpointStore::takeSpare, stateenv::begin keeps a same-size buffer) |
| Dense count bounded, bytes not: 1200 x 22 MB = 26 GB | `denseBytes` (1 GiB) besides `denseCapacity` |
| Every sparse checkpoint kept in memory, and opening read them all | persisted ones beyond `sparseResidentBytes` (512 MiB) leave memory, read back when used (Session::checkpointData); opening trims after each state read |
| Practice snapshots unbounded | bounded by `denseBytes` too |
| CRC32 of every sparse checkpoint on the frame thread | computed when its file is written (StateWriter) |
| Autosave wrote each new sparse state on the frame thread: copy into a file image, two CRCs, write, fsync | StateWriter thread; the file in parts (header, bytes, CRC) with no copy; referenced by a later autosave (rn_session_autosave_background) |

## On a Steam Deck (2026-11-01, after the above)

Rad Mobile in game mode, 40 s with saves: a checkpoint frame still took 15 ms (others 6 ms); the
input lead sat at its limit (22 ms), 16 frames missed their target, judder 550/min. A 22 MB copy
alone takes 1.7 ms there: before dense checkpoints were dropped (the first 1 GiB) a checkpoint frame
cost +12 ms (fresh memory: allocation, faults, zeroing with THP), after it +5.5 ms. Neither is needed
for the picture, and both sat between the input sample and the picture:

- `defer_checkpoints` (SessionOptions::deferCheckpoints; the desktop app sets it): rn_step leaves the
  checkpoint (or practice snapshot) of its frame pending; rn_session_checkpoint_work, called in the
  frame's slack after the picture (EmulationController::afterFrame), takes it and prepares a touched
  buffer for the next one. Whatever changes the machine (every core step, state load, power-on goes
  through Session::coreStep / coreLoadState / coreLoadContent) or the take (clearRewindCache, which
  every other operation calls first) takes it first; a pending one whose frame or take owner changed
  meanwhile is dropped (a cache entry less). Test: the same session with and without deferral records
  the same checkpoints and reaches the same states (and a version without those guards fails it).

## Not adopted

- Leaving System 32's tilemap draw buffers out of the state (FBNeo): they are not a pure cache, the
  seek replays differed (withdrawn, see ReplayArcade b6dd3a0).
- Making `rn_session_autosave` itself not wait: callers (tests, "autosave then close") rely on the
  sparse states being referenced when it returns. A separate call for the frame loop instead.
- Writing dense checkpoints anywhere: they are a memory cache only.

## Done when

- test_persist: offloaded states read back for seek / reopen / truncation (and a corrupted read
  back fails the test); background autosave references states only after they are written, a
  checkpoint removed while being written is safe, its file collected by the next full save.
- The whole test suite, ThreadSanitizer on test_persist / test_rewind_cache.
- frame_times on every set over 1 MB: no checkpoint or autosave frame stands out.
- On a Steam Deck: Rad Mobile's frames.
