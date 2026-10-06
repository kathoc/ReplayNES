/*
 * ReplayNES engine - stable C API (API version 1).
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The only interface frontends (macOS Swift app, future SDL/Qt/Win32 frontends) use.
 *
 * THREADING
 *   - rn_session / rn_renderer handles are NOT thread-safe: serialize all calls on one handle
 *     (typically: the emulation thread owns the session). Different handles may be used
 *     concurrently on different threads (each owns its own core instance).
 *   - rn_renderer_new() reads the session, so call it on the session's thread; afterwards the
 *     renderer is fully independent (it copied the active take) and may run on any thread.
 *   - rn_input handles ARE internally synchronized: device callbacks (UI thread) may call
 *     rn_input_set_* while the emulation thread calls rn_input_sample_game / poll_hotkeys.
 *   - rn_last_error() is thread-local: the message of the last failed call on this thread.
 *
 * TIME MODEL
 *   Logical time is the integer frame index. Frame f is emulated by rn_step() when
 *   rn_frame() == f; afterwards rn_frame() == f+1. The host clock only decides WHEN to call
 *   rn_step (pacing, pause, slow motion, frame advance); it never affects WHAT is emulated.
 *   NTSC rate = RN_FPS_NUM/RN_FPS_DEN Hz exactly. Video PTS of frame f = f*RN_FPS_DEN/RN_FPS_NUM s
 *   (CMTime(value: f*655171, timescale: 39375000)). Audio is 48 kHz mono int16; frame f emits
 *   exactly rn_audio_samples_for_frame(f) samples starting at sample rn_audio_samples_before(f).
 *
 * BUFFER LIFETIME
 *   Pointers returned by rn_video/rn_audio/rn_*_get (names) stay valid until the next call that
 *   mutates the same handle (step, seek, rewind, take/bookmark changes, open/close).
 *
 * ERRORS
 *   Functions return rn_status. No C++ exception crosses this boundary. Never a silent fallback:
 *   a corrupt project, core mismatch or ROM mismatch is always an explicit error code.
 */
#ifndef REPLAYNES_H
#define REPLAYNES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RN_API_VERSION 1

#define RN_VIDEO_WIDTH 256
#define RN_VIDEO_HEIGHT 240
#define RN_SAMPLE_RATE 48000
#define RN_FPS_NUM 39375000
#define RN_FPS_DEN 655171
#define RN_MAX_SAMPLES_PER_FRAME 1024

/* NES button bits (inputFormatVersion 1). */
#define RN_BTN_A 0x01
#define RN_BTN_B 0x02
#define RN_BTN_SELECT 0x04
#define RN_BTN_START 0x08
#define RN_BTN_UP 0x10
#define RN_BTN_DOWN 0x20
#define RN_BTN_LEFT 0x40
#define RN_BTN_RIGHT 0x80

/* System events, applied BEFORE emulating the frame they are recorded on. */
#define RN_EV_SOFT_RESET 0x01
#define RN_EV_POWER_CYCLE 0x02

typedef enum rn_status {
  RN_OK = 0,
  RN_ERR_INVALID_ARG = 1,
  RN_ERR_IO = 2,
  RN_ERR_NOT_FOUND = 3,
  RN_ERR_CORRUPT = 4,            /* checksum/structure error in project files */
  RN_ERR_CORE_MISMATCH = 5,      /* project recorded with a different core compat ID: refused */
  RN_ERR_ROM_MISMATCH = 6,       /* ROM SHA-256 differs from the project's */
  RN_ERR_ROM_NOT_FOUND = 7,      /* ROM missing at stored path: ask the user, reopen with override */
  RN_ERR_UNSUPPORTED_FORMAT = 8, /* project written by a newer format version */
  RN_ERR_DISK_FULL = 9,
  RN_ERR_ROM_INVALID = 10,
  RN_ERR_STATE = 11,
  RN_ERR_OUT_OF_RANGE = 12,
  RN_ERR_CANCELLED = 13,
  RN_ERR_ALREADY_EXISTS = 14,
  RN_ERR_WRONG_MODE = 15,
  RN_ERR_END_OF_TAKE = 16,
  RN_ERR_DISCONTINUITY = 17,     /* practice B: emulation was not continuous since that slot's A */
  RN_ERR_INTERNAL = 99
} rn_status;

