/*
 * ReplayNES shared frontend core - stable C API (API version 1).
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Platform-independent frontend logic shared by every frontend (macOS Swift app, Linux SDL app,
 * future Windows / iOS): display pacing math, input catalog / default bindings / layout
 * migrations / controller face mapping, session resume record + lock + launch decision,
 * timeline + filmstrip geometry and thumbnail cache policy, ROM library scan + project matching,
 * transport / practice state machines, export + streaming geometry, localization table.
 * Platform APIs (Metal, Vulkan, AppKit, SDL, AVFoundation, FFmpeg, GameController) stay in the
 * frontends; nothing here opens windows, devices or clocks. See docs/PORTING.md.
 *
 * CONVENTIONS
 *   - Status codes are the engine's rn_status (RN_OK, RN_ERR_*). No C++ exception crosses this
 *     boundary. rnf_last_error() is thread-local: the message of the last failed call.
 *   - char* results are heap strings owned by the caller: free with rnf_string_free().
 *     const char* results point to static data (valid for the life of the process) unless
 *     documented otherwise.
 *   - Sized outputs use the two-call pattern: a function writing into (out, cap) returns the
 *     total count; it writes min(count, cap) items (out may be NULL when cap == 0).
 *   - Times are seconds (double). Wall-clock dates are seconds since 1970-01-01 UTC.
 *   - Booleans are int (0 / 1).
 *
 * THREADING
 *   - Free functions (no handle) are pure and thread-safe, except rnf_l10n_set_language (call it
 *     once at startup, before other threads use localized strings).
 *   - Handles are NOT thread-safe (serialize all calls on one handle) unless documented:
 *     rnf_thumb_cache and rnf_rom_hash_cache ARE internally synchronized.
 *   - Functions taking an rn_session* follow the engine rule: call them on the session's thread.
 *
 * LOCALIZATION
 *   Display strings come from a table generated at build time from
 *   apps/macos/Resources/Localizable.xcstrings (the single source of truth for en / ja). Keys are
 *   the English source strings; values use Apple-style format specifiers (%@, %lld, %1$@ ...).
 */
#ifndef REPLAYNES_FRONTEND_H
#define REPLAYNES_FRONTEND_H

#include <stddef.h>
#include <stdint.h>

#include "replaynes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RNF_API_VERSION 1

/* ================================================================== general */
uint32_t rnf_api_version(void);
const char* rnf_last_error(void); /* thread-local; never NULL */
void rnf_string_free(char* s);
/* One NTSC frame in seconds (RN_FPS_DEN / RN_FPS_NUM). */
double rnf_frame_period(void);

/* ================================================================== localization */
/* "ja" when the first preferred language starts with "ja" (case-insensitive), else "en". */
const char* rnf_ui_language_choose(const char* const* preferred, size_t count);
/* Language used by every display string of this library ("ja" / "en"; anything else = "en").
 * Default "en". Call once at startup, before other threads use localized strings. */
void rnf_l10n_set_language(const char* lang);
const char* rnf_l10n_language(void);
/* Localized value of key in the current language / in lang; key itself if it is not in the table. */
const char* rnf_l10n_lookup(const char* key);
const char* rnf_l10n_lookup_in(const char* lang, const char* key);
/* Table enumeration (for frontends that build their own lookup / tooling). */
size_t rnf_l10n_count(void);
int rnf_l10n_entry(size_t index, const char** key, const char** en, const char** ja);

typedef enum rnf_arg_type { RNF_ARG_INT = 0, RNF_ARG_UINT = 1, RNF_ARG_DOUBLE = 2, RNF_ARG_STRING = 3 } rnf_arg_type;
typedef struct rnf_arg {
  rnf_arg_type type;
  int64_t i;
  uint64_t u;
  double d;
  const char* s; /* UTF-8 (%@) */
} rnf_arg;
/* Formats an Apple-style format string (%@, %lld, %llu, %d, %.1f, %016llx, %%, positional %2$@)
 * with args (no locale grouping). */
char* rnf_format(const char* format, const rnf_arg* args, size_t count);
/* rnf_format(rnf_l10n_lookup(key), args). */
char* rnf_l10n_format(const char* key, const rnf_arg* args, size_t count);

/* ================================================================== display pacing
 * Pure decision logic for display-locked pacing (docs/FRAME_PACING.md). The caller drives it
 * from its display callback with presentation timestamps (seconds). */

/* DisplayCadence: which display refreshes start a new emulated frame. Locked when the refresh
 * rate is an integer multiple k of the NES rate (every k-th refresh), free otherwise. */
#define RNF_CADENCE_LOCK_TOLERANCE 0.005
typedef struct rnf_cadence rnf_cadence;
rnf_cadence* rnf_cadence_new(double frame_period); /* <= 0: rnf_frame_period() */
rnf_cadence* rnf_cadence_clone(const rnf_cadence* c);
void rnf_cadence_free(rnf_cadence* c);
/* One display callback whose picture appears at t. Returns 1 when it starts a new frame. */
int rnf_cadence_refresh(rnf_cadence* c, double presentation);
double rnf_cadence_refresh_interval(const rnf_cadence* c); /* 0 until two timestamps were seen */
int rnf_cadence_refreshes_per_frame(const rnf_cadence* c); /* 0 = free (not locked / unknown) */
double rnf_cadence_emulation_rate(const rnf_cadence* c);
double rnf_cadence_expected_frame_interval(const rnf_cadence* c);
double rnf_cadence_frame_period(const rnf_cadence* c);
/* Refreshes per frame for a refresh interval (0 = not a multiple of the frame period). */
int rnf_refreshes_per_frame(double refresh, double frame_period);

/* Displays slower than the emulation rate (40-59 Hz modes, nested compositors, 30 Hz): the
 * refresh interval is longer than the frame period by more than RNF_CADENCE_LOCK_TOLERANCE.
 * Emulation keeps the NES rate by emulating up to RNF_MAX_FRAMES_PER_PRESENT frames for one present
 * (down to 30.05 Hz; slower displays slow the emulation down). */
#define RNF_MAX_FRAMES_PER_PRESENT 2
int rnf_display_slower_than_frames(double refresh, double frame_period); /* frame_period <= 0: NES */

/* FrameBudget: how many frames one present needs so the emulated time follows the displayed time
 * (a Bresenham-style accumulator: 45 Hz -> 1,2,1,1,2,1,...; average refresh / frame period). The
 * debt it keeps is within +-1/2 frame period; time beyond max_frames (a stall, skipped refreshes)
 * is dropped instead of being caught up in a burst. */
typedef struct rnf_frame_budget rnf_frame_budget;
rnf_frame_budget* rnf_frame_budget_new(double frame_period); /* <= 0: rnf_frame_period() */
rnf_frame_budget* rnf_frame_budget_clone(const rnf_frame_budget* b);
void rnf_frame_budget_free(rnf_frame_budget* b);
void rnf_frame_budget_reset(rnf_frame_budget* b);
/* A present `interval` seconds of display time after the previous one: frames to emulate for it
 * (1...max_frames; max_frames < 1 counts as 1). */
int rnf_frame_budget_frames(rnf_frame_budget* b, double interval, int max_frames);
double rnf_frame_budget_debt(const rnf_frame_budget* b); /* emulated time owed, seconds */
uint64_t rnf_frame_budget_dropped(const rnf_frame_budget* b); /* frames dropped (not caught up) */

/* InputDeadline: input is sampled `lead` seconds before the commit deadline. */
#define RNF_INPUT_DEADLINE_MARGIN 0.0015
#define RNF_INPUT_DEADLINE_MIN_LEAD 0.002
#define RNF_INPUT_DEADLINE_MAX_LEAD 0.010
#define RNF_INPUT_DEADLINE_MISS_PENALTY 0.0005
#define RNF_INPUT_DEADLINE_MAX_PENALTY 0.003
#define RNF_INPUT_DEADLINE_LATE_COMMIT 0.0005
#define RNF_INPUT_DEADLINE_DECAY_AFTER 600
#define RNF_INPUT_DEADLINE_PENALTY_DECAY 0.0001
#define RNF_INPUT_DEADLINE_WINDOW 600
typedef struct rnf_input_deadline rnf_input_deadline;
rnf_input_deadline* rnf_input_deadline_new(void);
rnf_input_deadline* rnf_input_deadline_clone(const rnf_input_deadline* d);
void rnf_input_deadline_free(rnf_input_deadline* d);
void rnf_input_deadline_observe_work(rnf_input_deadline* d, double seconds); /* sample -> commit */
void rnf_input_deadline_observe_miss(rnf_input_deadline* d);
double rnf_input_deadline_lead(const rnf_input_deadline* d);
double rnf_input_deadline_work_quantile(const rnf_input_deadline* d);
double rnf_input_deadline_penalty(const rnf_input_deadline* d);
uint64_t rnf_input_deadline_misses(const rnf_input_deadline* d);
/* Limits of the lead (defaults RNF_INPUT_DEADLINE_MIN_LEAD / _MAX_LEAD / _MAX_PENALTY). A frontend
 * whose deadline is the vblank itself (Linux: the compositor's latch point before it is unknown)
 * lets the lead grow past one refresh. Values <= 0 keep the current limit; the penalty is clamped
 * to the new maximum. */
void rnf_input_deadline_set_limits(rnf_input_deadline* d, double min_lead, double max_lead, double max_penalty);
double rnf_input_deadline_max_lead(const rnf_input_deadline* d);

/* PresentPath: whether the layer goes direct to the display (every recent presentDelay ~1 refresh). */
#define RNF_PRESENT_PATH_WINDOW 60
typedef struct rnf_present_path rnf_present_path;
rnf_present_path* rnf_present_path_new(void);
rnf_present_path* rnf_present_path_clone(const rnf_present_path* p);
void rnf_present_path_free(rnf_present_path* p);
void rnf_present_path_observe(rnf_present_path* p, double present_delay);
void rnf_present_path_reset(rnf_present_path* p);
double rnf_present_path_max_delay(const rnf_present_path* p);
int rnf_present_path_is_direct(const rnf_present_path* p, double refresh);

/* BacklogDrain: when a steady one-drawable backlog is drained. */
typedef struct rnf_backlog_policy {
  int late_run;        /* consecutive late presents */
  double min_interval; /* seconds since the previous drain */
} rnf_backlog_policy;
#define RNF_BACKLOG_FAST_LATE_RUN 4
#define RNF_BACKLOG_FAST_MIN_INTERVAL 0.25
#define RNF_BACKLOG_RARE_LATE_RUN 60
#define RNF_BACKLOG_RARE_MIN_INTERVAL 5.0
rnf_backlog_policy rnf_backlog_drain_policy(int variable_refresh, int direct);

/* BuildAhead: GPU-heavy pictures are built one refresh ahead when their p90 GPU time exceeds
 * what the display path leaves after the commit deadline. */
#define RNF_BUILD_AHEAD_WINDOW 120
#define RNF_BUILD_AHEAD_DELAY_WINDOW 60
#define RNF_BUILD_AHEAD_ENTER_MARGIN 0.0025
#define RNF_BUILD_AHEAD_LEAVE_MARGIN 0.004
typedef struct rnf_build_ahead rnf_build_ahead;
rnf_build_ahead* rnf_build_ahead_new(void);
rnf_build_ahead* rnf_build_ahead_clone(const rnf_build_ahead* b);
void rnf_build_ahead_free(rnf_build_ahead* b);
void rnf_build_ahead_add(rnf_build_ahead* b, double gpu_seconds);
void rnf_build_ahead_reset(rnf_build_ahead* b);
void rnf_build_ahead_update(rnf_build_ahead* b, double present_delay);
int rnf_build_ahead_active(const rnf_build_ahead* b);
int rnf_build_ahead_p90(const rnf_build_ahead* b, double* out); /* 0 until a quarter window was seen */

