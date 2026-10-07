// Windows part of host_clock.h: QueryPerformanceCounter, high-resolution waitable timers, CPU times.
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>

#include "host_clock.h"

namespace rnl {

namespace {
double qpcFrequency() {
  static const double f = [] {
    LARGE_INTEGER q;
    QueryPerformanceFrequency(&q);
    return double(q.QuadPart);
  }();
  return f;
}
double fileTimeSeconds(const FILETIME& f) {
  ULARGE_INTEGER u;
  u.LowPart = f.dwLowDateTime;
  u.HighPart = f.dwHighDateTime;
  return double(u.QuadPart) * 1e-7;
}
}  // namespace

double qpcToSeconds(int64_t ticks) { return double(ticks) / qpcFrequency(); }

double nowSeconds() {
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  return qpcToSeconds(q.QuadPart);
}

namespace {
/// Waits on this thread's high-resolution waitable timer for `seconds` (Windows 10 1803+; ~0.5 ms
/// granularity; an ordinary timer before 1803).
void timerWait(double seconds) {
  thread_local HANDLE timer = [] {
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!h) h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    return h;
  }();
  LARGE_INTEGER due;
  due.QuadPart = -int64_t(seconds * 1e7);  // relative, 100 ns units
  if (due.QuadPart >= 0) return;
  if (timer && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer, INFINITE);
  else Sleep(DWORD(seconds * 1000));
}
}  // namespace

void sleepUntil(double t) {
  if (!(t > 0)) return;
  constexpr double kSpin = 0.0004;
  double d = t - nowSeconds();
  if (d <= 0) return;
  if (d > kSpin) timerWait(d - kSpin);
  while (nowSeconds() < t) YieldProcessor();
}

void sleepUntilCoarse(double t) {
  if (!(t > 0)) return;
  double d = t - nowSeconds();
  if (d > 0) timerWait(d);
}

double processCPUSeconds() {
  FILETIME c, e, k, u;
  if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
  return fileTimeSeconds(k) + fileTimeSeconds(u);
}
double threadCPUSeconds() {
  FILETIME c, e, k, u;
  if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
  return fileTimeSeconds(k) + fileTimeSeconds(u);
}

}  // namespace rnl