/* ------------------------------------------------------------------ library */
const char* rn_version(void);          /* engine version string */
const char* rn_core_compat_id(void);   /* compatibility ID of the bundled Nestopia core */
const char* rn_core_build_id(void);    /* exact core revision + compiler (informational) */
const char* rn_last_error(void);       /* thread-local; never NULL */
const char* rn_status_name(rn_status s);
rn_status rn_sha256_file(const char* path, char out_hex[65]);
uint64_t rn_audio_samples_before(uint64_t frame);
uint32_t rn_audio_samples_for_frame(uint64_t frame);
/* Writes the generated ReplayNES test ROM (own code, NROM) to path. */
rn_status rn_write_test_rom(const char* path);
void rn_string_free(char* s); /* frees strings returned by rn_*_json functions */

/* ------------------------------------------------------------------ session */
typedef struct rn_session rn_session;

typedef enum rn_core_kind { RN_CORE_NESTOPIA = 0, RN_CORE_MOCK = 1 } rn_core_kind;
/* PRACTICE: non-recording play (see "practice" section below). */
typedef enum rn_mode { RN_MODE_RECORD = 0, RN_MODE_REPLAY = 1, RN_MODE_PRACTICE = 2 } rn_mode;

#define RN_OPEN_DROP_CORRUPT_STATES 0x1u   /* discard corrupt checkpoint files (they are a cache) */
#define RN_OPEN_DROP_CORRUPT_PRACTICE 0x2u /* discard A/B practice slots whose A state is corrupt
                                              (NOT a cache: the slot is lost; see rn_practice_dropped_slots) */

typedef struct rn_session_options {
  uint32_t struct_size;     /* set by rn_session_options_init */
  rn_core_kind core;        /* ignored by rn_session_open (taken from the project) */
  uint32_t dense_interval;  /* frames between rewind checkpoints (default 30) */
  uint32_t dense_capacity;  /* max dense checkpoints kept in memory (default 1200) */
  uint32_t sparse_interval; /* frames between permanent checkpoints (default 600) */
  uint32_t open_flags;      /* RN_OPEN_* */
} rn_session_options;

void rn_session_options_init(rn_session_options* opt);

/* New session for a ROM. project_dir == NULL: in-memory only (use rn_session_save_as later).
 * Otherwise project_dir must not exist or be an empty directory; an initial save is written. */
rn_status rn_session_new(const char* rom_path, const char* project_dir, const rn_session_options* opt,
                         rn_session** out);
/* Opens a .nesrec project. rom_override == NULL uses the stored ROM path. Restores the active
 * take and cursor; replays the journal if the previous run crashed (rn_session_recovered). */
rn_status rn_session_open(const char* project_dir, const char* rom_override, const rn_session_options* opt,
                          rn_session** out);
/* Frees the session. Does NOT save: call rn_session_save first for a clean shutdown. */
void rn_session_close(rn_session* s);
rn_status rn_session_save(rn_session* s);      /* full atomic save; resets the journal */
rn_status rn_session_autosave(rn_session* s);  /* cheap journal append + fsync (call every few s) */
rn_status rn_session_save_as(rn_session* s, const char* project_dir); /* in-memory -> project */
int rn_session_recovered(const rn_session* s);          /* 1 if open() replayed journal data */
int rn_session_has_unsaved_changes(const rn_session* s);
const char* rn_session_rom_sha256(const rn_session* s);
const char* rn_session_rom_path(const rn_session* s);
const char* rn_session_project_dir(const rn_session* s); /* "" for in-memory sessions */
/* Reads manifest.json of a project as JSON text without opening it (free with rn_string_free). */
rn_status rn_project_manifest_json(const char* project_dir, char** out_json);

/* ------------------------------------------------------------------ play / record */
/* RECORD <-> REPLAY switch the take mode. PRACTICE enters practice from the current take
 * position (machine state continues). Leaving PRACTICE (RECORD or REPLAY) restores the take
 * state at the frame practice was entered from, so the take is exactly as before. */
rn_status rn_set_mode(rn_session* s, rn_mode mode);
rn_mode rn_get_mode(const rn_session* s);

