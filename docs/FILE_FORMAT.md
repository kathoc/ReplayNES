# `.nesrec` project format (formatVersion 2)

A project is a **directory** (macOS shows it as a package). The ROM is never stored; it is
referenced by path and identified by SHA-256. Implementation: `engine/src/persist/ProjectStore.cpp`.

```
Name.nesrec/
  manifest.json              identity + compatibility; written LAST in a full save
  timeline/index.json        segments, checkpoints, head/cursor/undo/bookmarks ("meta")
  timeline/segments/<id>.seg input records of one segment (binary, CRC32)
  states/<id>.state          checkpoint machine states (binary, CRC32) - a cache, never the truth
  states/practice-<slot>-<seq>.state  A state of an A/B practice slot (binary, CRC32) - NOT a cache
  metadata/bookmarks.json    human-readable mirror of index.json meta.bookmarks
  metadata/practice.json     human-readable mirror of index.json meta.practice
  journal/journal.bin        append-only changes since the last full save (CRC32 per record)
```

All integers in binary files are little-endian. `varint` = unsigned LEB128.

## Concepts

* **Frame index**: logical frame number. Record *f* is applied when the machine has emulated *f*
  frames; afterwards it has emulated *f+1*. A "state at frame *f*" is the machine after *f* frames.
* **InputRecord** (inputFormatVersion 1): `p1`, `p2` = final NES bitfields given to the core
  (bit0 A, 1 B, 2 Select, 3 Start, 4 Up, 5 Down, 6 Left, 7 Right); `events`: bit0 soft reset,
  bit1 power cycle, applied *before* the frame. Other event bits are invalid in version 1.
* **Segment**: `{id, parent (0 = root), start, createdSeq, records[]}`. Records never change;
  a segment may only grow at its end. A child starts strictly after its parent's start.
* **Take** = path root → segment. On a path each segment covers `[start, start of next)`
  (the last one: its own end). Every segment is a take (id = segment id).
* **State owner**: a state at frame *f* belongs to the segment covering frame *f-1* on the path
  where it was captured (0 for *f* = 0). It is valid for every path that contains the owner with
  `owner.start < f <= end of owner on that path`.

## Versions

| formatVersion | change |
|---|---|
| 1 | initial format |
| 2 | A/B practice slots: `meta.practice`, `states/practice-*.state`, journal record type 4, `metadata/practice.json` |

Readers accept every version up to their own; a version-1 project opens with no practice slots and
is written as version 2 by the next full save. A newer version → `RN_ERR_UNSUPPORTED_FORMAT`.

## manifest.json

```json
{
  "format": "replaynes-project",
  "formatVersion": 2,
  "appVersion": "0.1.0",
  "generation": 7,
  "coreCompatId": "nestopia-ue@7b5c87d8dc3c+p2+adapter1+ntsc",
  "coreBuild": "nestopia-ue 7b5c87d8... patchlevel 2 clang-21.0.0",
  "stateFormatVersion": 1,
  "inputFormatVersion": 1,
  "region": "NTSC",
  "rom": { "sha256": "<64 hex>", "size": 24592, "lastPath": "/path/game.nes", "name": "game.nes" }
}
```

## timeline/index.json

```json
{
  "generation": 7, "nextSegmentId": 4, "seq": 3, "nextCheckpointId": 120,
  "segments":    [ {"id":1,"parent":0,"start":0,"count":2400,"createdSeq":1,"file":"timeline/segments/1.seg"} ],
  "checkpoints": [ {"id":20,"frame":600,"owner":1,"kind":"sparse","file":"states/20.state","crc32":123} ],
  "meta": {
    "activeHead": 3, "cursorFrame": 1900, "mode": "record", "romPath": "...", "nextBookmarkId": 2,
    "undo": [ {"head":1,"frame":1900} ],
    "bookmarks": [ {"id":1,"name":"stage 2","frame":1200,"owner":1,"stateId":41} ],
    "practice": {
      "nextSeq": 9,
      "slots": [ {"slot":0,"name":"boss","stateSeq":7,"file":"states/practice-0-7.state","crc32":123,
                  "size":20480,"hasB":true,"lengthFrames":340,"takeFrame":1200,"takeId":3,
                  "createdSeq":2,"updatedSeq":9} ]
    }
  }
}
```

`meta.mode` is the *take* mode (`record`/`replay`); practice mode is never persisted, and while
practicing `cursorFrame` is the take frame practice will return to.

`meta.practice` (version ≥ 2; absent = no slots): up to 8 slots, `slot` 0..7 unique. `stateSeq`
is the sequence number at which A was captured and names the state file (a new A gets a new file,
so the commit point stays `index.json`). `crc32`/`size` describe the core state bytes. `takeFrame`
is `null` when A was set inside a practice run (no take position). `createdSeq`/`updatedSeq` come
from the project-wide counter `nextSeq` (ordering only; no wall-clock time is stored, nothing here
affects emulation). Runtime anchors used to validate B are not persisted.

`kind`: `sparse` (permanent, every 600 f by default), `bookmark`, `head` (state at the cursor for
exact resume). Dense rewind checkpoints are memory-only.

## Segment file `<id>.seg`

| field | type |
|---|---|
| magic | u32 `RNSG` (0x47534E52) |
| version | u32 = 1 |
| id, parent, start, createdSeq, count | u64 ×5 |
| records | repeated `{varint run>=1, u8 p1, u8 p2, u8 events}` expanding to exactly `count` records |
| crc32 | u32 over all preceding bytes |

