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
#define RNF_CONTROLLER_LAYOUT_VERSION 4
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

/* Assignment summaries for the diagram. */
/* Actions bound to gc<slot>:<element>, in catalog order (a = action id). */
rnf_list* rnf_input_element_actions(const rnf_binding* bindings, size_t count, const char* element, int slot);
/* "Right Button (A)", "D-pad ↑", "ZL" ... label_override (may be NULL) replaces the family label. */
char* rnf_input_element_title(const char* element, rnf_controller_family f, const char* label_override);
/* Compact badge label: "A", "Turbo A", "Rewind", "2P B" ... */
char* rnf_input_action_short_label(const char* action, int slot);
/* Badge text (actions joined with " · "); NULL when nothing is bound. */
char* rnf_input_element_badge(const rnf_binding* bindings, size_t count, const char* element, int slot);
typedef enum rnf_group_summary { RNF_GROUP_NONE = 0, RNF_GROUP_MOVEMENT = 1, RNF_GROUP_CUSTOM = 2 } rnf_group_summary;
/* "dpad" / "lstick" / "rstick": MOVEMENT ("Move" / "2P Move" in *movement_text) when the four
 * directions map 1:1 to one player's directions; NONE when nothing is bound. */
rnf_group_summary rnf_input_group_summary(const rnf_binding* bindings, size_t count, const char* group, int slot,
                                          char** movement_text);

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

/* Practice A/B loop: playing -> (counter >= length) holding 0.5 s -> rewinding 0.5 s -> restart. */
#define RNF_PRACTICE_HOLD_SECONDS 0.5
#define RNF_PRACTICE_REWIND_SECONDS 0.5
typedef enum rnf_practice_phase { RNF_PHASE_PLAYING = 0, RNF_PHASE_HOLDING = 1, RNF_PHASE_REWINDING = 2 } rnf_practice_phase;
typedef enum rnf_practice_action_kind {
  RNF_PRACTICE_STEP = 0, RNF_PRACTICE_BEGIN_HOLD = 1, RNF_PRACTICE_HOLD = 2, RNF_PRACTICE_REWIND_FRAME = 3,
  RNF_PRACTICE_RESTART = 4
} rnf_practice_action_kind;
typedef struct rnf_practice_action {
  rnf_practice_action_kind kind;
  double back; /* REWIND_FRAME: 0...1, how far back into the recent frames */
} rnf_practice_action;
typedef struct rnf_practice_loop rnf_practice_loop;
rnf_practice_loop* rnf_practice_loop_new(void);
rnf_practice_loop* rnf_practice_loop_clone(const rnf_practice_loop* l);
void rnf_practice_loop_free(rnf_practice_loop* l);
/* One unpaused tick. has_length = 0: no B (free practice, never loops). */
rnf_practice_action rnf_practice_loop_tick(rnf_practice_loop* l, double now, uint64_t counter, int has_length,
                                           uint64_t length);
void rnf_practice_loop_interrupt(rnf_practice_loop* l);
void rnf_practice_loop_reset(rnf_practice_loop* l);
rnf_practice_phase rnf_practice_loop_phase(const rnf_practice_loop* l, double* since);
int rnf_practice_loop_loops(const rnf_practice_loop* l);
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
/* Average video bit rate: canvas pixels x 60 x bits_per_pixel, at least 2 Mbit/s. */
int64_t rnf_export_video_bitrate(const rnf_export_geometry* g, double bits_per_pixel);

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