/* AudioRateControl: dynamic rate control keeping the audio buffer level (|ratio-base| <= 0.5 %). */
#define RNF_DRC_MAX_DEVIATION 0.005
#define RNF_DRC_GAIN 0.005
#define RNF_DRC_FILL_SMOOTHING 0.02
typedef struct rnf_audio_rate rnf_audio_rate;
rnf_audio_rate* rnf_audio_rate_new(double target_fill);
rnf_audio_rate* rnf_audio_rate_clone(const rnf_audio_rate* a);
void rnf_audio_rate_free(rnf_audio_rate* a);
/* Frames are emulated at rate Hz while the content is nominal Hz (<= 0: 1 / rnf_frame_period()). */
void rnf_audio_rate_set_frame_rate(rnf_audio_rate* a, double rate, double nominal);
void rnf_audio_rate_reset(rnf_audio_rate* a);
double rnf_audio_rate_update(rnf_audio_rate* a, double fill); /* returns the ratio for this frame */
double rnf_audio_rate_base(const rnf_audio_rate* a);
double rnf_audio_rate_ratio(const rnf_audio_rate* a);
double rnf_audio_rate_target_fill(const rnf_audio_rate* a);
int rnf_audio_rate_smoothed_fill(const rnf_audio_rate* a, double* out); /* 0 = none yet */

/* AudioResampler: streaming 4-point Hermite resampler (int16 mono, ratio clamped to 0.5...2). */
typedef struct rnf_resampler rnf_resampler;
rnf_resampler* rnf_resampler_new(void);
rnf_resampler* rnf_resampler_clone(const rnf_resampler* r);
void rnf_resampler_free(rnf_resampler* r);
void rnf_resampler_reset(rnf_resampler* r);
/* Upper bound of the output count of one rnf_resampler_process call with n input samples. */
size_t rnf_resampler_output_bound(size_t n);
/* Resamples n samples; writes the output to out (out_cap >= rnf_resampler_output_bound(n), else
 * RN_ERR_INVALID_ARG and nothing is consumed). *out_count = samples written. */
rn_status rnf_resampler_process(rnf_resampler* r, const int16_t* in, size_t n, double ratio, int16_t* out,
                                size_t out_cap, size_t* out_count);

/* FramePacing counters: how evenly new emulated frames reach the screen. */
#define RNF_FRAME_PACING_CONTINUITY_LIMIT 0.25
typedef struct rnf_frame_pacing rnf_frame_pacing;
rnf_frame_pacing* rnf_frame_pacing_new(void);
rnf_frame_pacing* rnf_frame_pacing_clone(const rnf_frame_pacing* p);
void rnf_frame_pacing_free(rnf_frame_pacing* p);
double rnf_frame_pacing_hitch_interval(void); /* 1.5 frame periods */
void rnf_frame_pacing_present(rnf_frame_pacing* p, uint64_t frame, double time);
uint64_t rnf_frame_pacing_presents(const rnf_frame_pacing* p);
uint64_t rnf_frame_pacing_hitches(const rnf_frame_pacing* p);
uint64_t rnf_frame_pacing_skipped(const rnf_frame_pacing* p);
double rnf_frame_pacing_window_max(const rnf_frame_pacing* p);
double rnf_frame_pacing_take_window_max(rnf_frame_pacing* p); /* returns and resets */
int rnf_frame_pacing_last_present(const rnf_frame_pacing* p, uint64_t* frame, double* time);

/* CallbackRegularity: callbacks later than late_after after the previous one (gaps > idle_after ignored). */
typedef struct rnf_callback_regularity rnf_callback_regularity;
rnf_callback_regularity* rnf_callback_regularity_new(double late_after, double idle_after);
rnf_callback_regularity* rnf_callback_regularity_clone(const rnf_callback_regularity* r);
void rnf_callback_regularity_free(rnf_callback_regularity* r);
void rnf_callback_regularity_tick(rnf_callback_regularity* r, double t);
void rnf_callback_regularity_set_late_after(rnf_callback_regularity* r, double late_after);
double rnf_callback_regularity_late_after(const rnf_callback_regularity* r);
double rnf_callback_regularity_idle_after(const rnf_callback_regularity* r);
uint64_t rnf_callback_regularity_count(const rnf_callback_regularity* r);
uint64_t rnf_callback_regularity_late(const rnf_callback_regularity* r);
double rnf_callback_regularity_window_max(const rnf_callback_regularity* r);
double rnf_callback_regularity_take_window_max(rnf_callback_regularity* r);

/* ================================================================== string lists */
/* Immutable list of (a, b, value) entries returned by input functions. */
typedef struct rnf_list rnf_list;
size_t rnf_list_count(const rnf_list* l);
const char* rnf_list_a(const rnf_list* l, size_t i); /* valid until rnf_list_free */
const char* rnf_list_b(const rnf_list* l, size_t i); /* "" when unused */
int64_t rnf_list_value(const rnf_list* l, size_t i);
void rnf_list_free(rnf_list* l);

/* ================================================================== input catalog
 * Physical ids: "kb:<code>" (keyboard; code per scheme below), "gc<slot>:<element>"
 * (controller slot 0..3). Controller elements are POSITIONAL: face.south/east/west/north,
 * dpad.up/down/left/right, lstick.* / rstick.*, leftShoulder, rightShoulder, leftTrigger,
 * rightTrigger, menu, options, home, leftThumb, rightThumb (+ paddle.l4/r4/l5/r5, misc,
 * touchpad from SDL). NES A = east, NES B = south on every controller. */
typedef struct rnf_binding {
  const char* input;  /* physical id */
  const char* action; /* engine action, e.g. "p1.a", "hk.pause" */
} rnf_binding;

typedef enum rnf_action_group { RNF_GROUP_PLAYER1 = 0, RNF_GROUP_PLAYER2 = 1, RNF_GROUP_HOTKEY = 2 } rnf_action_group;
typedef struct rnf_action_info {
  const char* id;    /* static */
  rnf_action_group group;
} rnf_action_info;
/* All actions in catalog order: player 1, player 2 (10 each), then hotkeys. */
size_t rnf_input_action_count(void);
int rnf_input_action_get(size_t index, rnf_action_info* out);
char* rnf_input_action_label(const char* action_id); /* localized; NULL for unknown ids */
char* rnf_input_group_title(rnf_action_group group);

/* Keyboard code schemes for "kb:" ids. */
typedef enum rnf_keyboard_scheme {
  RNF_KEYBOARD_MACOS = 0, /* macOS virtual key codes (kVK_*) */
  RNF_KEYBOARD_SDL = 1    /* SDL3 scancodes (SDL_SCANCODE_*) */
} rnf_keyboard_scheme;

/* Current layout version of saved controller bindings (see migrations below). */
#define RNF_CONTROLLER_LAYOUT_VERSION 6
/* Default keyboard + controller layout for a scheme (static strings). */
size_t rnf_input_default_binding_count(rnf_keyboard_scheme scheme);
int rnf_input_default_binding_get(rnf_keyboard_scheme scheme, size_t index, rnf_binding* out);
/* Engine config JSON (rn_input_load_json) of the defaults. */
char* rnf_input_default_config_json(rnf_keyboard_scheme scheme);
/* Controller hotkeys of layout 1 (<= 0.1.x) and layout 2+ (pairs in a = input, b = action). */
rnf_list* rnf_input_legacy_controller_hotkeys(void);
rnf_list* rnf_input_controller_hotkeys(void);
/* Layout-2 face defaults (GameController names "gc<slot>:buttonA" ...) of a slot. */
rnf_list* rnf_input_legacy_face_defaults(int slot);
int rnf_input_is_legacy_face(const char* input, int slot);

/* Parsed engine input config (rn_input_save_json text). */
typedef struct rnf_input_config rnf_input_config;
rn_status rnf_input_config_parse(const char* json, rnf_input_config** out); /* RN_ERR_CORRUPT on bad JSON */
void rnf_input_config_free(rnf_input_config* c);
size_t rnf_input_config_binding_count(const rnf_input_config* c);
int rnf_input_config_binding_get(const rnf_input_config* c, size_t index, rnf_binding* out); /* valid until free */
int rnf_input_config_turbo_period(const rnf_input_config* c);
int rnf_input_config_turbo_duty(const rnf_input_config* c);
const char* rnf_input_config_socd(const rnf_input_config* c);
double rnf_input_config_analog_threshold(const rnf_input_config* c);

/* Migration / reset plans over a binding table: *unbind and *bind receive (input, action) pairs
 * (empty lists when there is nothing to do). Apply unbinds first, then binds. */
/* Layout 1 -> 2: untouched 0.1.x controller hotkeys -> the 0.2.0 hotkeys. */
void rnf_input_controller_layout_migration(const rnf_binding* bindings, size_t count, rnf_list** unbind,
                                           rnf_list** bind);
/* Layout 3 -> 4: pad 1's triggers, if still exactly the layout-3 defaults (R2 rewind, L2 fast-forward),
 * are swapped to the new defaults (L2 rewind, R2 fast-forward). */
void rnf_input_trigger_swap_migration(const rnf_binding* bindings, size_t count, rnf_list** unbind, rnf_list** bind);
/* Layout 2 -> 3 step 1 (on load): slots with untouched layout-2 face defaults get positional defaults. */
void rnf_input_face_layout_migration(const rnf_binding* bindings, size_t count, rnf_list** unbind, rnf_list** bind);
/* Layout 2 -> 3 step 2 (controller attached): legacy GameController names of slot move to the
 * positions they have on THAT controller. names[i] ("buttonA" ...) -> positions[i] (rnf_face_position). */
void rnf_input_legacy_face_translation(const rnf_binding* bindings, size_t count, int slot, const char* const* names,
                                       const int* positions, size_t position_count, rnf_list** unbind,
                                       rnf_list** bind);
/* "Reset to Defaults" for one controller slot (every gc<slot>: binding -> the defaults). */
void rnf_input_controller_reset_plan(const rnf_binding* bindings, size_t count, int slot, rnf_list** unbind,
                                     rnf_list** bind);
/* Applies a plan to an engine input table. Returns 1 if it did anything. */
int rnf_input_apply_plan(rn_input* in, const rnf_list* unbind, const rnf_list* bind);
/* Layout 4 -> 5: the Quick Menu action "hk.menu" (docs/design/UI_REDESIGN.md). Binds pad 1's L+R combo
 * ("gc0:leftShoulder+gc0:rightShoulder") unless pad 1 already has an hk.menu binding, and Esc (kb:53 macOS /
 * kb:41 SDL) unless Esc is bound to something or hk.menu is bound anywhere. R (hk.pause) and L (hk.slow) stay;
 * the menu no longer opens on a pause. */
void rnf_input_menu_migration(const rnf_binding* bindings, size_t count, rnf_keyboard_scheme scheme, rnf_list** unbind,
                              rnf_list** bind);
/* The HOME / guide button (Steam, Xbox, PS, Nintendo HOME: element "home") belongs to the system
 * (Steam overlay, Game Bar, ...): never assignable, never captured, never routed to bindings.
 * rnf_input_element_ignored: 1 for such an element ("home"; also accepts "gc<n>:home"). */
int rnf_input_element_ignored(const char* element);
/* Layout 5 -> 6: drops every binding whose input is (or a combo containing) an ignored element. */
void rnf_input_home_migration(const rnf_binding* bindings, size_t count, rnf_list** unbind, rnf_list** bind);
/* Two-input combo ids "<a>+<b>" (both held: e.g. the Quick Menu's L+R). rnf_input_combo_id: NULL for an empty
 * part. rnf_input_combo_split: 1 and the parts (free with rnf_string_free) when id is a combo. */
char* rnf_input_combo_id(const char* a, const char* b);
int rnf_input_combo_split(const char* id, char** a, char** b);
/* Frame-step direction while paused: controller ids bound to a D-pad left/right game action
 * (a = input, value = -1 back / +1 forward). Sticks are excluded. */