typedef struct rn_step_info {
  uint64_t frame;       /* rn_frame() after the call */
  rn_mode mode;
  int branched;         /* RECORD: this step created a new take (rewound then re-recorded) */
  int end_of_take;      /* REPLAY: cursor is at the take end (no frame was emulated if frame unchanged) */
  uint8_t p1, p2, events; /* input actually applied */
  uint64_t take_id;     /* active take after the call */
} rn_step_info;

/* RECORD: emulates one frame with (p1,p2,events) and records it at rn_frame(). Recording at a
 * frame < take length creates a new branch (old future is kept as another take).
 * REPLAY: inputs are ignored; the recorded frame is emulated. At the take end nothing is
 * emulated and info.end_of_take = 1 (returns RN_OK).
 * PRACTICE: emulates one frame with (p1,p2,events) and records NOTHING (rn_frame, take length,
 * takes, checkpoints and the unsaved-changes flag are unchanged); rn_practice_frame() += 1.
 * info may be NULL. */
rn_status rn_step(rn_session* s, uint8_t p1, uint8_t p2, uint8_t events, rn_step_info* info);

const uint32_t* rn_video(const rn_session* s); /* 256x240 BGRA8 (B,G,R,A bytes), alpha 255 */
/* Display-only side channel for signal-level display models (the CRT / composite path): the raw
 * PPU output of the SAME picture as rn_video. Never affects emulation, states or any hash. */
typedef struct rn_video_indices_info {
  const uint16_t* codes;  /* 256x240 9-bit codes: bits 0-5 palette index (after greyscale),
                             bits 6-8 = $2001 colour-emphasis bits 5-7; valid like rn_video */
  uint32_t burst_phase;   /* core colour-burst phase of that frame (0..2; Nestopia's NTSC phase) */
  uint64_t frame;         /* machine frame ordinal that produced it (core frame index) */
} rn_video_indices_info;
/* RN_ERR_UNSUPPORTED_FORMAT when the core has no raw PPU output (mock core). */
rn_status rn_video_indices(const rn_session* s, rn_video_indices_info* out);
/* PCM of the last emulated frame. count = 0 after seek/rewind/take switch (seeking is silent). */
const int16_t* rn_audio(const rn_session* s, size_t* count);

uint64_t rn_frame(const rn_session* s);       /* take cursor; frozen at the return frame in PRACTICE */
uint64_t rn_take_length(const rn_session* s);
/* 0 <= frame <= take length. RN_ERR_WRONG_MODE in PRACTICE (leave practice first). */
rn_status rn_seek(rn_session* s, uint64_t frame);
/* RECORD/REPLAY: seek back on the take, clamps at 0. PRACTICE: rewinds the practice run (bounded
 * in-memory ring, same policy as dense checkpoints), clamped at the current anchor (last A set /
 * goto A, or the practice start) and at the oldest kept snapshot; never touches the take. */
rn_status rn_rewind(rn_session* s, uint64_t nframes);

/* Verification hashes (determinism diagnostics, tests, CLI). */
uint64_t rn_state_hash(rn_session* s);
uint64_t rn_video_hash(const rn_session* s);
uint64_t rn_audio_hash(const rn_session* s);

/* ------------------------------------------------------------------ bookmarks */
typedef struct rn_bookmark_info {
  uint64_t id;
  uint64_t frame;
  uint64_t take_id;     /* take (segment) the bookmark state belongs to */
  const char* name;     /* UTF-8, valid until the next mutating call */
  int on_active_take;   /* 1 if reachable on the active take without switching */
} rn_bookmark_info;

rn_status rn_bookmark_add(rn_session* s, const char* name, uint64_t* out_id); /* at rn_frame() */
rn_status rn_bookmark_remove(rn_session* s, uint64_t id);
rn_status rn_bookmark_rename(rn_session* s, uint64_t id, const char* name);
size_t rn_bookmark_count(const rn_session* s);
rn_status rn_bookmark_get(const rn_session* s, size_t index, rn_bookmark_info* out);
/* Seeks to the bookmark; switches the active take if needed (undoable via rn_undo_take_switch). */
rn_status rn_bookmark_goto(rn_session* s, uint64_t id);

