// Host clock helpers (CLOCK_MONOTONIC seconds, absolute sleeps, CPU time).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <sys/resource.h>
#include <time.h>

#include <cerrno>
#include <cmath>

namespace rnl {

inline double nowSeconds() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

/// Sleeps until the absolute CLOCK_MONOTONIC time t (returns at once if it passed).
inline void sleepUntil(double t) {
  if (!(t > 0)) return;
  timespec ts;
  ts.tv_sec = time_t(std::floor(t));
  ts.tv_nsec = long((t - std::floor(t)) * 1e9);
  if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
#ifdef TIMER_ABSTIME
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
#else  // (the headless logic tests also build on macOS)
  double d = t - nowSeconds();
  if (d <= 0) return;
  timespec rel{time_t(d), long((d - std::floor(d)) * 1e9)};
  while (nanosleep(&rel, &rel) == -1 && errno == EINTR) {}
#endif
}

inline double tvSeconds(const timeval& tv) { return double(tv.tv_sec) + double(tv.tv_usec) * 1e-6; }

/// CPU seconds (user + system) of the process / the calling thread.
inline double processCPUSeconds() {
  rusage r{};
  getrusage(RUSAGE_SELF, &r);
  return tvSeconds(r.ru_utime) + tvSeconds(r.ru_stime);
}
inline double threadCPUSeconds() {
  rusage r{};
#ifdef RUSAGE_THREAD
  getrusage(RUSAGE_THREAD, &r);
#else
  getrusage(RUSAGE_SELF, &r);
#endif
  return tvSeconds(r.ru_utime) + tvSeconds(r.ru_stime);
}

}  // namespace rnl