rnf_list* rnf_input_paused_step_directions(const rnf_binding* bindings, size_t count);

/* Display name of a physical id ("Key ↑", "Pad 1 Right Button"). face_label (may be NULL): the
 * printed label of a connected controller's face button, appended as "(A)". */
char* rnf_input_display_name(rnf_keyboard_scheme scheme, const char* physical_id, const char* face_label);
/* Controller slot of a "gc<slot>:..." id. Returns 0 for other ids. */
int rnf_input_controller_slot(const char* physical_id, int* slot);

/* ------------------------------------------------------------------ controllers */
typedef enum rnf_face_position {
  RNF_FACE_SOUTH = 0, RNF_FACE_EAST = 1, RNF_FACE_WEST = 2, RNF_FACE_NORTH = 3
} rnf_face_position;
const char* rnf_face_position_element(rnf_face_position p); /* "face.south" ... */
const char* rnf_face_position_name(rnf_face_position p);    /* "south" ... */

typedef enum rnf_controller_family {
  RNF_FAMILY_NINTENDO = 0, RNF_FAMILY_XBOX = 1, RNF_FAMILY_PLAYSTATION = 2, RNF_FAMILY_GENERIC = 3,
  RNF_FAMILY_STEAM_DECK = 4
} rnf_controller_family;
/* From a product category / name ("Switch Pro Controller", "DualSense", "Xbox One", "Steam Deck")
 * and an optional vendor name. */
rnf_controller_family rnf_controller_family_from(const char* product_category, const char* vendor_name);
char* rnf_controller_family_title(rnf_controller_family f);
/* Printed label of an element on a family ("" when none); static. */
const char* rnf_controller_family_label(rnf_controller_family f, const char* element);
int rnf_controller_family_is_symmetric(rnf_controller_family f); /* sticks both at the bottom */

/* Apple GameController: positions of buttonA/B/X/Y (index 0..3) from the product category,
 * vendor and the reported glyphs (sfSymbolsName, NULL = not reported). Nintendo-layout pads are
 * placed by glyph when all four are reported and distinct, else by label. */
void rnf_gc_face_positions(const char* product_category, const char* vendor_name, const char* const symbols[4],
                           int out_positions[4]);
/* Micro gamepads: buttonA = south, buttonX = west. */
const char* rnf_gc_label_from_symbol(const char* symbol); /* "A", "✕" ...; NULL if unknown */

/* SDL3 gamepads (values of SDL_GamepadButton / SDL_GamepadAxis / SDL_GamepadType /
 * SDL_GamepadButtonLabel; SDL buttons are already positional). NULL / 0 when not mapped. */
const char* rnf_sdl_button_element(int sdl_gamepad_button);
/* Axis direction: negative = 0 (left / up), positive = 1 (right / down; triggers use 1). */
const char* rnf_sdl_axis_element(int sdl_gamepad_axis, int positive);
rnf_controller_family rnf_sdl_controller_family(int sdl_gamepad_type, const char* name);
const char* rnf_sdl_button_label_text(int sdl_gamepad_button_label);
/* XInput (Windows): one XINPUT_GAMEPAD_* button bit -> element (Xbox layout, positional). */
const char* rnf_xinput_button_element(uint32_t xinput_button_bit);

/* ------------------------------------------------------------------ controller diagram */
#define RNF_DIAGRAM_CANVAS_WIDTH 560.0
#define RNF_DIAGRAM_CANVAS_HEIGHT 262.0
#define RNF_DIAGRAM_STICK_RADIUS 28.0
#define RNF_DIAGRAM_FACE_SPACING 25.0
#define RNF_DIAGRAM_FACE_RADIUS 12.0
#define RNF_DIAGRAM_DPAD_ARM 18.0
typedef enum rnf_diagram_kind {
  RNF_DIAGRAM_FACE = 0, RNF_DIAGRAM_DPAD, RNF_DIAGRAM_STICK_DIRECTION, RNF_DIAGRAM_STICK_CLICK,
  RNF_DIAGRAM_SHOULDER, RNF_DIAGRAM_TRIGGER, RNF_DIAGRAM_SMALL, RNF_DIAGRAM_HOME
} rnf_diagram_kind;
typedef enum rnf_diagram_side { RNF_SIDE_LEFT = 0, RNF_SIDE_RIGHT, RNF_SIDE_ABOVE, RNF_SIDE_BELOW } rnf_diagram_side;
typedef struct rnf_diagram_element {
  const char* element; /* static */
  rnf_diagram_kind kind;
  double cx, cy, width, height;
  rnf_diagram_side badge_side;
  const char* group;     /* "dpad" / "lstick" / "rstick" or NULL */
  double badge_x, badge_y; /* where the assignment badge is anchored */
} rnf_diagram_element;
typedef struct rnf_diagram_info {
  double left_stick_x, left_stick_y, right_stick_x, right_stick_y, stick_radius;
  double dpad_x, dpad_y, face_x, face_y;
  int has_touchpad;
  double touchpad_x, touchpad_y, touchpad_width, touchpad_height;
  double dpad_anchor_x, dpad_anchor_y, lstick_anchor_x, lstick_anchor_y, rstick_anchor_x, rstick_anchor_y;
} rnf_diagram_info;
void rnf_diagram_info_get(rnf_controller_family f, rnf_diagram_info* out);
size_t rnf_diagram_element_count(rnf_controller_family f);
int rnf_diagram_element_get(rnf_controller_family f, size_t index, rnf_diagram_element* out);
/* The outline drawn under the elements, back to front: the body (a rounded pill; the Steam Deck: a wide
 * rounded rectangle), then its screen (Steam Deck) and touch / track pads. No grips. */
typedef enum rnf_diagram_decor_kind {
  RNF_DECOR_BODY = 0, RNF_DECOR_SCREEN = 1, RNF_DECOR_PAD = 2
} rnf_diagram_decor_kind;
typedef struct rnf_diagram_decor {
  rnf_diagram_decor_kind kind;
  double x, y, width, height, radius; /* top-left, size, corner radius (canvas points) */
} rnf_diagram_decor;
size_t rnf_diagram_decor_count(rnf_controller_family f);
int rnf_diagram_decor_get(rnf_controller_family f, size_t index, rnf_diagram_decor* out);

/* Assignment summaries for the diagram. */
/* Actions bound to gc<slot>:<element>, in catalog order (a = action id). */
rnf_list* rnf_input_element_actions(const rnf_binding* bindings, size_t count, const char* element, int slot);
/* "Right Button (A)", "D-pad ↑", "ZL" ... label_override (may be NULL) replaces the family label. */
char* rnf_input_element_title(const char* element, rnf_controller_family f, const char* label_override);
/* Compact badge label: "A", "Turbo A", "Rewind", "2P B" ... */
char* rnf_input_action_short_label(const char* action, int slot);
/* The action picker of a controller button (Settings > Controls > Controller > a button): "" (None)
 * first, then the slot's own player's actions, the other player's, the hotkeys (static ids).
 * Two-call pattern. Shown RNF_MENU_MAX_ITEMS rows per sheet (the menu page "controls.assign"). */
size_t rnf_input_assign_choices(int slot, const char** out, size_t cap);
/* Badge text (actions joined with " · "); NULL when nothing is bound. */
char* rnf_input_element_badge(const rnf_binding* bindings, size_t count, const char* element, int slot);
typedef enum rnf_group_summary { RNF_GROUP_NONE = 0, RNF_GROUP_MOVEMENT = 1, RNF_GROUP_CUSTOM = 2 } rnf_group_summary;
/* "dpad" / "lstick" / "rstick": MOVEMENT ("Move" / "2P Move" in *movement_text) when the four
 * directions map 1:1 to one player's directions; NONE when nothing is bound. */
rnf_group_summary rnf_input_group_summary(const rnf_binding* bindings, size_t count, const char* group, int slot,
                                          char** movement_text);

/* ------------------------------------------------------------------ chord detector (L+R)
 * Two-button combos bound to an action (the Quick Menu's "gc0:leftShoulder+gc0:rightShoulder" ->
 * "hk.menu"). The frontend feeds the presses / releases of the combo members here instead of
 * passing them on; the detector decides:
 *   - both members pressed within the window (either order): one RNF_CHORD_COMBO_DOWN (the frontend
 *     presses the combo id: menu toggle), RNF_CHORD_COMBO_UP once both are released. Neither member
 *     fires alone. Presses while the chord is held (re-pressing one of them) do nothing.
 *   - a member alone: RNF_CHORD_ALONE_DOWN when the window ended while it is held (rnf_chord_tick: it
 *     can no longer become a chord) or at its release when released within the window (a tap: DOWN
 *     and UP together); RNF_CHORD_ALONE_UP on its release. ALONE_UP is the TRIGGER of a single press
 *     (pause, slow, page, frame step ...: docs/design/UI_REDESIGN.md): an L or R press never acts
 *     before it is released, so it never interferes with the chord. ALONE_DOWN only says "held
 *     alone" (a member bound to a game button is pressed from there; a held marker moves).
 *   - repeat (rnf_chord_set_repeat, off by default): a member held alone fires RNF_CHORD_ALONE_REPEAT
 *     at press + RNF_CHORD_HOLD_DELAY and then every RNF_CHORD_REPEAT_INTERVAL until released (paused
 *     frame stepping); its ALONE_UP then carries the number of repeats (the frontend skips the
 *     single action when it is > 0). A chorded member never repeats.
 *   - repeated presses of a held id (key repeat) are ignored.
 * Times are seconds on one monotonic clock (event timestamps). Not thread-safe. */
#define RNF_CHORD_WINDOW 0.100
#define RNF_CHORD_HOLD_DELAY 0.400
#define RNF_CHORD_REPEAT_INTERVAL 0.050
typedef struct rnf_chord rnf_chord;
typedef enum rnf_chord_kind {
  RNF_CHORD_COMBO_DOWN = 0,  /* both down: press the combo id (input = "<a>+<b>") */
  RNF_CHORD_COMBO_UP = 1,    /* both released after COMBO_DOWN: release the combo id */
  RNF_CHORD_ALONE_DOWN = 2,  /* member held alone (or tapped): it is not part of a chord */
  RNF_CHORD_ALONE_UP = 3,    /* that member released: the single press's trigger */
  RNF_CHORD_ALONE_REPEAT = 4 /* repeat mode: held alone >= RNF_CHORD_HOLD_DELAY (and every interval) */
} rnf_chord_kind;
typedef struct rnf_chord_event {
  rnf_chord_kind kind;
  const char* input; /* valid until the next call on the handle */
  int member;        /* ALONE_*: 0 = the combo's first id (L), 1 = the second (R); COMBO_*: -1 */
  double time;       /* when it happened (the release, press + window for a held member, the repeat) */
  int repeats;       /* ALONE_UP / ALONE_REPEAT: repeats fired during this hold so far (incl. this one) */
} rnf_chord_event;
rnf_chord* rnf_chord_new(double window); /* <= 0: RNF_CHORD_WINDOW */
void rnf_chord_free(rnf_chord* c);
/* Combos from a binding table: every binding of `action` (NULL: "hk.menu") whose input is a combo id.
 * Replaces the previous combos and drops all state. Returns the number of combos. */
size_t rnf_chord_configure(rnf_chord* c, const rnf_binding* bindings, size_t count, const char* action);
/* Adds one combo; returns its index (-1: bad ids or a member already used). */
int rnf_chord_add(rnf_chord* c, const char* a, const char* b);
size_t rnf_chord_combo_count(const rnf_chord* c);
int rnf_chord_is_member(const rnf_chord* c, const char* physical_id);
/* A press (1) / release (0) of physical_id at time t. Returns 1 when the id is a combo member (the event
 * is taken: its outcome comes from rnf_chord_poll), 0 otherwise (pass it on as usual). */