/* ------------------------------------------------------------------ practice + A/B repeat slots */
/* Per-project A/B slots 0..RN_PRACTICE_SLOTS-1, saved in the .nesrec (full save + autosave
 * journal). A = full machine state; B = frame count after A ("length").
 * Anchors: set_a / goto_a start an anchor (counter 0). Emulation continuity is broken by seek,
 * rewind on the take, bookmark goto, take switch/undo, leaving practice and goto_a; practice
 * rewind keeps continuity back to its target. rn_bookmark_add, rn_bookmark_goto,
 * rn_take_activate, rn_undo_take_switch, rn_seek: RN_ERR_WRONG_MODE in PRACTICE. */
#define RN_PRACTICE_SLOTS 8

typedef struct rn_practice_slot_info {
  int has_a;              /* 0 = empty slot (all other fields zero / "") */
  int has_b;
  uint64_t length_frames; /* B - A (valid if has_b) */
  const char* name;       /* UTF-8, valid until the next mutating call */
  int has_take_frame;     /* 1 if A was set on the take (not inside a practice run) */
  uint64_t take_frame;    /* take frame at A (informational label) */
  uint64_t take_id;       /* take active when A was set */
  uint64_t created_seq;   /* project-wide sequence numbers (ordering; no wall-clock time) */
  uint64_t updated_seq;
  int b_settable;         /* 1 if rn_practice_set_b would pass the continuity check now */
} rn_practice_slot_info;

typedef struct rn_practice_status {
  int active;                /* 1 in RN_MODE_PRACTICE */
  int anchor_slot;           /* slot of the live current anchor, -1 = practice start / none */
  uint64_t counter;          /* == rn_practice_frame() */
  uint64_t return_frame;     /* take frame restored when practice is left (== rn_frame) */
  uint64_t rewind_available; /* frames rn_rewind can go back in practice (0 outside) */
} rn_practice_status;

/* Captures the CURRENT machine state as A of slot (any mode); counter = 0 from here; clears B.
 * Keeps the slot's name. RN_ERR_OUT_OF_RANGE for slot >= RN_PRACTICE_SLOTS. */
rn_status rn_practice_set_a(rn_session* s, uint32_t slot);
/* length = frames emulated since this slot's A anchor (>= 1). RN_ERR_NOT_FOUND: no A;
 * RN_ERR_DISCONTINUITY: seek/load/take switch since the anchor (also after reopening: goto A
 * first); RN_ERR_INVALID_ARG: 0 frames. */
rn_status rn_practice_set_b(rn_session* s, uint32_t slot);
/* Defines slot = [a_frame, b_frame) from take frames (cursor positions on the active take, e.g.
 * picked on a timeline) without playing it: A = take state at a_frame (as if rn_seek(a_frame) then
 * rn_practice_set_a), length = b_frame - a_frame, has_take_frame = 1, take_id = active take.
 * Keeps the slot's name. The cursor, machine state, video buffer and the continuity of other
 * slots' anchors are restored exactly afterwards; rn_audio reports 0 samples (like a seek).
 * Costs a seek (checkpoint + replay). RN_ERR_WRONG_MODE in PRACTICE; RN_ERR_INVALID_ARG unless
 * a_frame < b_frame; RN_ERR_OUT_OF_RANGE if b_frame > take length or slot >= RN_PRACTICE_SLOTS. */
rn_status rn_practice_set_range(rn_session* s, uint32_t slot, uint64_t a_frame, uint64_t b_frame);
/* Enters PRACTICE if needed (remembering the take position), loads A, counter = 0. A loop:
 * step until rn_practice_frame() == length_frames, then goto_a again. RN_ERR_NOT_FOUND: no A. */
rn_status rn_practice_goto_a(rn_session* s, uint32_t slot);
rn_status rn_practice_slot_get(const rn_session* s, uint32_t slot, rn_practice_slot_info* out);
rn_status rn_practice_slot_rename(rn_session* s, uint32_t slot, const char* name); /* NOT_FOUND if empty */
rn_status rn_practice_slot_clear(rn_session* s, uint32_t slot);                   /* OK if already empty */
/* Frames since the current anchor (set_a / goto_a / practice start); 0 once continuity breaks. */
uint64_t rn_practice_frame(const rn_session* s);
rn_status rn_practice_get_status(const rn_session* s, rn_practice_status* out);
/* Bitmask of slots discarded by rn_session_open with RN_OPEN_DROP_CORRUPT_PRACTICE (tell the user). */
uint32_t rn_practice_dropped_slots(const rn_session* s);

