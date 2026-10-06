// SPDX-License-Identifier: GPL-2.0-or-later
#include "perf_stats.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace rnl {

namespace {
struct Stat {
  size_t n = 0;
  double mean = 0, p50 = 0, p95 = 0, p99 = 0, max = 0;
};
Stat stat(std::vector<double> v) {
  Stat s;
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  s.n = v.size();
  double sum = 0;
  for (double x : v) sum += x;
  s.mean = sum / double(v.size());
  auto q = [&](double p) { return v[std::min(v.size() - 1, size_t(p * double(v.size() - 1)))]; };
  s.p50 = q(0.5);
  s.p95 = q(0.95);
  s.p99 = q(0.99);
  s.max = v.back();
  return s;
}
std::string esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o;
}
}  // namespace

void PerfStats::beginWindow(double now, double cpuProcess, double cpuThread, uint64_t underruns, uint64_t emulated) {
  begun_ = true;
  t0_ = now;
  cpuP0_ = cpuProcess;
  cpuT0_ = cpuThread;
  under0_ = underruns;
  emu0_ = emulated;
}

void PerfStats::endWindow(double now, double cpuProcess, double cpuThread, uint64_t underruns, uint64_t emulated,
                          double audioRatio, double audioFillMs, uint64_t skippedRefreshes) {
  ended_ = true;
  t1_ = now;
  cpuP1_ = cpuProcess;
  cpuT1_ = cpuThread;
  under1_ = underruns;
  emu1_ = emulated;
  ratio_ = audioRatio;
  fillMs_ = audioFillMs;
  skipped_ = skippedRefreshes;
}