int rnf_chord_feed(rnf_chord* c, const char* physical_id, int pressed, double t);
/* Fires the members held alone for the whole window and the repeats (call every frame / poll, with the
 * current time). */
void rnf_chord_tick(rnf_chord* c, double t);
/* Earliest time a tick can fire something (0: nothing pending). */
double rnf_chord_deadline(const rnf_chord* c);
/* Next event (1) or none (0). */
int rnf_chord_poll(rnf_chord* c, rnf_chord_event* out);
/* Forgets everything held / pending without events (controller disconnected, focus lost). */
void rnf_chord_reset(rnf_chord* c);
/* Repeat mode on / off (members held alone fire ALONE_REPEAT). Turning it on while a member is held
 * counts its hold from its press. */
void rnf_chord_set_repeat(rnf_chord* c, int on);
int rnf_chord_repeat(const rnf_chord* c);

/* ------------------------------------------------------------------ UI confirm / cancel
 * Menus, dialogs, the on-screen keyboard, prompts and the paused seek bar confirm with the EAST face
 * button and cancel / go back with the SOUTH one by default (Nintendo style, on every controller
 * family); the setting "Confirm Button" (south_confirm = 1) swaps them. Game input never changes. */
const char* rnf_ui_confirm_element(int south_confirm); /* "face.east" (default) / "face.south" */
const char* rnf_ui_cancel_element(int south_confirm);  /* "face.south" (default) / "face.east" */

/* ------------------------------------------------------------------ hold speed
 * Rewind (L2 held), fast-forward (R2 held) and a seek bar marker moved with L / R held move by the
 * same number of frames per 60 Hz tick, in opposite directions: rnf_hold_speed(n) for the n-th tick
 * of a hold (n >= 1): 2 for the first RNF_HOLD_SPEED_FAST_AFTER ticks, 3 until
 * RNF_HOLD_SPEED_FASTEST_AFTER, then 4. */
#define RNF_HOLD_SPEED_FAST_AFTER 30
#define RNF_HOLD_SPEED_FASTEST_AFTER 90
int rnf_hold_speed(int tick);

/* ================================================================== quick menu model
 * The menu tree of docs/design/UI_REDESIGN.md, shared by every frontend: the Quick Menu (six tiles in
 * one row), the pages behind them, the four Settings pages and their detail pages, and the navigation
 * state (back stack with each level's focus, breadcrumb, L / R page switching). No page scrolls: a page
 * holds at most RNF_MENU_MAX_ITEMS items (the Practice page's A/B slots: RNF_MENU_MAX_CARDS cards, 4 x 2);
 * user data (takes, bookmarks, key bindings, projects) are LIST pages whose rows the frontend counts
 * (rnf_menu_set_count) and that show RNF_MENU_MAX_ITEMS rows per sheet (L / R or moving past the end
 * switch sheets). Labels / descriptions are localization keys (rnf_menu_text); icons are Tabler Icons
 * names (SF Symbols on macOS are the frontend's mapping). Values and enabled states are the frontend's.
 * Not thread-safe. */
#define RNF_MENU_MAX_ITEMS 6
#define RNF_MENU_MAX_CARDS 8
#define RNF_MENU_MAX_DEPTH 4 /* Quick Menu > Settings > Display > CRT details */
typedef enum rnf_menu_feature {
  RNF_MENU_FEATURE_EXPORT = 1u << 0,      /* Share > Export MP4 */
  RNF_MENU_FEATURE_STREAM = 1u << 1,      /* Share > Stream output (Syphon / Spout) */
  RNF_MENU_FEATURE_CRT = 1u << 2,         /* Display > CRT + CRT details */
  RNF_MENU_FEATURE_STEAM = 1u << 3,       /* System > Add to Steam */
  RNF_MENU_FEATURE_OSK = 1u << 4,         /* Controls > On-screen keyboard */
  RNF_MENU_FEATURE_UPDATES = 1u << 5,     /* System > Updates */
  RNF_MENU_FEATURE_SLOW_AUDIO = 1u << 6,  /* Sound > Sound in slow motion */
  RNF_MENU_FEATURE_LOW_LATENCY = 1u << 7, /* Sound > Low latency */
  RNF_MENU_FEATURE_QUIT = 1u << 8,        /* System > Quit */
  RNF_MENU_FEATURE_FULLSCREEN = 1u << 9,  /* System > More > Full screen */
  RNF_MENU_FEATURE_UI_SCALE = 1u << 10    /* System > More > UI size */
} rnf_menu_feature;
typedef enum rnf_menu_page_kind {
  RNF_MENU_PAGE_TILES = 0,    /* one row of large icon tiles (Quick Menu, Retry, Share, Game) */
  RNF_MENU_PAGE_SETTINGS = 1, /* rows: label + value (up / down move, left / right adjust) */
  RNF_MENU_PAGE_CARDS = 2,    /* a grid of cards (Practice: 4 x 2) */
  RNF_MENU_PAGE_LIST = 3,     /* rows of frontend data, RNF_MENU_MAX_ITEMS per sheet */
  RNF_MENU_PAGE_CUSTOM = 4    /* drawn and navigated by the frontend (controller diagram) */
} rnf_menu_page_kind;
typedef enum rnf_menu_item_kind {
  RNF_MENU_ITEM_RESUME = 0, /* closes the menu */
  RNF_MENU_ITEM_ACTION = 1, /* the frontend performs it */
  RNF_MENU_ITEM_PAGE = 2,   /* opens `target` */
  RNF_MENU_ITEM_TOGGLE = 3, /* on / off (A flips, left / right set) */
  RNF_MENU_ITEM_CHOICE = 4, /* one of `choice_count` values (left / right) */
  RNF_MENU_ITEM_SLIDER = 5, /* a number (left / right; range is the frontend's) */
  RNF_MENU_ITEM_INFO = 6    /* read-only text */
} rnf_menu_item_kind;
typedef struct rnf_menu_page_info {
  const char* id;    /* "quick", "retry", "settings.display" ... (static) */
  const char* title; /* localization key */
  const char* icon;
  rnf_menu_page_kind kind;
  int columns;       /* grid columns (TILES: the item count; SETTINGS / LIST: 1; CARDS: 4) */
  size_t count;      /* items / cards / rows */
  const char* group; /* "settings": L / R switch between the group's pages; "" none */
} rnf_menu_page_info;
typedef struct rnf_menu_item_info {
  const char* id;          /* "resume", "display.crt" ... (static) */
  const char* label;       /* localization key */
  const char* description; /* localization key of the one line at the bottom ("" none) */
  const char* icon;        /* Tabler Icons name ("" none) */
  rnf_menu_item_kind kind;
  const char* target;      /* PAGE: the page id; else "" */
  size_t choice_count;     /* CHOICE */
} rnf_menu_item_info;
typedef struct rnf_menu_crumb {
  const char* label; /* localization key */
  const char* icon;
} rnf_menu_crumb;
typedef enum rnf_menu_event {
  RNF_MENU_EVENT_NONE = 0,     /* nothing (edge of a page, nothing to adjust) */
  RNF_MENU_EVENT_MOVED = 1,    /* the focus moved */
  RNF_MENU_EVENT_PUSHED = 2,   /* a page opened (its first item focused) */
  RNF_MENU_EVENT_POPPED = 3,   /* back to the parent page (its focus restored) */
  RNF_MENU_EVENT_SWITCHED = 4, /* L / R: the group's next page, or a LIST's next sheet */
  RNF_MENU_EVENT_ACTIVATE = 5, /* the frontend performs the focused item (ACTION / TOGGLE / CARDS / LIST / CUSTOM) */
  RNF_MENU_EVENT_ADJUST = 6,   /* left / right on a TOGGLE / CHOICE / SLIDER (or a LIST row): *dir = -1 / +1 */
  RNF_MENU_EVENT_CLOSE = 7     /* leave the menu: Resume, or back on the root page */
} rnf_menu_event;
typedef struct rnf_menu rnf_menu;
rnf_menu* rnf_menu_new(uint32_t features);
void rnf_menu_free(rnf_menu* m);
size_t rnf_menu_page_count(const rnf_menu* m);
int rnf_menu_page_get(const rnf_menu* m, size_t page, rnf_menu_page_info* out);
int rnf_menu_page_find(const rnf_menu* m, const char* id); /* index, -1 if none (or left out by features) */
int rnf_menu_item_get(const rnf_menu* m, size_t page, size_t item, rnf_menu_item_info* out); /* not LIST / CARDS rows */
int rnf_menu_item_find(const rnf_menu* m, size_t page, const char* id); /* index, -1 if none */
/* Localization key of choice `choice` of a CHOICE item; NULL out of range. Keys starting with "=" are
 * literal text (language names): rnf_menu_text shows them without lookup. */
const char* rnf_menu_item_choice(const rnf_menu* m, size_t page, size_t item, size_t choice);
/* LIST / CARDS pages: the number of rows / cards (the frontend's data). Keeps the focus in range. */
void rnf_menu_set_count(rnf_menu* m, size_t page, size_t count);
/* Navigation. rnf_menu_open: a new stack with `root_page` (NULL: "quick"), its first item focused. */
int rnf_menu_open(rnf_menu* m, const char* root_page);
void rnf_menu_close(rnf_menu* m);
size_t rnf_menu_depth(const rnf_menu* m);                    /* 0 = closed */
size_t rnf_menu_level_page(const rnf_menu* m, size_t level); /* page index of a stack level (0 = root) */
size_t rnf_menu_current(const rnf_menu* m);                  /* page index of the top */
size_t rnf_menu_focus(const rnf_menu* m);                    /* focused item (LIST: the absolute row) */
void rnf_menu_set_focus(rnf_menu* m, size_t item);           /* pointer hover / tap; clamped */
size_t rnf_menu_sheet(const rnf_menu* m);                    /* LIST: sheet of the focus; else 0 */
size_t rnf_menu_sheet_count(const rnf_menu* m);              /* LIST: sheets (>= 1); else 1 */
rnf_menu_event rnf_menu_move(rnf_menu* m, int dx, int dy, int* adjust_dir);
rnf_menu_event rnf_menu_confirm(rnf_menu* m);
rnf_menu_event rnf_menu_back(rnf_menu* m);
rnf_menu_event rnf_menu_switch(rnf_menu* m, int dir); /* L (-1) / R (+1) */
rnf_menu_event rnf_menu_push(rnf_menu* m, const char* page_id);
/* Breadcrumb of the open stack (the root page left out; a group's title before its first page). */
size_t rnf_menu_breadcrumb(const rnf_menu* m, rnf_menu_crumb* out, size_t cap);
/* A click / tap on breadcrumb segment `crumb` (index as rnf_menu_breadcrumb returns them): back to
 * that level (its focus restored, like rnf_menu_back repeated). A group's title ("Settings") goes to
 * the group's first page (what its Quick Menu tile opens), focus on its first item. POPPED / SWITCHED;
 * NONE for the current page (the last segment) or an index out of range. Pointer only: controllers
 * go back with the back button. */
rnf_menu_event rnf_menu_crumb_select(rnf_menu* m, size_t crumb);
/* The UI string of a key: rnf_l10n_lookup, or the literal after a leading "=". */
const char* rnf_menu_text(const char* key);

/* ================================================================== session resume
 * Always-on session persistence. A session folder holds the temporary project of a session
 * without a project, the resume record and the single-instance lock. */
#define RNF_SESSION_TEMP_PROJECT "current.nesrec"
#define RNF_SESSION_RESUME_FILE "resume.json"
#define RNF_SESSION_LOCK_FILE ".lock"
#define RNF_RESUME_VERSION 1