/* ------------------------------------------------------------------ takes (branches) */
typedef struct rn_take_info {
  uint64_t id;
  uint64_t parent_id;     /* 0 = root */
  uint64_t branch_frame;  /* frame where this take diverges from its parent */
  uint64_t length;        /* total frames of this take (root..this) */
  uint64_t created_seq;   /* creation order */
  int is_active;
  uint32_t child_count;
} rn_take_info;

size_t rn_take_count(const rn_session* s);
rn_status rn_take_get(const rn_session* s, size_t index, rn_take_info* out); /* ordered by id */
uint64_t rn_active_take(const rn_session* s); /* 0 = nothing recorded yet */
rn_status rn_take_activate(rn_session* s, uint64_t take_id); /* keeps cursor if possible; undoable */
/* "前の試行へ戻す": returns to the take active before the last branch/switch, at that frame. */
rn_status rn_undo_take_switch(rn_session* s);
size_t rn_undo_depth(const rn_session* s);

/* ------------------------------------------------------------------ input pipeline */
typedef struct rn_input rn_input;

typedef enum rn_socd { RN_SOCD_NEUTRAL = 0, RN_SOCD_LAST_WINS = 1, RN_SOCD_ALLOW = 2 } rn_socd;

/* Hotkey bits returned by rn_input_poll_hotkeys (bind with action names "hk.<name>"). */
#define RN_HK_PAUSE (1u << 0)
#define RN_HK_FRAME_ADVANCE (1u << 1)
#define RN_HK_REWIND (1u << 2)
#define RN_HK_SLOW (1u << 3)
#define RN_HK_BOOKMARK (1u << 4)
#define RN_HK_SOFT_RESET (1u << 5)
#define RN_HK_POWER_CYCLE (1u << 6)
#define RN_HK_TOGGLE_MODE (1u << 7)
#define RN_HK_SAVE (1u << 8)
#define RN_HK_UNDO_TAKE (1u << 9)
#define RN_HK_FAST_FORWARD (1u << 10)
#define RN_HK_STEP_BACK (1u << 11)

rn_input* rn_input_new(void);
void rn_input_free(rn_input* in);
/* physical_id: free-form stable string chosen by the frontend, e.g. "kb:49", "gc0:buttonA",
 * "gc0:dpad.up". Analog sticks: rn_input_set_axis("gc0:lstick", x, y) produces virtual ids
 * "gc0:lstick.left/.right/.up/.down". action: "p1.a" "p1.b" "p1.select" "p1.start" "p1.up"
 * "p1.down" "p1.left" "p1.right" (same for p2), "p1.turbo_a" "p1.turbo_b" "p2.turbo_a"
 * "p2.turbo_b", hotkeys "hk.pause" "hk.frame_advance" "hk.rewind" "hk.slow" "hk.bookmark"
 * "hk.soft_reset" "hk.power_cycle" "hk.toggle_mode" "hk.save" "hk.undo_take" "hk.fast_forward"
 * "hk.step_back". One physical id may map to several actions and vice versa. */
rn_status rn_input_bind(rn_input* in, const char* physical_id, const char* action);
rn_status rn_input_unbind(rn_input* in, const char* physical_id, const char* action /* NULL = all */);
void rn_input_clear_bindings(rn_input* in);
rn_status rn_input_load_json(rn_input* in, const char* json);
char* rn_input_save_json(const rn_input* in); /* free with rn_string_free */
rn_status rn_input_set_turbo(rn_input* in, uint32_t period, uint32_t duty); /* frames; 1<=duty<=period */
rn_status rn_input_set_socd(rn_input* in, rn_socd policy);
rn_status rn_input_set_analog_threshold(rn_input* in, float threshold); /* 0..1, default 0.5 */
void rn_input_set_pressed(rn_input* in, const char* physical_id, int pressed);
void rn_input_set_axis(rn_input* in, const char* stick_id, float x, float y); /* y>0 = up */
/* Controller disconnected: release every pressed id starting with prefix (e.g. "gc0:"). The
 * frontend should also pause. */
