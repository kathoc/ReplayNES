// Host clock helpers: monotonic seconds, absolute sleeps, CPU time.
//   Linux / macOS: CLOCK_MONOTONIC, clock_nanosleep(TIMER_ABSTIME), getrusage.
//   Windows: QueryPerformanceCounter (the clock of DXGI's frame statistics, SyncQPCTime), a
//   high-resolution waitable timer for the bulk of a sleep + a short spin for the last ~0.4 ms,
//   GetProcessTimes / GetThreadTimes.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#ifndef _WIN32
#include <sys/resource.h>
#include <time.h>
#endif

#include <cerrno>
#include <cmath>
#include <cstdint>

namespace rnl {

#ifdef _WIN32

// platform/host_clock_windows.cpp (keeps <windows.h> out of the shared headers).
double nowSeconds();
/// QPC ticks -> seconds on the nowSeconds() clock (DXGI_FRAME_STATISTICS::SyncQPCTime).
double qpcToSeconds(int64_t ticks);
/// Sleeps until the absolute nowSeconds() time t (returns at once if it passed). A per-thread
/// high-resolution waitable timer (Windows 10 1803+; ~0.5 ms granularity) sleeps until ~0.4 ms
/// before t, the rest is a yielding spin, so just-in-time input samples land within ~50 us.
void sleepUntil(double t);
/// The same without the final spin (timer precision, ~0.5-1 ms late): background polling threads.
void sleepUntilCoarse(double t);
/// CPU seconds (user + kernel) of the process / the calling thread.
double processCPUSeconds();
double threadCPUSeconds();

#else

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

inline void sleepUntilCoarse(double t) { sleepUntil(t); }

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

#endif

}  // namespace rnl