typedef enum rnf_resume_mode { RNF_RESUME_MODE_NONE = -1, RNF_RESUME_MODE_RECORD = 0, RNF_RESUME_MODE_REPLAY = 1 } rnf_resume_mode;
typedef struct rnf_resume_record {
  int version;
  const char* project_path;
  int is_temp;
  const char* rom_path;
  const char* rom_sha256;
  int has_frame;
  uint64_t frame;             /* take position (has_frame = 0: keep the project's cursor) */
  int has_at_take_end;
  int at_take_end;            /* resume at the end of what the project holds */
  rnf_resume_mode mode;       /* NONE: keep the project's own mode */
  int has_practice_slot;
  int practice_slot;          /* -1 = free practice; present = reopen the practice panel */
  int has_content;            /* anything recorded */
  double updated;             /* seconds since 1970 (stored as ISO 8601, whole seconds) */
} rnf_resume_record;
/* Strings of a record filled by this library are heap copies: release them with
 * rnf_resume_record_clear (which zeroes the record). */
void rnf_resume_record_clear(rnf_resume_record* r);
/* Equal apart from the timestamp. */
int rnf_resume_same_state(const rnf_resume_record* a, const rnf_resume_record* b);
/* *found = 0 if there is no record. RN_ERR_CORRUPT / RN_ERR_IO if it exists but cannot be read. */
rn_status rnf_resume_read(const char* path, rnf_resume_record* out, int* found);
/* Atomic (temp file + rename). */
rn_status rnf_resume_write(const char* path, const rnf_resume_record* r);
void rnf_resume_clear(const char* path);

typedef enum rnf_resume_decision {
  RNF_RESUME_NONE = 0, RNF_RESUME_RESUME = 1,
  RNF_RESUME_PROJECT_MISSING = 2 /* the recorded (non-temporary) project is gone */
} rnf_resume_decision;
/* Launch-time decision for session folder session_root. explicit_open: the launch opens
 * something itself. legacy_project_path (may be NULL): crash marker of old versions. *out is
 * filled for RESUME / PROJECT_MISSING (clear it with rnf_resume_record_clear). */
rnf_resume_decision rnf_resume_decide(const char* session_root, int explicit_open, const char* legacy_project_path,
                                      rnf_resume_record* out);
/* Take position to go to (recorded frame clamped to take_length; take end when at_take_end).
 * Returns 0 when the project's own cursor is kept. */
int rnf_resume_target_frame(const rnf_resume_record* r, uint64_t take_length, uint64_t* out);
/* Restores take mode and position on a freshly opened session (no-op when already there). */
rn_status rnf_resume_apply(const rnf_resume_record* r, rn_session* s);
/* Takes with frames, bookmarks or A/B slots: worth asking before a temporary project is discarded. */
int rnf_session_has_recorded_content(rn_session* s);
/* Same file system location (symlinks resolved, trailing separators ignored). */
int rnf_paths_equal(const char* a, const char* b);

/* Exclusive, non-blocking lock held until release (or process exit). NULL if held elsewhere. */
typedef struct rnf_session_lock rnf_session_lock;
rnf_session_lock* rnf_session_lock_acquire(const char* lock_file);
void rnf_session_lock_release(rnf_session_lock* l);

/* ================================================================== timeline
 * Positions are CURSOR frames (rn_frame values 0...take length). */
double rnf_timeline_x_for_frame(double width, uint64_t length, uint64_t frame);
uint64_t rnf_timeline_frame_at_x(double width, uint64_t length, double x); /* nearest, clamped */

typedef struct rnf_timeline_range {
  int slot;
  uint64_t a;
  int has_b;
  uint64_t b;
} rnf_timeline_range;
typedef enum rnf_timeline_handle { RNF_HANDLE_A = 0, RNF_HANDLE_B = 1 } rnf_timeline_handle;
typedef enum rnf_timeline_hit_kind { RNF_HIT_NONE = 0, RNF_HIT_HANDLE = 1, RNF_HIT_BODY = 2 } rnf_timeline_hit_kind;
typedef struct rnf_timeline_hit {
  rnf_timeline_hit_kind kind;
  int slot;
  rnf_timeline_handle handle;
} rnf_timeline_hit;
/* Range selected by dragging x0 -> x1 (either direction). 0 if shorter than one frame. */
int rnf_timeline_range_from_drag(double width, uint64_t length, double x0, double x1, uint64_t* a, uint64_t* b);
/* Handles within tolerance win over bodies; closest edge, then the preferred slot, then the
 * range drawn last. */
rnf_timeline_hit rnf_timeline_hit_test(double x, const rnf_timeline_range* ranges, size_t count, double width,
                                       uint64_t length, double tolerance, int has_preferred, int preferred);
/* Moves one handle of [a, b) to x keeping a < b inside 0...length. */
void rnf_timeline_drag(rnf_timeline_handle handle, uint64_t a, uint64_t b, double x, double width, uint64_t length,
                       uint64_t* out_a, uint64_t* out_b);
typedef enum rnf_mark_plan { RNF_MARK_SET_RANGE = 0, RNF_MARK_SET_A_ONLY = 1, RNF_MARK_INVALID = 2 } rnf_mark_plan;
/* "Set A Here" / "Set B Here" at cursor f. existing: the slot as visible on the active take
 * (NULL if empty or elsewhere). INVALID: *message (localized) explains. */
rnf_mark_plan rnf_timeline_mark_a(uint64_t f, const rnf_timeline_range* existing, uint64_t* a, uint64_t* b,
                                  char** message);
rnf_mark_plan rnf_timeline_mark_b(uint64_t f, const rnf_timeline_range* existing, uint64_t* a, uint64_t* b,
                                  char** message);
/* Leading frames two takes have in common (unknown takes share nothing). */
uint64_t rnf_take_shared_prefix(const rn_take_info* takes, size_t count, uint64_t x, uint64_t y);
typedef struct rnf_practice_slot {
  int index;
  int has_a, has_b, has_take_frame;
  uint64_t take_frame, length, take_id;
} rnf_practice_slot;
/* Slots whose A (and B) lie on the active take. out_cap >= slot_count is always enough. */
size_t rnf_timeline_visible_ranges(const rnf_practice_slot* slots, size_t slot_count, const rn_take_info* takes,
                                   size_t take_count, uint64_t active_take, uint64_t take_length,
                                   rnf_timeline_range* out, size_t out_cap);

/* ------------------------------------------------------------------ seek bar A/B markers
 * The paused seek bar's markers (docs/design/UI_REDESIGN.md, "Seek bar while paused"): the controller's
 * view of the SELECTED practice slot's section on the active take - none (empty slot, or its A is on
 * another take), one (A only) or two (the left one is A, the right one B). Focus is the seek bar
 * (-1: D-pad steps the playhead, confirm drops a marker there) or a marker (0 / 1, left to right:
 * D-pad up from the bar picks the one nearest the playhead, left / right pick, down / cancel go back;
 * X deletes it). Confirm on a marker edits it: D-pad / L / R held move it (the picture follows it:
 * seek preview), it skips over the other marker's frame and the two swap roles when it passes it;
 * confirm commits (focus back to the bar, the playhead stays at the marker), cancel reverts it (and
 * the playhead). Writes: one marker = "A only" at its frame; two = the range [left, right); deleting
 * one of two leaves "A only" at the other; deleting the last clears the slot. The frontend applies the
 * results (writes the slot, seeks) and re-syncs the slot every frame. Pure; not thread-safe. */
typedef struct rnf_markers rnf_markers;
typedef enum rnf_markers_outcome {
  RNF_MARKERS_IGNORED = 0,  /* not the markers' input in this state: it keeps its usual meaning */
  RNF_MARKERS_CHANGED = 1,  /* handled (focus / edit / markers changed) */
  RNF_MARKERS_FULL = 2,     /* confirm on the bar with two markers: nothing (a subtle hint) */
  RNF_MARKERS_OCCUPIED = 3  /* confirm on the bar where a marker already is: nothing (hint) */
} rnf_markers_outcome;
typedef enum rnf_markers_write {
  RNF_MARKERS_WRITE_NONE = 0,
  RNF_MARKERS_WRITE_A_ONLY = 1, /* the slot = A at `a`, no B */
  RNF_MARKERS_WRITE_RANGE = 2,  /* the slot = [a, b) (rn_practice_set_range) */
  RNF_MARKERS_WRITE_CLEAR = 3   /* the slot is emptied */
} rnf_markers_write;
typedef struct rnf_markers_result {
  rnf_markers_outcome outcome;
  int seek;            /* 1: show seek_frame (the edited marker / back to the playhead) */
  uint64_t seek_frame;
  rnf_markers_write write;
  uint64_t a, b;
} rnf_markers_result;
rnf_markers* rnf_markers_new(void);
void rnf_markers_free(rnf_markers* m);
/* The selected slot as visible on the active take (NULL: none) and the take length. Ignored while a
 * marker is being edited; keeps the focus valid. */
void rnf_markers_sync(rnf_markers* m, const rnf_timeline_range* slot, uint64_t take_length);
size_t rnf_markers_count(const rnf_markers* m);
uint64_t rnf_markers_frame(const rnf_markers* m, size_t i); /* left to right (0 = A) */
int rnf_markers_focus(const rnf_markers* m);                /* -1: the seek bar; else the marker */
int rnf_markers_editing(const rnf_markers* m);
rnf_markers_result rnf_markers_confirm(rnf_markers* m, uint64_t playhead);
rnf_markers_result rnf_markers_cancel(rnf_markers* m);
/* D-pad: dx / dy -1 / +1 (up = dy -1). Editing: dx moves the marker one frame. */
rnf_markers_result rnf_markers_move(rnf_markers* m, int dx, int dy, uint64_t playhead);
/* Editing: moves the marker by `frames` (L / R held: -+rnf_hold_speed). */
rnf_markers_result rnf_markers_nudge(rnf_markers* m, int64_t frames);
rnf_markers_result rnf_markers_delete(rnf_markers* m);
/* Leaving the paused seek bar (resume, the menu): an edit is reverted, the focus goes back to the bar. */
rnf_markers_result rnf_markers_leave(rnf_markers* m);
/* The paused seek bar's Y (north) and X (west) taps:
 *   Y: practice the selected slot from its A (RNF_SEEK_FACE_PRACTICE) when it has a section on this
 *      take (one or two markers: A only = practice without a loop) and no marker is being edited;
 *   X: delete the focused marker (rnf_markers_delete), or - focus on the bar - the next A/B slot. */
typedef enum rnf_seek_face_action {
  RNF_SEEK_FACE_NONE = 0, RNF_SEEK_FACE_NEXT_SLOT = 1, RNF_SEEK_FACE_DELETE_MARKER = 2, RNF_SEEK_FACE_PRACTICE = 3
} rnf_seek_face_action;
rnf_seek_face_action rnf_markers_face(const rnf_markers* m, int north);

/* ------------------------------------------------------------------ filmstrip grid
 * One tile = F frames at its natural width, F = 300 * 2^e (5 s, coarser grids are subsets). */
#define RNF_THUMB_BASE_STEP 300.0
#define RNF_THUMB_MIN_EXPONENT 0
#define RNF_THUMB_MAX_EXPONENT 40
#define RNF_THUMB_START_FRAME 60
#define RNF_THUMB_FALLBACK_OPACITY 0.45
double rnf_thumb_step(int exponent);
int rnf_thumb_is_step(double f);
uint64_t rnf_thumb_frame(uint64_t k, double step);
size_t rnf_thumb_frames(uint64_t length, double step, uint64_t* out, size_t cap); /* F, 2F ... <= length */
int rnf_thumb_is_grid_frame(uint64_t x, double step);
uint64_t rnf_thumb_start_picture(double step);
uint64_t rnf_thumb_picture_frame(uint64_t tile, double step);
size_t rnf_thumb_targets(uint64_t length, double step, uint64_t* out, size_t cap); /* sorted pictures */
double rnf_thumb_extent(uint64_t length, double step, double tile_width);
double rnf_thumb_tile_step(double width, double tile_width, uint64_t length);
typedef struct rnf_thumb_tile {
  uint64_t index, frame, picture;
  double x, visible;
} rnf_thumb_tile;
size_t rnf_thumb_tiles(uint64_t length, double step, double tile_width, rnf_thumb_tile* out, size_t cap);
double rnf_thumb_snap_to_pixel(double x, double scale);
uint64_t rnf_thumb_fallback_window(double step);