The file may contain *more* records than `index.json` says (appends written by a save that did not
commit); the loader uses the first `count`. Fewer records, bad CRC or header mismatch → corrupt.

## State file `<id>.state`

`u32 'RNST', u32 version=1, u64 id, u64 frame, u64 owner, u8 kind, u32 stateFormatVersion,
varint+bytes compatId, u64 n, n bytes core state, u32 crc32`. The core state itself is the engine
envelope `u32 'RNCS', u32 1, varint+bytes compatId, u64 frameIndex, u64 n, Nestopia NST state
(uncompressed)`. A state whose compat ID differs from the project's is a core mismatch.

## Practice state file `practice-<slot>-<seq>.state`

`u32 'RNPS' (0x53504E52), u32 version=1, u8 slot, u64 stateSeq, u32 stateFormatVersion,
varint+bytes compatId, u64 n, n bytes core state (engine envelope as above), u32 crc32` (over all
preceding bytes). `slot`/`stateSeq` must match the index entry and `crc32(core state)`/`n` must
match the entry's `crc32`/`size`.

## Journal `journal/journal.bin`

Header: `u32 'RNJL', u32 1, u64 baseGeneration, u32 crc32(previous 16 bytes)`.
Records: `u32 'RNJR', u32 len, len bytes body, u32 crc32(body)`; body = `u8 type` + payload:

| type | payload |
|---|---|
| 1 SEG_NEW | u64 id, parent, start, createdSeq |
| 2 SEG_APPEND | u64 id, u64 offset, u64 n, RLE records (n) — offset must equal the current length (idempotent overlap allowed if identical) |
| 3 META | UTF-8 JSON, same schema as `index.json` `meta` |
| 4 PRACTICE_STATE | u8 slot, u64 stateSeq, u32 stateFormatVersion, varint+bytes compatId, u64 n, n bytes core state (= practice state file body). Always written before the META that references it |

## Write protocol

Every file write is *temp file in the same directory → write → flush → fsync → rename over target →
fsync directory* (`util/Fs.cpp`). Journal appends are `append → fsync`; on failure the file is
truncated back to its previous size so later appends stay parseable.

**Full save** (`rn_session_save`), generation g → g+1:
1. write new/changed segment files, not-yet-persisted state files and new practice A state files;
2. write `timeline/index.json` with generation g+1 (**commit point**);
3. write `metadata/bookmarks.json` and `metadata/practice.json` mirrors, then `manifest.json` with
   generation g+1;
4. reset the journal (header base g+1); 5. delete unreferenced state files (incl. replaced
   practice A states).

While practicing, the `head` checkpoint is the take state saved when practice was entered, never
the practice machine state.

**Autosave** (`rn_session_autosave`): append SEG_NEW / SEG_APPEND for unjournaled data,
PRACTICE_STATE for practice A states not yet in a file or the journal, and META if it changed,
then fsync. Cheap; call every few seconds.

## Open / recovery rules

1. `manifest.json` must parse; `format` must match; `formatVersion` > supported → `RN_ERR_UNSUPPORTED_FORMAT`;
   `coreCompatId` ≠ this build's → `RN_ERR_CORE_MISMATCH` (never opened, see COMPATIBILITY.md).
2. ROM: override path or `rom.lastPath`; missing → `RN_ERR_ROM_NOT_FOUND`; SHA-256 differs →
   `RN_ERR_ROM_MISMATCH`. After opening with an override the new path is saved on the next save.
3. `index.generation` must equal `manifest.generation` or be exactly one higher (crash after the
   index commit but before the manifest write: the index is complete and is used).
4. Segments: CRC, header and length checks → `RN_ERR_CORRUPT` naming the file.
5. States: CRC/metadata/compat checks → `RN_ERR_CORRUPT`, unless the caller passes
   `RN_OPEN_DROP_CORRUPT_STATES` (explicit opt-in; dropped files are reported). States are only
   an acceleration cache, so dropping them never changes replay results.
6. Journal: header must be valid. If `base == index.generation` the records are replayed. An
   incomplete or bad record at the **end** is a torn write from a crash: it is discarded and the
   file truncated. A bad record followed by any valid record → `RN_ERR_CORRUPT`. A journal with an
   older base is stale (already folded into the index) and ignored. `rn_session_recovered()` = 1
   when records were replayed.
7. Practice slots (index meta + journal): each A state must exist (file of the index, or a
   journal PRACTICE_STATE record), pass CRC/size/slot/seq checks → else `RN_ERR_CORRUPT` naming the
   slot and file; a different compat ID → `RN_ERR_CORE_MISMATCH`. Practice states are user data,
   not a cache, so `RN_OPEN_DROP_CORRUPT_STATES` does **not** cover them: only the separate opt-in
   `RN_OPEN_DROP_CORRUPT_PRACTICE` discards such slots (reported via `rn_practice_dropped_slots`,
   project marked unsaved). Checks run after journal replay, so a slot whose A was replaced in the
   journal is judged by its newest state.
8. The machine is brought to the cursor by power-on + nearest valid checkpoint + replay. The
   session opens in the persisted take mode, never in practice.

Locking against two processes opening the same project is the frontend's job (v1).
