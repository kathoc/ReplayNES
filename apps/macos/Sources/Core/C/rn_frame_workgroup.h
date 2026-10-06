// Deadline hint for the emulation thread's per-frame burst (see docs/FRAME_PACING.md).
// The frame work (sample input, emulate, flash filter, render, commit: ~1-2 ms) runs once per
// 16.7 ms; the CPU's performance controller sees a mostly idle thread and runs it at a low clock,
// so the burst took 2x longer and varied 4x (measured: mean 4.2 ms, p99 7.9 ms vs 2.1 / 2.8 ms).
// A workgroup interval with an explicit deadline per frame lets it pick the clock that meets the
// deadline. Created through AudioWorkIntervalCreate (the public way to make an interval workgroup
// for a real-time thread; not callable from Swift, hence this C shim). macOS 14 / iOS 17.
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef RN_FRAME_WORKGROUP_H
#define RN_FRAME_WORKGROUP_H
#include <stdint.h>

typedef struct rn_frame_workgroup rn_frame_workgroup;

/* Creates the workgroup and joins the calling thread to it. NULL when unavailable. */
rn_frame_workgroup* rn_frame_workgroup_join_new(const char* name);
/* Leaves (on the joined thread) and frees. */
void rn_frame_workgroup_leave_free(rn_frame_workgroup* wg);
/* One frame's work starts now (mach ticks) and must be done by `deadline` (mach ticks). */
void rn_frame_workgroup_start(rn_frame_workgroup* wg, uint64_t start, uint64_t deadline);
void rn_frame_workgroup_finish(rn_frame_workgroup* wg);
#endif
