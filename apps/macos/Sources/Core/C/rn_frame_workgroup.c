// SPDX-License-Identifier: GPL-2.0-or-later
#include "rn_frame_workgroup.h"
#include <AudioToolbox/AudioWorkInterval.h>
#include <os/workgroup.h>
#include <stdlib.h>

struct rn_frame_workgroup {
  os_workgroup_interval_t wg;
  os_workgroup_join_token_s token;
  int started;
};

rn_frame_workgroup* rn_frame_workgroup_join_new(const char* name) {
  os_workgroup_interval_t wg = AudioWorkIntervalCreate(name, OS_CLOCK_MACH_ABSOLUTE_TIME, NULL);
  if (!wg) return NULL;
  rn_frame_workgroup* f = (rn_frame_workgroup*)calloc(1, sizeof(rn_frame_workgroup));
  if (!f) { os_release(wg); return NULL; }
  f->wg = wg;
  if (os_workgroup_join(wg, &f->token) != 0) { os_release(wg); free(f); return NULL; }
  return f;
}

void rn_frame_workgroup_leave_free(rn_frame_workgroup* f) {
  if (!f) return;
  if (f->started) os_workgroup_interval_finish(f->wg, NULL);
  os_workgroup_leave(f->wg, &f->token);
  os_release(f->wg);
  free(f);
}

void rn_frame_workgroup_start(rn_frame_workgroup* f, uint64_t start, uint64_t deadline) {
  if (!f) return;
  if (f->started) os_workgroup_interval_finish(f->wg, NULL);
  f->started = os_workgroup_interval_start(f->wg, start, deadline > start ? deadline : start + 1, NULL) == 0;
}

void rn_frame_workgroup_finish(rn_frame_workgroup* f) {
  if (!f || !f->started) return;
  os_workgroup_interval_finish(f->wg, NULL);
  f->started = 0;
}