/* 256x240 BGRA (rn_video) -> 128x120 by 2x2 box averaging (opaque alpha). */
#define RNF_THUMB_FACTOR 2
#define RNF_THUMB_WIDTH 128
#define RNF_THUMB_HEIGHT 120
void rnf_thumb_downscale(const uint32_t* src, uint32_t* dst);

/* Thumbnail cache of the active take keyed by cursor frame. Thread-safe. Payloads are opaque
 * frontend images: the cache calls retain() when it stores one and release() when it drops it;
 * lookups return a payload already retained for the caller. on_change runs after every change on
 * the calling thread, outside the lock. */
typedef struct rnf_thumb_cache rnf_thumb_cache;
typedef void (*rnf_payload_fn)(void* payload);
typedef void (*rnf_change_fn)(void* ctx);
rnf_thumb_cache* rnf_thumb_cache_new(rnf_payload_fn retain, rnf_payload_fn release);
void rnf_thumb_cache_free(rnf_thumb_cache* c);
void rnf_thumb_cache_set_on_change(rnf_thumb_cache* c, rnf_change_fn fn, void* ctx);
uint64_t rnf_thumb_cache_take(rnf_thumb_cache* c);
uint64_t rnf_thumb_cache_generation(rnf_thumb_cache* c); /* bumped on reset / take change */
uint64_t rnf_thumb_cache_version(rnf_thumb_cache* c);    /* bumped on every change */
size_t rnf_thumb_cache_count(rnf_thumb_cache* c);
double rnf_thumb_cache_step(rnf_thumb_cache* c);
size_t rnf_thumb_cache_capacity(rnf_thumb_cache* c);
void rnf_thumb_cache_set_capacity(rnf_thumb_cache* c, size_t capacity); /* default 600 */
void rnf_thumb_cache_reset(rnf_thumb_cache* c, uint64_t take);
/* Active take changed to take; frames <= keep_through (shared prefix) stay. */
void rnf_thumb_cache_rebase(rnf_thumb_cache* c, uint64_t take, uint64_t keep_through);
void rnf_thumb_cache_set_step(rnf_thumb_cache* c, double step);
int rnf_thumb_cache_wants(rnf_thumb_cache* c, uint64_t frame, uint64_t take);
int rnf_thumb_cache_insert(rnf_thumb_cache* c, uint64_t frame, uint64_t take, int has_generation, uint64_t generation,
                           void* payload);
int rnf_thumb_cache_insert_batch(rnf_thumb_cache* c, const uint64_t* frames, void* const* payloads, size_t count,
                                 uint64_t take, uint64_t generation);
int rnf_thumb_cache_contains(rnf_thumb_cache* c, uint64_t frame);
size_t rnf_thumb_cache_missing(rnf_thumb_cache* c, const uint64_t* frames, size_t count, uint64_t* out, size_t cap);
void* rnf_thumb_cache_image_at(rnf_thumb_cache* c, uint64_t frame);                      /* retained or NULL */
void* rnf_thumb_cache_image_before(rnf_thumb_cache* c, uint64_t frame, uint64_t window); /* [f-window, f) */

/* ================================================================== ROM library
 * <root>/ROM: the user's .nes files (top level + one level of sub-folders);
 * <root>/Projects: "<ROM name> <yyyy-MM-dd HHmm>.nesrec", matched to ROMs by SHA-256. */
#define RNF_LIBRARY_ROM_DIR "ROM"
#define RNF_LIBRARY_PROJECTS_DIR "Projects"
/* Creates ROM/ and Projects/ if missing. RN_ERR_ALREADY_EXISTS: a file is where a folder should
 * be; RN_ERR_IO: cannot create. *failed_path (may be NULL) receives the folder concerned. */
rn_status rnf_library_ensure(const char* root, char** failed_path);

typedef int (*rnf_name_compare)(const char* a, const char* b, void* ctx); /* <0, 0, >0 */
typedef struct rnf_rom_entry {
  const char* path;
  const char* name;          /* file name without extension */
  const char* relative_path; /* relative to ROM/ ("Sub/Game.nes") */
  int64_t size;
  double modified;
} rnf_rom_entry;
typedef struct rnf_rom_list rnf_rom_list;
/* .nes files (case-insensitive) in dir and its sub-folders (hidden files skipped), sorted by name
 * with compare (NULL: case-insensitive natural order), then relative path. RN_ERR_IO if dir
 * cannot be read (unreadable sub-folders are skipped). */
rn_status rnf_library_scan_roms(const char* dir, rnf_name_compare compare, void* ctx, rnf_rom_list** out);
size_t rnf_rom_list_count(const rnf_rom_list* l);
int rnf_rom_list_get(const rnf_rom_list* l, size_t index, rnf_rom_entry* out); /* valid until free */
void rnf_rom_list_free(rnf_rom_list* l);

typedef struct rnf_project_entry {
  const char* path;
  const char* name; /* without .nesrec */
  const char* rom_sha256; /* lowercase */
  const char* rom_name;
  double modified; /* last save (manifest.json date) */
} rnf_project_entry;
typedef struct rnf_project_list rnf_project_list;
/* *.nesrec packages directly in dir with a readable manifest, newest first. */
rn_status rnf_library_scan_projects(const char* dir, rnf_project_list** out);
size_t rnf_project_list_count(const rnf_project_list* l);
int rnf_project_list_get(const rnf_project_list* l, size_t index, rnf_project_entry* out);
void rnf_project_list_free(rnf_project_list* l);

char* rnf_library_sanitize(const char* rom_name);  /* file-name-safe, never empty */
char* rnf_library_timestamp(double date);          /* "yyyy-MM-dd HHmm", local time */
/* <projects_dir>/<ROM name> <timestamp>.nesrec, " 2", " 3" ... appended if taken. */
char* rnf_library_new_project_path(const char* projects_dir, const char* rom_name, double date);

/* SHA-256 of ROM files cached by path + size + modification date. Thread-safe. */
typedef struct rnf_rom_hash_cache rnf_rom_hash_cache;
rnf_rom_hash_cache* rnf_rom_hash_cache_new(void);
void rnf_rom_hash_cache_free(rnf_rom_hash_cache* c);
rn_status rnf_rom_hash_cache_sha256(rnf_rom_hash_cache* c, const char* path, int64_t size, double modified,
                                    char out_hex[65]); /* lowercase */

/* "Reset Project" backup: "<name> (Before Reset yyyy-MM-dd HH.mm).<ext>" next to the project,
 * " 2", " 3" ... appended while exists(candidate) (NULL: the file system). */
typedef int (*rnf_exists_fn)(const char* path, void* ctx);
char* rnf_backup_path(const char* project_path, double date, rnf_exists_fn exists, void* ctx);

/* ================================================================== game database
 * Offline catalogue of NES / Famicom games compiled into this library (frontend/data/nesdb.tsv,
 * generated by tools/nesdb/build_nesdb.py from Wikidata (CC0) + frontend/data/nesdb-overrides.json;
 * titles and metadata only). A ROM is matched by the CRC32 / SHA-1 of its PRG+CHR data (iNES
 * header and trainer excluded, as Nestopia's NstDatabase.xml defines them; the whole-file CRC32 is
 * tried too), then by its file name ("Super Mario Bros. (World)") against titles and aliases.
 * Game info strings are static (valid for the life of the process). Thread-safe. */
typedef enum rnf_genre {
  RNF_GENRE_UNKNOWN = 0, RNF_GENRE_ACTION = 1, RNF_GENRE_SHOOTER = 2, RNF_GENRE_PUZZLE = 3, RNF_GENRE_RPG = 4,
  RNF_GENRE_ADVENTURE = 5, RNF_GENRE_SPORTS = 6, RNF_GENRE_RACING = 7, RNF_GENRE_FIGHTING = 8, RNF_GENRE_STRATEGY = 9,
  RNF_GENRE_TABLE = 10, RNF_GENRE_MUSIC = 11, RNF_GENRE_EDUCATIONAL = 12, RNF_GENRE_OTHER = 13, RNF_GENRE_COUNT = 14
} rnf_genre;
typedef struct rnf_game_info {
  const char* id;           /* Wikidata Q-id ("Q11168") or "x-<slug>" for games added by hand */
  const char* title_en;
  const char* title_ja;     /* the English title when there is no Japanese one */
  const char* reading;      /* katakana reading of the Japanese title ("" = unknown) */
  const char* publisher_en; /* "" = unknown */
  const char* publisher_ja;
  int year;                 /* first release; 0 = unknown */
  rnf_genre genre;
  const char* region;       /* "JP", "JP,NA" ... ("" = unknown) */
} rnf_game_info;
typedef enum rnf_game_match { RNF_GAME_MATCH_NONE = 0, RNF_GAME_MATCH_HASH = 1, RNF_GAME_MATCH_NAME = 2 } rnf_game_match;

size_t rnf_gamedb_count(void);
int rnf_gamedb_get(size_t index, rnf_game_info* out);
int rnf_gamedb_find_id(const char* id, rnf_game_info* out);
/* By the PRG+CHR hashes (sha1: 40 hex digits, any case). */
int rnf_gamedb_find_crc32(uint32_t crc, rnf_game_info* out);
int rnf_gamedb_find_sha1(const char* sha1_hex, rnf_game_info* out);
/* By a ROM file name (with or without the extension; tags such as "(Japan)" / "[!]" ignored, a
 * region tag prefers the game released there). */
int rnf_gamedb_find_name(const char* file_name, rnf_game_info* out);
/* Identifies a ROM image (iNES / NES 2.0 with or without a trainer, or headerless): hashes first,
 * then file_name (may be NULL). */
rnf_game_match rnf_gamedb_identify(const uint8_t* data, size_t size, const char* file_name, rnf_game_info* out);
/* Reads path (at most 8 MiB) and identifies it by content and file name. */
rnf_game_match rnf_gamedb_identify_file(const char* path, rnf_game_info* out);
/* rnf_gamedb_identify_file cached by path + size + modification date (rnf_rom_hash_cache). */
rnf_game_match rnf_rom_hash_cache_identify(rnf_rom_hash_cache* c, const char* path, int64_t size, double modified,
                                           rnf_game_info* out);
/* CRC32 / SHA-1 (uppercase hex) of the PRG+CHR data of a ROM image (whole data when headerless). */
uint32_t rnf_rom_data_crc32(const uint8_t* data, size_t size);
void rnf_rom_data_sha1(const uint8_t* data, size_t size, char out_hex[41]);

const char* rnf_genre_code(rnf_genre g);           /* "action", "shooter" ... ("" = unknown) */
rnf_genre rnf_genre_from_code(const char* code);
const char* rnf_genre_name(rnf_genre g);           /* localized; "" = unknown */

/* Display strings in the current language (heap strings). info may be NULL (a ROM not in the
 * database): the title is then made from the file name ("Gradius (Japan).nes" -> "Gradius"). */
char* rnf_game_title(const rnf_game_info* info, const char* file_name);
char* rnf_game_publisher(const rnf_game_info* info);           /* "" = unknown */
/* "Konami · 1986" (publisher and year; either may be missing; "" when both are). */
char* rnf_game_byline(const rnf_game_info* info);
/* "Konami · 1986 · Shooter" (the one-line description of a focused game). */
char* rnf_game_details(const rnf_game_info* info);
/* Title made from a file name: extension and trailing "(...)" / "[...]" tags removed, ", The" moved. */
char* rnf_title_from_file_name(const char* file_name);