void rn_input_release_prefix(rn_input* in, const char* prefix);
void rn_input_release_all(rn_input* in);
/* Final game bitfields for logical frame `frame` (pass rn_frame(session)); taps shorter than a
 * frame are latched once. Feed the result to rn_step - that output is what gets recorded. */
void rn_input_sample_game(rn_input* in, uint64_t frame, uint8_t* p1, uint8_t* p2);
/* Hotkey press edges since the previous poll + currently held hotkeys (works while paused). */
void rn_input_poll_hotkeys(rn_input* in, uint32_t* pressed_edges, uint32_t* held);

/* ------------------------------------------------------------------ offline renderer (export) */
typedef struct rn_renderer rn_renderer;

/* Snapshots the active take; renders frames [start_frame, end_frame) (end_frame 0 = take length)
 * on a fresh core from power-on. The session is not modified and may keep running. */
rn_status rn_renderer_new(rn_session* s, uint64_t start_frame, uint64_t end_frame, rn_renderer** out);
uint64_t rn_renderer_total_frames(const rn_renderer* r);
uint64_t rn_renderer_frames_done(const rn_renderer* r); /* progress = done/total */
/* Next frame: video (256x240 BGRA), PCM samples, absolute frame index.
 * Returns RN_ERR_END_OF_TAKE when finished. Cancel = stop calling and free. */
rn_status rn_renderer_next(rn_renderer* r, const uint32_t** video, const int16_t** audio, size_t* sample_count,
                           uint64_t* frame_index);
/* rn_video_indices for the frame returned by the last rn_renderer_next. */
rn_status rn_renderer_video_indices(const rn_renderer* r, rn_video_indices_info* out);
uint64_t rn_renderer_hash(const rn_renderer* r); /* running hash of output (compare with replay) */
void rn_renderer_free(rn_renderer* r);

/* ------------------------------------------------------------------ photosensitive flash reduction */
/* Display-side filter that limits large-area luminance / saturated-red flashes (WCAG 2.x general
 * and red flash thresholds) in the frames a frontend SHOWS (live view, exported video). It is a
 * pure deterministic function of the frame sequence passed to it + its level; it never reads or
 * changes a session, so recorded input, emulation and every verification hash are unaffected.
 * Feed consecutive displayed frames; call rn_flash_filter_reset on discontinuities (seek, load,
 * new session, take switch, start of a rewind or scrub) so unrelated pictures are not blended.
 * It reduces flashes; it cannot guarantee that no seizure is triggered. Algorithm and parameters:
 * docs/FLASH_REDUCTION.md. Not thread-safe per handle. */
typedef struct rn_flash_filter rn_flash_filter;

typedef enum rn_flash_level {
  RN_FLASH_OFF = 0,      /* output == input */
  RN_FLASH_LOW = 1,      /* "弱": WCAG thresholds as written, <= 3 flashes/s on >= 25% of the screen */
  RN_FLASH_STANDARD = 2, /* "標準" (recommended default): earlier detection, <= 2 flashes/s, >= 20% */
  RN_FLASH_HIGH = 3      /* "強": <= 1 flash/s, >= 15% of the screen, smaller residual flicker */
} rn_flash_level;

typedef struct rn_flash_info {
  int altered;                  /* 1 if the output differs from the input this frame */
  uint32_t altered_blocks;      /* 16x16 blocks changed (of 240) */
  uint32_t event_area_permille; /* screen share of simultaneous same-direction transitions */
  int large_area;               /* 1 if a large-area flash candidate was detected */
} rn_flash_info;

rn_flash_filter* rn_flash_filter_new(rn_flash_level level); /* NULL on invalid level */
void rn_flash_filter_free(rn_flash_filter* f);
void rn_flash_filter_reset(rn_flash_filter* f);
rn_status rn_flash_filter_set_level(rn_flash_filter* f, rn_flash_level level); /* also resets */
rn_flash_level rn_flash_filter_get_level(const rn_flash_filter* f);
/* in/out: 256x240 BGRA8 like rn_video (in == out allowed). info may be NULL. */
rn_status rn_flash_filter_process(rn_flash_filter* f, const uint32_t* in, uint32_t* out, rn_flash_info* info);

#ifdef __cplusplus
}
#endif
#endif /* REPLAYNES_H */
