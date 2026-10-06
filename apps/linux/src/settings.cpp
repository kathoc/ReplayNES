// SPDX-License-Identifier: GPL-2.0-or-later
#include "settings.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace rnl {

namespace {
std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}
bool parseBool(const std::string& v, bool def) {
  if (v == "1" || v == "true") return true;
  if (v == "0" || v == "false") return false;
  return def;
}
bool parseNumber(const std::string& v, double* out) {
  if (v.empty()) return false;
  char* end = nullptr;
  double d = std::strtod(v.c_str(), &end);
  if (!end || *end != '\0' || !std::isfinite(d)) return false;
  *out = d;
  return true;
}
}  // namespace

Settings Settings::parse(const std::string& text) {
  Settings s;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
    double d = 0;
    bool num = parseNumber(v, &d);
    if (k == "integerScale") s.integerScale = parseBool(v, s.integerScale);
    else if (k == "par87") s.par87 = parseBool(v, s.par87);
    else if (k == "hideOverscan") s.hideOverscan = parseBool(v, s.hideOverscan);
    else if (k == "showStats") s.showStats = parseBool(v, s.showStats);
    else if (k == "flash" && num && d >= 0 && d <= 3 && d == std::floor(d)) s.flash = int(d);
    else if (k == "showFlashIndicator") s.showFlashIndicator = parseBool(v, s.showFlashIndicator);
    else if (k == "uiScale" && num && d >= 0.5 && d <= 2.5) s.uiScale = float(d);
    else if (k == "volume" && num && d >= 0 && d <= 1) s.volume = float(d);
    else if (k == "pauseAfterRewind") s.pauseAfterRewind = parseBool(v, s.pauseAfterRewind);
    else if (k == "autosaveInterval" && num && d >= 1 && d <= 600) s.autosaveInterval = d;
    else if (k == "dpadStepWhenPaused") s.dpadStepWhenPaused = parseBool(v, s.dpadStepWhenPaused);
    else if (k == "controllerLayoutVersion" && num && d >= 0 && d <= 1000) s.controllerLayoutVersion = int(d);
    else if (k == "diagramSlot" && num && d >= 0 && d <= 3) s.diagramSlot = int(d);
    else if (k == "diagramFamily" && (v == "auto" || (num && d >= 0 && d <= 4))) s.diagramFamily = v;
    else if (k == "timelineSlot" && num && d >= 0 && d < 8) s.timelineSlot = int(d);
  }
  return s;
}

std::string Settings::serialize() const {
  std::ostringstream o;
  o << "# ReplayNES (Linux) settings\n";
  o << "integerScale=" << int(integerScale) << "\n";
  o << "par87=" << int(par87) << "\n";
  o << "hideOverscan=" << int(hideOverscan) << "\n";
  o << "showStats=" << int(showStats) << "\n";
  o << "flash=" << flash << "\n";
  o << "showFlashIndicator=" << int(showFlashIndicator) << "\n";
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.2f", double(uiScale));
  o << "uiScale=" << buf << "\n";
  std::snprintf(buf, sizeof buf, "%.3f", double(volume));
  o << "volume=" << buf << "\n";
  o << "pauseAfterRewind=" << int(pauseAfterRewind) << "\n";
  std::snprintf(buf, sizeof buf, "%g", autosaveInterval);
  o << "autosaveInterval=" << buf << "\n";
  o << "dpadStepWhenPaused=" << int(dpadStepWhenPaused) << "\n";
  o << "controllerLayoutVersion=" << controllerLayoutVersion << "\n";
  o << "diagramSlot=" << diagramSlot << "\n";
  o << "diagramFamily=" << diagramFamily << "\n";
  o << "timelineSlot=" << timelineSlot << "\n";
  return o.str();
}

bool Settings::load(const std::string& path) {
  std::ifstream f(path);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  *this = parse(ss.str());
  return true;
}

bool Settings::save(const std::string& path) const {
  std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return false;
    f << serialize();
    if (!f.good()) return false;
  }
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace rnl