/* Collation of titles for the given language ("ja": gojūon order of the kana reading with
 * dakuten / handakuten and small kana folded, long vowel marks read as the preceding vowel, kana
 * before Latin before kanji; "en": case-insensitive, natural number order, leading "The " ignored).
 * <0, 0, >0. */
int rnf_collate(const char* a, const char* b, const char* lang);
/* Hiragana primary sort key of Japanese text (tests / debugging): "フロントライン" -> "ふろんとらいん",
 * "スーパー" -> "すうぱあ" with marks folded -> "すうはあ". */
char* rnf_kana_sort_key(const char* text);

/* ================================================================== library catalog
 * Favourites, play history and the library order (sort / filter / search) shared by all frontends.
 * Persisted as <settings dir>/library.json (portable JSON). ROMs are keyed by their SHA-256. */
#define RNF_LIBRARY_PREFS_FILE "library.json"
#define RNF_LIBRARY_HISTORY_MAX 100 /* history entries kept */
#define RNF_LIBRARY_RECENT_COUNT 24 /* games shown by the "Recent" filter */
typedef enum rnf_library_sort {
  RNF_LIBRARY_SORT_NAME = 0, RNF_LIBRARY_SORT_RECENT = 1, RNF_LIBRARY_SORT_PUBLISHER = 2, RNF_LIBRARY_SORT_YEAR = 3,
  RNF_LIBRARY_SORT_GENRE = 4, RNF_LIBRARY_SORT_COUNT = 5
} rnf_library_sort;
typedef enum rnf_library_filter {
  RNF_LIBRARY_FILTER_ALL = 0, RNF_LIBRARY_FILTER_FAVORITES = 1, RNF_LIBRARY_FILTER_RECENT = 2, RNF_LIBRARY_FILTER_COUNT = 3
} rnf_library_filter;
const char* rnf_library_sort_name(rnf_library_sort s);     /* localized: "Name", "Recent" ... */
const char* rnf_library_filter_name(rnf_library_filter f); /* localized: "All", "Favorites", "Recent" */

typedef struct rnf_library_prefs rnf_library_prefs;
rnf_library_prefs* rnf_library_prefs_new(void);
void rnf_library_prefs_free(rnf_library_prefs* p);
/* A missing file leaves the prefs empty (RN_OK); RN_ERR_CORRUPT: unreadable (prefs left empty). */
rn_status rnf_library_prefs_load(rnf_library_prefs* p, const char* path);
rn_status rnf_library_prefs_save(const rnf_library_prefs* p, const char* path); /* atomic */
int rnf_library_prefs_is_favorite(const rnf_library_prefs* p, const char* key);
void rnf_library_prefs_set_favorite(rnf_library_prefs* p, const char* key, int favorite);
int rnf_library_prefs_toggle_favorite(rnf_library_prefs* p, const char* key); /* the new state */
/* A game was played: moves it to the top of the history (date = start, seconds since 1970) and
 * adds seconds to its play time (call again with date = the same start and more seconds). */
void rnf_library_prefs_record_play(rnf_library_prefs* p, const char* key, double date, double seconds);
typedef struct rnf_library_history_entry {
  const char* key;    /* valid until the prefs change */
  double last_played; /* seconds since 1970 */
  double play_seconds;
  int64_t plays;
} rnf_library_history_entry;
size_t rnf_library_prefs_history_count(const rnf_library_prefs* p);
int rnf_library_prefs_history_get(const rnf_library_prefs* p, size_t index, rnf_library_history_entry* out); /* newest first */
int rnf_library_prefs_history_find(const rnf_library_prefs* p, const char* key, rnf_library_history_entry* out);
void rnf_library_prefs_forget(rnf_library_prefs* p, const char* key); /* removes it from the history */
/* The chosen order, persisted with the rest. */
rnf_library_sort rnf_library_prefs_sort(const rnf_library_prefs* p);
void rnf_library_prefs_set_sort(rnf_library_prefs* p, rnf_library_sort s);
rnf_library_filter rnf_library_prefs_filter(const rnf_library_prefs* p);
void rnf_library_prefs_set_filter(rnf_library_prefs* p, rnf_library_filter f);

typedef struct rnf_library_item {
  const char* key;           /* ROM SHA-256 (NULL / "": unknown; never a favourite or in the history) */
  const char* file_name;     /* file name, with or without the extension */
  const char* relative_path; /* tie-break (may be NULL) */
  const rnf_game_info* game; /* NULL: not in the database */
} rnf_library_item;
/* Indices of the items to show, in order: filter (Favorites / Recent: the last
 * RNF_LIBRARY_RECENT_COUNT games played, newest first whatever the sort), search query (may be
 * NULL; case-, width- and kana-insensitive substring of the title in either language, the reading
 * or the file name), sort (in the current language; ties by title, then relative path). prefs may
 * be NULL. Two-call pattern. */
size_t rnf_library_arrange(const rnf_library_item* items, size_t count, rnf_library_sort sort, rnf_library_filter filter,
                           const char* query, const rnf_library_prefs* prefs, size_t* out, size_t cap);

/* ================================================================== transport + practice */
/* Slow motion rates: 1 = normal, 2 = 1/2, 4 = 1/4. The hotkey toggles 1/2 <-> normal. */
int rnf_slow_toggled(int rate);
char* rnf_slow_label(int rate);

typedef struct rnf_record_toggle_plan {
  int record;
  int has_seek;
  uint64_t seek;
  int play;
} rnf_record_toggle_plan;
/* The single "Record" toggle: record -> replay plays (from the start at the take end);
 * replay -> record stays at the position, paused. */
rnf_record_toggle_plan rnf_record_toggle(int recording, uint64_t frame, uint64_t take_length);
int rnf_record_toggle_restarts_on_play(int recording, int practicing, uint64_t frame, uint64_t take_length);

/* One paused frame step forward ("frame advance": R / D-pad right / hotkey / menu, also the
 * hold-to-repeat steps). Stepping is NAVIGATION on the take, like scrubbing or fast-forward:
 *   RECORD, cursor < take length, events == 0: emulates the RECORDED input of that frame (live
 *     p1/p2 are ignored); never branches, the take (takes, lengths, inputs) is unchanged.
 *   RECORD at the take end: records one frame with (p1, p2) = TAS-style frame advance (extends
 *     the take, no branch).
 *   events != 0 (a reset / power cycle requested while paused) is an explicit edit: recorded with
 *     the live input like rn_step, so mid-take it branches (the old continuation stays a take).
 *   REPLAY / PRACTICE: exactly rn_step.
 * Branching on purpose = resuming play (rn_step) from an earlier frame in RECORD. Stepping back is
 * rn_seek (take) / rn_rewind (practice). info (may be NULL) reports the session mode afterwards
 * (RECORD stays RECORD); branched is 0 for a replayed step. */
rn_status rnf_transport_step(rn_session* s, uint8_t p1, uint8_t p2, uint8_t events, rn_step_info* info);

/* Paused D-pad stepping with key repeat: press steps at once, tick yields the repeats. */
#define RNF_STEP_REPEAT_INITIAL_DELAY 18
#define RNF_STEP_REPEAT_INTERVAL 3
typedef struct rnf_step_repeater rnf_step_repeater;
rnf_step_repeater* rnf_step_repeater_new(void);
rnf_step_repeater* rnf_step_repeater_clone(const rnf_step_repeater* r);
void rnf_step_repeater_free(rnf_step_repeater* r);
int rnf_step_repeater_press(rnf_step_repeater* r, int direction);
void rnf_step_repeater_release(rnf_step_repeater* r, int direction);
int rnf_step_repeater_tick(rnf_step_repeater* r);
void rnf_step_repeater_reset(rnf_step_repeater* r);
int rnf_step_repeater_direction(const rnf_step_repeater* r);

/* Practice A/B loop (docs/design/UI_REDESIGN.md, "Practice: return to A"):
 *   playing -> (counter >= length) holding 0.5 s -> rewinding 1 s (the VTR sweep back to A: always
 *   RNF_PRACTICE_REWIND_SECONDS, however long the section; the frontend shows rnf_reel frames) ->
 *   restart (the frontend goes to A and shows its picture) -> countdown 3 s ("3", "2", "1"; optional,
 *   on by default) -> playing.
 * L2 + R2 (rnf_practice_loop_shoulders / rnf_practice_loop_return) starts the rewind at once from
 * playing or holding. Nothing is emulated outside playing (the game input is not used: the frontend
 * samples and drops it, so a tap during the countdown is not latched into the first frame). A gap
 * between ticks (paused, the menu open) does not count: the phase resumes where it was. */
#define RNF_PRACTICE_HOLD_SECONDS 0.5
#define RNF_PRACTICE_REWIND_SECONDS 1.0
#define RNF_PRACTICE_COUNTDOWN_SECONDS 3.0
typedef enum rnf_practice_phase {
  RNF_PHASE_PLAYING = 0, RNF_PHASE_HOLDING = 1, RNF_PHASE_REWINDING = 2, RNF_PHASE_COUNTDOWN = 3
} rnf_practice_phase;
typedef enum rnf_practice_action_kind {
  RNF_PRACTICE_STEP = 0, RNF_PRACTICE_BEGIN_HOLD = 1, RNF_PRACTICE_HOLD = 2, RNF_PRACTICE_REWIND_FRAME = 3,
  RNF_PRACTICE_RESTART = 4, RNF_PRACTICE_COUNTDOWN = 5
} rnf_practice_action_kind;
typedef struct rnf_practice_action {
  rnf_practice_action_kind kind;
  double back;     /* REWIND_FRAME: 0...1, how far back towards A (rnf_reel_sweep_index) */
  int count;       /* COUNTDOWN: the number shown (3, 2, 1) */
  double fraction; /* COUNTDOWN: 0...1 within that number's second (rnf_practice_countdown_visual) */
} rnf_practice_action;
typedef struct rnf_practice_loop rnf_practice_loop;
rnf_practice_loop* rnf_practice_loop_new(void);
rnf_practice_loop* rnf_practice_loop_clone(const rnf_practice_loop* l);
void rnf_practice_loop_free(rnf_practice_loop* l);
/* One unpaused tick. has_length = 0: no B (free practice, never loops by itself). */
rnf_practice_action rnf_practice_loop_tick(rnf_practice_loop* l, double now, uint64_t counter, int has_length,
                                           uint64_t length);
/* The countdown after arriving at A (default on). Off: restart -> playing at once. */
void rnf_practice_loop_set_countdown(rnf_practice_loop* l, int on);
int rnf_practice_loop_countdown(const rnf_practice_loop* l);
/* Return to A now (L2 + R2): playing / holding -> rewinding (no hold). 1 = started; 0 while already
 * returning (rewinding / countdown). */
int rnf_practice_loop_return(rnf_practice_loop* l, double now);
/* The run starts at A (practice started from the menu / the seek bar): the countdown when it is on
 * (returns 1), else playing (0). */
int rnf_practice_loop_arrive(rnf_practice_loop* l, double now);
/* Every unpaused practice tick, before rnf_practice_loop_tick: L2 (rewind) / R2 (fast-forward) held.
 * has_a: the run has an A to return to. Returns 1 when both have just become held (the chord):
 * rnf_practice_loop_return was started. *rewind (may be NULL) = whether the frontend may rewind the
 * practice run now: 0 while the chord is latched (until both are released again) and during the
 * return (rewinding / countdown), else rewind_held. */
int rnf_practice_loop_shoulders(rnf_practice_loop* l, double now, int has_a, int rewind_held, int ff_held,
                                int* rewind);
void rnf_practice_loop_interrupt(rnf_practice_loop* l);
void rnf_practice_loop_reset(rnf_practice_loop* l);
rnf_practice_phase rnf_practice_loop_phase(const rnf_practice_loop* l, double* since);
int rnf_practice_loop_loops(const rnf_practice_loop* l);
/* After rn_practice_goto_a: makes rn_video show the picture at A (the frame after A, emulated with no
 * input) without moving - one rn_step then rn_rewind(1): the machine state, the practice counter (0)
 * and A's continuity are exactly as before. Display only (the countdown's picture). RN_ERR_WRONG_MODE
 * outside practice. */
