// SPDX-License-Identifier: GPL-2.0-or-later
#include "update_model.h"

#include <algorithm>
#include <cctype>

namespace rnl {

namespace {
bool contains(const std::string& s, const char* part) {
  std::string a = s, b = part;
  std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return a.find(b) != std::string::npos;
}
bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
}  // namespace

UpdateKind classifyUpdate(const std::string& running, const std::string& local, const std::string& remote) {
  if (local.empty()) return UpdateKind::none;
  if (!remote.empty() && remote != local) return UpdateKind::download;
  if (!running.empty() && running != local) return UpdateKind::restartToUse;
  return UpdateKind::none;
}

int overallPercent(uint32_t op, uint32_t nOps, uint32_t progress) {
  if (nOps == 0) return 0;
  uint32_t p = std::min<uint32_t>(progress, 100);
  uint32_t o = std::min(op, nOps - 1);  // op counts the operations done (0-based current)
  return int((uint64_t(o) * 100 + p) / nOps);
}

UpdateError classifyUpdateError(const std::string& name, const std::string& message) {
  if (contains(message, "requires new permissions")) return UpdateError::newPermissions;
  if (contains(name, "AccessDenied") || contains(message, "not allowed")) return UpdateError::denied;
  // No Access portal to ask (Gaming Mode: the gamescope portal set has none) or it failed to start.
  if (contains(message, "No portal support") || contains(message, "Access portal") || contains(message, "access dialog") ||
      contains(message, "activate remote peer"))
    return UpdateError::noDialog;
  // A remote without a URL (a bundle installed without --repo-url) or a deleted / disabled one.
  if (contains(message, "no url") || contains(message, "without a url") || contains(message, "is disabled") ||
      contains(message, "remote not found") || contains(message, "no remote") || contains(message, "has no remote"))
    return UpdateError::noRepository;
  if (contains(message, "resolve") || contains(message, "connect") || contains(message, "timeout") ||
      contains(message, "timed out") || contains(message, "network") || contains(message, "while fetching"))
    return UpdateError::network;
  return UpdateError::other;
}

void UpdateModel::setUnsupported(UpdateUnavailable why) {
  phase = UpdatePhase::unsupported;
  reason = why;
  touch();
}

void UpdateModel::portalReady() {
  if (phase == UpdatePhase::unsupported && reason != UpdateUnavailable::noRepository) phase = UpdatePhase::idle;
  if (phase == UpdatePhase::unsupported) return;
  reason = UpdateUnavailable::none;
  touch();
}

void UpdateModel::updateAvailable(const std::string& running, const std::string& local, const std::string& remote) {
  if (busy()) return;  // the running update reports its own result
  switch (classifyUpdate(running, local, remote)) {
    case UpdateKind::download:
      if (phase == UpdatePhase::installed && remote == remoteCommit) return;  // installed it, waiting for a restart
      if (remote != remoteCommit) noticeHidden = false;
      remoteCommit = remote;
      phase = UpdatePhase::available;
      reason = UpdateUnavailable::none;
      error = UpdateError::none;
      break;
    case UpdateKind::restartToUse:
      if (phase != UpdatePhase::installed) noticeHidden = false;
      remoteCommit = remote;
      phase = UpdatePhase::installed;
      break;
    case UpdateKind::none:
      if (phase == UpdatePhase::available) phase = UpdatePhase::idle;
      break;
  }
  touch();
}

void UpdateModel::checkStarted() {
  phase = UpdatePhase::checking;
  userInitiated = true;
  error = UpdateError::none;
  errorMessage.clear();
  percent = 0;
  touch();
}

void UpdateModel::updateStarted() {
  phase = UpdatePhase::checking;
  userInitiated = false;
  noticeHidden = false;
  error = UpdateError::none;
  errorMessage.clear();
  percent = 0;
  touch();
}

void UpdateModel::progress(const UpdateProgressInfo& p) {
  switch (p.status) {
    case kUpdateRunning:
      phase = UpdatePhase::updating;
      percent = overallPercent(p.op, p.nOps, p.progress);
      break;
    case kUpdateEmpty:
      phase = UpdatePhase::upToDate;
      percent = 0;
      break;
    case kUpdateDone:
      phase = UpdatePhase::installed;
      percent = 100;
      noticeHidden = false;
      break;
    default:
      callFailed(p.error, p.errorMessage);
      return;
  }
  touch();
}

void UpdateModel::callFailed(const std::string& name, const std::string& message) {
  error = classifyUpdateError(name, message);
  errorMessage = message;
  if (error == UpdateError::noRepository) {
    phase = UpdatePhase::unsupported;
    reason = UpdateUnavailable::noRepository;
  } else {
    phase = UpdatePhase::failed;
  }
  noticeHidden = false;
  touch();
}

void UpdateModel::dismiss() {
  noticeHidden = true;
  if (phase == UpdatePhase::failed || phase == UpdatePhase::upToDate) phase = UpdatePhase::idle;
  touch();
}

bool UpdateModel::noticeVisible() const {
  if (noticeHidden) return false;
  switch (phase) {
    case UpdatePhase::available:
    case UpdatePhase::updating:
    case UpdatePhase::installed: return true;
    case UpdatePhase::checking:
    case UpdatePhase::failed: return !userInitiated;  // Check now shows its result in Settings
    default: return false;
  }
}

std::vector<std::string> restartArguments(const std::vector<std::string>& argv) {
  static const char* withValue[] = {"--script", "--perf-seconds", "--warmup", "--stats-log", "--frame-log", "--label"};
  static const char* flags[] = {"--inject-input"};
  std::vector<std::string> out;
  for (size_t i = 1; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (std::any_of(std::begin(withValue), std::end(withValue), [&](const char* o) { return a == o; })) {
      ++i;  // and its value
      continue;
    }
    if (std::any_of(std::begin(flags), std::end(flags), [&](const char* o) { return a == o; })) continue;
    out.push_back(a);
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> restartEnvironment(const std::vector<std::string>& environ) {
  static const char* exact[] = {"LANG", "LANGUAGE", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE", "XDG_SESSION_DESKTOP",
                                "SteamDeck", "SteamGameId", "SteamAppId", "SteamOverlayGameId", "SteamClientLaunch",
                                "SteamGamepadUI", "SteamEnv", "ENABLE_GAMESCOPE_WSI"};
  static const char* prefixes[] = {"LC_", "REPLAYNES_", "SDL_", "STEAM_COMPAT_", "GAMESCOPE_"};
  std::vector<std::pair<std::string, std::string>> out;
  for (const std::string& e : environ) {
    size_t eq = e.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    std::string k = e.substr(0, eq), v = e.substr(eq + 1);
    bool keep = std::any_of(std::begin(exact), std::end(exact), [&](const char* x) { return k == x; }) ||
                std::any_of(std::begin(prefixes), std::end(prefixes), [&](const char* p) { return startsWith(k, p); });
    if (keep) out.emplace_back(k, v);
  }
  return out;
}

std::string parseVersionOutput(const std::string& text) {
  size_t p = text.find("ReplayNES ");
  size_t i = p == std::string::npos ? 0 : p + 10;
  while (i < text.size() && !std::isdigit(static_cast<unsigned char>(text[i]))) {
    if (text[i] == '\n') return {};
    ++i;
  }
  size_t j = i;
  while (j < text.size() && (std::isdigit(static_cast<unsigned char>(text[j])) || text[j] == '.')) ++j;
  std::string v = text.substr(i, j - i);
  while (!v.empty() && v.back() == '.') v.pop_back();
  return v.find('.') == std::string::npos ? std::string() : v;
}

}  // namespace rnl