std::string PerfStats::finish(const RunInfo& info, const std::string& statsPath, const std::string& framePath) {
  if (!framePath.empty()) {
    std::ofstream f(framePath);
    f << "seq,frame,emulated,wake,sample,polled,emulatedAt,ui,acquireWait,submit,target,displayed,event,lead,refresh\n";
    char buf[512];
    for (const auto& r : frames_) {
      std::snprintf(buf, sizeof buf, "%llu,%llu,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
                    (unsigned long long)r.seq, (unsigned long long)r.frame, r.emulated ? 1 : 0, r.wake, r.sample,
                    r.polled, r.emulatedAt, r.ui, r.acquireWait, r.submit, r.target, r.displayed, r.event, r.lead, r.refresh);
      f << buf;
    }
  }
  if (!begun_) return "no measurement window\n";
  if (!ended_) t1_ = frames_.empty() ? t0_ : frames_.back().sample;
  double dt = std::max(1e-6, t1_ - t0_), mins = dt / 60;
  std::vector<FrameRecord> win;
  for (const auto& r : frames_)
    if (r.sample >= t0_ && r.sample <= t1_) win.push_back(r);
  auto ms = [](double s) { return s * 1000.0; };
  std::vector<double> jit, work, gpuToScreen, total, vsTarget, event, leads, poll, emu, ui, draw, acq;
  uint64_t live = 0, unconfirmed = 0, missed = 0, judder = 0, intervalJudder = 0, pairs = 0;
  double refresh = 0;
  std::vector<double> intervals;
  const FrameRecord* prev = nullptr;
  for (const auto& r : win) {
    if (r.refresh > 0) refresh = r.refresh;
    if (!r.emulated) { prev = nullptr; continue; }
    live += 1;
    leads.push_back(ms(r.lead));
    jit.push_back(ms(r.sample - r.wake));
    work.push_back(ms(r.submit - r.sample));
    poll.push_back(ms(r.polled - r.sample));
    emu.push_back(ms(r.emulatedAt - r.polled));
    ui.push_back(ms(r.ui - r.emulatedAt));
    draw.push_back(ms(r.submit - r.ui));
    acq.push_back(ms(r.acquireWait));
    if (r.displayed > 0) {
      gpuToScreen.push_back(ms(r.displayed - r.submit));
      total.push_back(ms(r.displayed - r.sample));
      if (r.target > 0) {
        vsTarget.push_back(ms(r.displayed - r.target));
        if (r.refresh > 0 && r.displayed > r.target + r.refresh / 2) missed += 1;
      }
      if (r.event > 0) event.push_back(ms(r.displayed - r.event));
    } else {
      unconfirmed += 1;
    }
    if (prev && r.frame > prev->frame && r.frame <= prev->frame + uint64_t(std::max(1, r.frames)) && r.displayed > 0 && prev->displayed > 0 && r.refresh > 0) {
      double actual = r.displayed - prev->displayed;
      intervals.push_back(actual);
      pairs += 1;
      if (r.target > 0 && prev->target > 0 && std::fabs(actual - (r.target - prev->target)) > r.refresh / 2) judder += 1;
    }
    prev = &r;
  }
  if (!intervals.empty() && refresh > 0) {
    std::vector<double> s = intervals;
    std::sort(s.begin(), s.end());
    double steady = s[s.size() / 2];
    for (double x : intervals)
      if (std::fabs(x - steady) > refresh / 2) intervalJudder += 1;
  }
  Stat sJit = stat(jit), sWork = stat(work), sGpu = stat(gpuToScreen), sTotal = stat(total), sTarget = stat(vsTarget),
       sEvent = stat(event), sLead = stat(leads), sPoll = stat(poll), sEmu = stat(emu), sUi = stat(ui), sDraw = stat(draw), sAcq = stat(acq);
  double cpuProc = (cpuP1_ - cpuP0_) / dt * 100, cpuThread = (cpuT1_ - cpuT0_) / dt * 100;
  double emuFps = double(emu1_ - emu0_) / dt, presFps = double(win.size()) / dt;

  std::ostringstream js;
  auto jstat = [&](const char* name, const Stat& s) {
    char b[256];
    std::snprintf(b, sizeof b, "\"%s\":{\"n\":%zu,\"mean\":%.3f,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f}", name, s.n,
                  s.mean, s.p50, s.p95, s.p99, s.max);
    return std::string(b);
  };
  char head[1024];
  std::snprintf(head, sizeof head,
                "{\"label\":\"%s\",\"mode\":\"%s\",\"videoDriver\":\"%s\",\"gpu\":\"%s\",\"audioDevice\":\"%s\","
                "\"cadence\":\"%s\",\"width\":%d,\"height\":%d,\"fullscreen\":%s,\"presentWait\":%s,",
                esc(info.label).c_str(), esc(info.mode).c_str(), esc(info.videoDriver).c_str(), esc(info.gpu).c_str(),
                esc(info.audioDevice).c_str(), esc(info.cadence).c_str(), info.width, info.height,
                info.fullscreen ? "true" : "false", info.presentWait ? "true" : "false");
  char body[1024];
  std::snprintf(body, sizeof body,
                "\"seconds\":%.1f,\"refreshHz\":%.3f,\"emulatedFPS\":%.3f,\"presentedFPS\":%.3f,\"liveFrames\":%llu,"
                "\"judderPerMin\":%.2f,\"intervalJudderPerMin\":%.2f,\"missedTargets\":%llu,\"unconfirmedFrames\":%llu,"
                "\"skippedRefreshes\":%llu,\"audioUnderruns\":%llu,\"audioRatio\":%.5f,\"audioFillMs\":%.1f,"
                "\"cpuProcess\":%.2f,\"cpuFrameThread\":%.2f,\"backlogDrains\":%llu,\"multiFramePresents\":%llu,"
                "\"droppedFrames\":%llu,\"crt\":\"%s\",",
                dt, refresh > 0 ? 1 / refresh : 0.0, emuFps, presFps, (unsigned long long)live, judder / mins,
                intervalJudder / mins, (unsigned long long)missed, (unsigned long long)unconfirmed,
                (unsigned long long)skipped_, (unsigned long long)(under1_ - under0_), ratio_, fillMs_, cpuProc, cpuThread,
                (unsigned long long)info.backlogDrains, (unsigned long long)info.multiFramePresents,
                (unsigned long long)info.droppedFrames, esc(info.crt).c_str());
  js << head << body << "\"stages\":{" << jstat("wake->sample", sJit) << "," << jstat("sample->submit", sWork) << "," << jstat("events", sPoll) << "," << jstat("emulate", sEmu) << "," << jstat("ui", sUi) << ","
     << jstat("draw+present", sDraw) << "," << jstat("acquireWait", sAcq) << ","
     << jstat("submit->onScreen", sGpu) << "," << jstat("sample->onScreen", sTotal) << ","
     << jstat("onScreen-target", sTarget) << "," << jstat("event->onScreen", sEvent) << "," << jstat("lead", sLead)
     << "}}";
  if (!statsPath.empty()) {
    std::ofstream f(statsPath, std::ios::app);
    f << js.str() << "\n";
  }

  std::ostringstream h;
  char line[512];
  std::snprintf(line, sizeof line, "%s: %s  %s  %dx%d %s  present_wait=%s  cadence %s  refresh %.3f Hz  window %.0f s\n",
                info.label.c_str(), info.mode.c_str(), info.videoDriver.c_str(), info.width, info.height,
                info.fullscreen ? "fullscreen" : "window", info.presentWait ? "yes" : "no", info.cadence.c_str(),
                refresh > 0 ? 1 / refresh : 0.0, dt);
  h << line;
  std::snprintf(line, sizeof line, "emulated fps %.3f   presented fps %.3f   live frames %llu\n", emuFps, presFps,
                (unsigned long long)live);
  h << line;
  std::snprintf(line, sizeof line, "%-22s %7s %7s %7s %7s %7s  n\n", "stage (ms)", "mean", "p50", "p95", "p99", "max");
  h << line;
  auto row = [&](const char* name, const Stat& s) {
    if (!s.n) return;
    char b[256];
    std::snprintf(b, sizeof b, "%-22s %7.2f %7.2f %7.2f %7.2f %7.2f  %zu\n", name, s.mean, s.p50, s.p95, s.p99, s.max, s.n);
    h << b;
  };
  row("wake->sample (JIT)", sJit);
  row("sample->submit", sWork);
  row("  events", sPoll);
  row("  emulate+filter+audio", sEmu);
  row("  ui", sUi);
  row("  draw+present", sDraw);
  row("    (acquire wait)", sAcq);
  row("submit->on screen", sGpu);
  row("SAMPLE->ON SCREEN", sTotal);
  row("on screen - target", sTarget);
  row("event->on screen", sEvent);
  row("input lead", sLead);
  std::snprintf(line, sizeof line,
                "judder/min %.1f (vs cadence)  %.1f (vs steady interval)  missed targets %llu  unconfirmed %llu  skipped "
                "refreshes %llu  backlog drains %llu\n",
                judder / mins, intervalJudder / mins, (unsigned long long)missed, (unsigned long long)unconfirmed,
                (unsigned long long)skipped_, (unsigned long long)info.backlogDrains);
  h << line;
  std::snprintf(line, sizeof line, "audio underruns %llu  ratio %.5f  fill %.1f ms   CPU %% of one core: process %.1f  frame thread %.1f\n",
                (unsigned long long)(under1_ - under0_), ratio_, fillMs_, cpuProc, cpuThread);
  h << line;
  std::snprintf(line, sizeof line, "presents with 2 emulated frames %llu  dropped frames %llu  CRT %s\n",
                (unsigned long long)info.multiFramePresents, (unsigned long long)info.droppedFrames, info.crt.c_str());
  h << line;
  return h.str();
}

}  // namespace rnl