rn_status rnf_practice_preview_a(rn_session* s);
/* The countdown's look at `fraction` (0...1) of number `count` (shared by every frontend): a subtle
 * scale-in / fade-in, hold, fade-out; ring = the part of the second left (1 -> 0). */
typedef struct rnf_countdown_visual {
  int number;  /* 0: nothing to draw */
  float alpha; /* 0...1 */
  float scale; /* around 1 */
  float ring;  /* 1 ... 0 */
} rnf_countdown_visual;
rnf_countdown_visual rnf_practice_countdown_visual(int count, double fraction);
/* Index (0 = newest) into a history of count frames for animation progress back. */
int rnf_practice_history_index(double back, int count);

/* Ring of the most recent 256x240 BGRA frames (allocated on first append). */
typedef struct rnf_frame_history rnf_frame_history;
rnf_frame_history* rnf_frame_history_new(int capacity);
void rnf_frame_history_free(rnf_frame_history* h);
void rnf_frame_history_append(rnf_frame_history* h, const uint32_t* frame);
const uint32_t* rnf_frame_history_frame(const rnf_frame_history* h, int back); /* 0 = newest; NULL out of range */
int rnf_frame_history_count(const rnf_frame_history* h);
int rnf_frame_history_capacity(const rnf_frame_history* h);
void rnf_frame_history_clear(rnf_frame_history* h);
void rnf_frame_history_release(rnf_frame_history* h); /* clear + free the memory */

/* The practice run's reel: display-only pictures for the 1 s sweep back to A (never emulation /
 * determinism). Offer every picture with its position (practice counter, >= 1 = frames since A); it
 * keeps at most `capacity` 256x240 BGRA frames evenly spread from A: with stride k only positions
 * with (position - 1) % k == 0 are kept (the first frame after A always is); when full, every other
 * one is dropped and the stride doubles. Memory is allocated on the first kept frame; dropping
 * frames never moves pixels (slot indirection), so a capture costs one 240 KB copy at most. */
#define RNF_REEL_CAPACITY 120
typedef struct rnf_reel rnf_reel;
rnf_reel* rnf_reel_new(int capacity);
void rnf_reel_free(rnf_reel* r);
int rnf_reel_offer(rnf_reel* r, uint64_t position, const uint32_t* frame); /* 1 = kept */
void rnf_reel_truncate(rnf_reel* r, uint64_t position); /* drops frames after position (practice rewind) */
void rnf_reel_clear(rnf_reel* r);                       /* empty, stride 1 (keeps the memory) */
void rnf_reel_release(rnf_reel* r);                     /* clear + free the memory */
int rnf_reel_count(const rnf_reel* r);
int rnf_reel_capacity(const rnf_reel* r);
uint64_t rnf_reel_stride(const rnf_reel* r);
uint64_t rnf_reel_position(const rnf_reel* r, int i);       /* 0 = oldest (nearest A) */
const uint32_t* rnf_reel_frame(const rnf_reel* r, int i);   /* 0 = oldest; NULL out of range */
/* Frame index for sweep progress back (0 = the newest ... 1 = the oldest, evenly in position); -1 if empty. */
int rnf_reel_sweep_index(const rnf_reel* r, double back);

/* ------------------------------------------------------------------ VTR effect (display only)
 * Tape-rewind look over the picture while it runs backwards (docs/design/UI_REDESIGN.md, "VTR
 * effect"): two soft tracking-noise bands drifting smoothly, slight line jitter and head-switching
 * skew at the bottom, mild chroma bleed, desaturation. A CPU pass on the 256x240 picture (identical on
 * every platform, before the CRT / scaling), run only while active: zero cost otherwise.
 * Photosensitivity: low contrast, no full-screen brightness change (the mean luminance moves < 5 %),
 * the noise changes at 30 Hz inside the bands only, the bands move smoothly; Reduce Flashing
 * Standard / High tone it down further; the "VTR Effect" setting turns it off.
 * Kinds: REWIND (L2 hold, subtle), FAST_FORWARD (R2: a lighter variant - thin bands only, no colour
 * change), RETURN (practice sweep back to A, full). */
typedef enum rnf_vtr_kind {
  RNF_VTR_NONE = 0, RNF_VTR_REWIND = 1, RNF_VTR_FAST_FORWARD = 2, RNF_VTR_RETURN = 3
} rnf_vtr_kind;
#define RNF_VTR_FADE_IN_SECONDS 0.12
#define RNF_VTR_FADE_OUT_SECONDS 0.25
typedef struct rnf_vtr_params {
  float strength;    /* 0: off (do not call rnf_vtr_apply) ... 1 */
  double time;       /* seconds since the effect started (animation clock) */
  rnf_vtr_kind kind; /* the variant (kept while fading out) */
} rnf_vtr_params;
typedef struct rnf_vtr rnf_vtr;
rnf_vtr* rnf_vtr_new(void);
void rnf_vtr_free(rnf_vtr* v);
/* enabled = the VTR Effect setting; level = Reduce Flashing (Standard / High: weaker). */
void rnf_vtr_configure(rnf_vtr* v, int enabled, rn_flash_level level);
/* Once per display tick with what is wanted now (NONE: fades out). */
rnf_vtr_params rnf_vtr_tick(rnf_vtr* v, double now, rnf_vtr_kind want);
int rnf_vtr_active(const rnf_vtr* v); /* the last tick's strength > 0 */
void rnf_vtr_reset(rnf_vtr* v);       /* off at once */
/* Peak strength of a kind under the current configuration (0 when disabled). */
float rnf_vtr_peak(const rnf_vtr* v, rnf_vtr_kind kind);
/* in -> out (256x240 BGRA, may not alias). */
void rnf_vtr_apply(const uint32_t* in, uint32_t* out, const rnf_vtr_params* p);

/* Decaying audio tail built from the last frame's samples (practice reaching B). Returns
 * n * repeats (0 when n or repeats is 0). */
size_t rnf_audio_fade_tail(const int16_t* last, size_t n, int repeats, int16_t* out, size_t cap);

/* Fast-forward = replay the recorded take only (record mode is suspended for the hold). */
typedef struct rnf_fast_forward rnf_fast_forward;
rnf_fast_forward* rnf_fast_forward_new(void);
rnf_fast_forward* rnf_fast_forward_clone(const rnf_fast_forward* f);
void rnf_fast_forward_free(rnf_fast_forward* f);
rn_status rnf_fast_forward_begin(rnf_fast_forward* f, rn_session* s);
/* Emulates up to n recorded frames: *done frames, *reached_end at the take end. */
rn_status rnf_fast_forward_step(const rnf_fast_forward* f, rn_session* s, int n, int* done, int* reached_end);
rn_status rnf_fast_forward_end(rnf_fast_forward* f, rn_session* s);
int rnf_fast_forward_active(const rnf_fast_forward* f);
int rnf_fast_forward_shows_recording(const rnf_fast_forward* f, rn_session* s);

/* ================================================================== export + streaming geometry */
typedef enum rnf_export_preset_kind { RNF_PRESET_NATIVE = 0, RNF_PRESET_CANVAS = 1 } rnf_export_preset_kind;
typedef struct rnf_export_preset {
  rnf_export_preset_kind kind;
  int scale;          /* NATIVE: source x scale */
  int width, height;  /* CANVAS */
} rnf_export_preset;
size_t rnf_export_preset_count(void);
int rnf_export_preset_get(size_t index, rnf_export_preset* out);
char* rnf_export_preset_label(const rnf_export_preset* p);

typedef struct rnf_export_settings {
  rnf_export_preset preset;
  int crop_top, crop_bottom, crop_left, crop_right;
  int pixel_aspect_87;
  uint64_t start_frame, end_frame; /* end 0 = take end */
} rnf_export_settings;
/* RN_ERR_INVALID_ARG with a localized *message (may be NULL) when the settings are unusable. */
rn_status rnf_export_validate(const rnf_export_settings* s, char** message);

typedef struct rnf_export_geometry {
  int canvas_width, canvas_height;
  int dst_x, dst_y, dst_width, dst_height;
  int src_x, src_y, src_width, src_height;
  int vertical_scale;
} rnf_export_geometry;
void rnf_export_geometry_compute(const rnf_export_settings* s, rnf_export_geometry* out);
/* Source column / row of every destination column / row (out: dst_width / dst_height entries). */
void rnf_export_column_map(const rnf_export_geometry* g, int* out);
void rnf_export_row_map(const rnf_export_geometry* g, int* out);
/* Bit rate choice of the export dialog: the YouTube table scaled by 0.5 / 1 / 2. */
typedef enum rnf_export_quality { RNF_QUALITY_LIGHT = 0, RNF_QUALITY_STANDARD = 1, RNF_QUALITY_HIGH = 2 } rnf_export_quality;
#define RNF_QUALITY_COUNT 3
/* Average video bit rate (bit/s) following YouTube's recommended SDR upload table for 48-60 fps:
 * 2160p 60, 1440p 24, 1080p 12, 720p 7.5, 480p 4, 360p 1.5 Mbit/s. Keyed by the canvas height: exact at
 * those heights, linear in height^2 (= the 16:9 pixel count) between two of them, proportional to
 * height^2 beyond the ends, the base never below 1.5 Mbit/s. hevc != 0: x0.7 (HEVC needs ~30% less).
 * quality scales the result (Light x0.5, Standard x1 = the table, High x2); out-of-range = Standard. */
int64_t rnf_export_video_bitrate(const rnf_export_geometry* g, int hevc, int quality);
/* AAC mono, as YouTube recommends (128 kbit/s). */
int64_t rnf_export_audio_bitrate(void);
/* Key frame interval (frames): half the frame rate, as YouTube recommends. */
#define RNF_EXPORT_GOP_FRAMES 30
typedef struct rnf_export_prediction {
  int64_t video_bitrate, audio_bitrate; /* bit/s */
  double seconds;                       /* frames x 655171 / 39375000 */
  int64_t video_bytes, audio_bytes;     /* rate x duration */
  int64_t container_bytes;              /* MP4 boxes: ftyp/moov headers, per-sample tables, chunk offsets */
  int64_t total_bytes;
} rnf_export_prediction;
/* Expected size of the MP4 for exactly `frames` frames of this canvas / codec / quality: the encoders'
 * rate control targets the bit rates (the file is within a few percent), the container part is
 * computed from the MP4 structure (stsz 4 B per video / AAC sample, stss per key frame, stco per
 * 1 s chunk, fixed boxes). Returns total_bytes; `detail` may be NULL. */
int64_t rnf_export_predict_size(const rnf_export_geometry* g, int hevc, int quality, uint64_t frames,
                                rnf_export_prediction* detail);

typedef enum rnf_stream_size {
  RNF_STREAM_X1 = 0, RNF_STREAM_X2, RNF_STREAM_X3, RNF_STREAM_X4, RNF_STREAM_W1280, RNF_STREAM_W1920
} rnf_stream_size;
#define RNF_STREAM_DEFAULT RNF_STREAM_X4
typedef struct rnf_stream_layout {
  int canvas_width, canvas_height;
  double x, y, width, height; /* the 256x240 frame inside the canvas (origin top-left) */
} rnf_stream_layout;
rnf_stream_layout rnf_stream_output_layout(rnf_stream_size size, int par87);
int rnf_stream_output_multiple(rnf_stream_size size); /* 0 for the fixed 4:3 canvases */
char* rnf_stream_output_label(rnf_stream_size size, int par87);

/* ================================================================== flash reduction (UI) */
char* rnf_flash_level_label(rn_flash_level level);
char* rnf_flash_level_detail(rn_flash_level level);

#ifdef __cplusplus
}
#endif
#endif /* REPLAYNES_FRONTEND_H */
