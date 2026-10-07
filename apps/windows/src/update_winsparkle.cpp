// SPDX-License-Identifier: GPL-2.0-or-later
// WinSparkle updates + the zip installer (see update_winsparkle.h).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "replaynes/frontend.h"
#include "update_winsparkle.h"

namespace fs = std::filesystem;

#ifndef RNW_UPDATE_PUBLIC_KEY
#define RNW_UPDATE_PUBLIC_KEY ""
#endif
#ifndef RNW_APPCAST_URL
#define RNW_APPCAST_URL "https://github.com/kathoc/ReplayNES/releases/latest/download/appcast-windows.xml"
#endif

namespace rnl {

namespace {

std::wstring widen(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(size_t(std::max(0, n - 1)), L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}

std::string narrow(const std::wstring& w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(size_t(std::max(0, n - 1)), '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  return s;
}

fs::path exePath() {
  std::wstring p(32768, L'\0');
  DWORD n = GetModuleFileNameW(nullptr, p.data(), DWORD(p.size()));
  p.resize(n);
  return fs::path(p);
}

/// %LOCALAPPDATA%\ReplayNES\Update
fs::path updateRoot() {
  PWSTR p = nullptr;
  fs::path root;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) root = fs::path(p) / L"ReplayNES" / L"Update";
  CoTaskMemFree(p);
  return root;
}

/// One line to %LOCALAPPDATA%\ReplayNES\Update\update.log (and stderr).
void logLine(const std::string& s) {
  std::fprintf(stderr, "update: %s\n", s.c_str());
  std::error_code ec;
  fs::path root = updateRoot();
  if (root.empty()) return;
  fs::create_directories(root, ec);
  std::ofstream f(root / L"update.log", std::ios::app);
  char t[32];
  std::time_t now = std::time(nullptr);
  std::strftime(t, sizeof t, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
  f << t << "  " << s << "\n";
}

/// One argument for a CreateProcess command line (the MSVC runtime's parsing rules).
std::wstring quoteArg(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
  std::wstring r = L"\"";
  for (size_t i = 0;; ++i) {
    size_t slashes = 0;
    while (i < a.size() && a[i] == L'\\') {
      ++i;
      ++slashes;
    }
    if (i == a.size()) {
      r.append(slashes * 2, L'\\');
      break;
    }
    if (a[i] == L'"') {
      r.append(slashes * 2 + 1, L'\\');
      r += L'"';
    } else {
      r.append(slashes, L'\\');
      r += a[i];
    }
  }
  return r + L"\"";
}

/// Starts `exe args` (no window of its own for console programs); waits when `wait`, returns the
/// exit code (0 when not waiting), -1 if it could not start.
int run(const fs::path& exe, const std::vector<std::wstring>& args, bool wait, const fs::path& cwd = {}) {
  std::wstring cmd = quoteArg(exe.wstring());
  for (const auto& a : args) cmd += L" " + quoteArg(a);
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      cwd.empty() ? nullptr : cwd.c_str(), &si, &pi))
    return -1;
  DWORD code = 0;
  if (wait) {
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return int(code);
}

// ------------------------------------------------------------------ WinSparkle (loaded at run time)
struct WinSparkleApi {
  HMODULE dll = nullptr;
  void(__cdecl* init)() = nullptr;
  void(__cdecl* cleanup)() = nullptr;
  void(__cdecl* set_appcast_url)(const char*) = nullptr;
  int(__cdecl* set_eddsa_public_key)(const char*) = nullptr;
  void(__cdecl* set_app_details)(const wchar_t*, const wchar_t*, const wchar_t*) = nullptr;
  void(__cdecl* set_registry_path)(const char*) = nullptr;
  void(__cdecl* set_lang)(const char*) = nullptr;
  void(__cdecl* set_automatic_check_for_updates)(int) = nullptr;
  int(__cdecl* get_automatic_check_for_updates)() = nullptr;
  void(__cdecl* set_can_shutdown_callback)(int(__cdecl*)()) = nullptr;
  void(__cdecl* set_shutdown_request_callback)(void(__cdecl*)()) = nullptr;
  void(__cdecl* set_user_run_installer_callback)(int(__cdecl*)(const wchar_t*)) = nullptr;
  void(__cdecl* check_update_with_ui)() = nullptr;
  void(__cdecl* check_update_with_ui_and_install)() = nullptr;

  bool load(const fs::path& path) {
    dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) return false;
    bool ok = true;
    auto get = [&](auto& fn, const char* name) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
      ok &= fn != nullptr;
    };
    get(init, "win_sparkle_init");
    get(cleanup, "win_sparkle_cleanup");
    get(set_appcast_url, "win_sparkle_set_appcast_url");
    get(set_eddsa_public_key, "win_sparkle_set_eddsa_public_key");
    get(set_app_details, "win_sparkle_set_app_details");
    get(set_registry_path, "win_sparkle_set_registry_path");
    get(set_lang, "win_sparkle_set_lang");
    get(set_automatic_check_for_updates, "win_sparkle_set_automatic_check_for_updates");
    get(get_automatic_check_for_updates, "win_sparkle_get_automatic_check_for_updates");
    get(set_can_shutdown_callback, "win_sparkle_set_can_shutdown_callback");
    get(set_shutdown_request_callback, "win_sparkle_set_shutdown_request_callback");
    get(set_user_run_installer_callback, "win_sparkle_set_user_run_installer_callback");
    get(check_update_with_ui, "win_sparkle_check_update_with_ui");
    get(check_update_with_ui_and_install, "win_sparkle_check_update_with_ui_and_install");
    if (!ok) {
      FreeLibrary(dll);
      dll = nullptr;
    }
    return ok;
  }
};

std::vector<std::string> gRestartArgs;  // the app's arguments for the updated instance

int __cdecl canShutdown() { return 1; }  // the session is saved on quit as always

void __cdecl requestShutdown() {
  SDL_Event e{};
  e.type = SDL_EVENT_QUIT;
  SDL_PushEvent(&e);  // thread-safe; the frame loop saves and quits
}

/// WinSparkle downloaded and verified the update (EdDSA): a zip is unpacked and its ReplayNES.exe
/// started as the finishing helper; anything else is left to WinSparkle (run as an installer).
int __cdecl runInstaller(const wchar_t* file) {
  const fs::path zip(file);
  std::wstring ext = zip.extension().wstring();
  for (auto& c : ext) c = wchar_t(std::towlower(c));
  if (ext != L".zip") return 0;
  std::error_code ec;
  const fs::path root = updateRoot();
  if (root.empty()) return -1;
  const fs::path stage = root / (L"staging-" + std::to_wstring(GetCurrentProcessId()));
  fs::remove_all(stage, ec);
  fs::create_directories(stage, ec);
  wchar_t sys[MAX_PATH] = {};
  GetSystemDirectoryW(sys, MAX_PATH);
  // Windows 10 1803+ ships bsdtar (zip support) as System32\tar.exe.
  int rc = run(fs::path(sys) / L"tar.exe", {L"-xf", zip.wstring(), L"-C", stage.wstring()}, true);
  if (rc != 0) {
    logLine("extracting " + narrow(zip.wstring()) + " failed (tar exit " + std::to_string(rc) + ")");
    return -1;
  }
  fs::path newExe;
  for (auto it = fs::recursive_directory_iterator(stage, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    if (it->path().filename() == L"ReplayNES.exe") {
      newExe = it->path();
      break;
    }
  }
  if (newExe.empty()) {
    logLine("the update has no ReplayNES.exe");
    return -1;
  }
  const fs::path installDir = exePath().parent_path();
  std::vector<std::wstring> args = {L"--finish-update", installDir.wstring(), std::to_wstring(GetCurrentProcessId()), L"--"};
  for (const auto& a : gRestartArgs) args.push_back(widen(a));
  logLine("installing " + narrow(newExe.parent_path().wstring()) + " into " + narrow(installDir.wstring()));
  if (run(newExe, args, false, newExe.parent_path()) < 0) {
    logLine("could not start the update helper");
    return -1;
  }
  return 1;
}

class WinSparkleUpdates : public UpdateService {
 public:
  explicit WinSparkleUpdates(WinSparkleApi api) : api_(api) {}
  ~WinSparkleUpdates() override {
    if (started_) api_.cleanup();
    if (api_.dll) FreeLibrary(api_.dll);
  }

  void start(bool) override {
    if (started_) return;
    const char* url = std::getenv("REPLAYNES_APPCAST_URL");  // testing; signatures still checked
    api_.set_registry_path("Software\\ReplayNES\\WinSparkle");
    api_.set_appcast_url(url && *url ? url : RNW_APPCAST_URL);
    if (!api_.set_eddsa_public_key(RNW_UPDATE_PUBLIC_KEY)) {
      std::fprintf(stderr, "update: invalid EdDSA public key - updates disabled\n");
      return;
    }
    api_.set_app_details(L"ReplayNES", L"ReplayNES", widen(RNL_APP_VERSION).c_str());
    api_.set_lang(std::strcmp(rnf_l10n_language(), "ja") == 0 ? "ja" : "en");
    // Automatic checks: WinSparkle's own setting (registry), which it asks for on the second launch
    // (like Sparkle on macOS); Settings -> System shows and changes it.
    api_.set_can_shutdown_callback(&canShutdown);
    api_.set_shutdown_request_callback(&requestShutdown);
    api_.set_user_run_installer_callback(&runInstaller);
    api_.init();
    started_ = true;
    std::fprintf(stderr, "update: WinSparkle, %s, automatic checks %s\n", url && *url ? url : RNW_APPCAST_URL,
                 api_.get_automatic_check_for_updates() ? "on" : "off");
    // Staging folders of finished updates (the helper can't delete the one it runs from).
    std::thread([] {
      std::error_code ec;
      const fs::path root = updateRoot(), self = exePath().parent_path();
      for (auto& e : fs::directory_iterator(root, ec)) {
        const std::wstring n = e.path().filename().wstring();
        if (n.rfind(L"staging-", 0) == 0 && self.wstring().rfind(e.path().wstring(), 0) != 0) fs::remove_all(e.path(), ec);
      }
    }).detach();
  }
  void setAutoCheck(bool on) override {
    if (started_) api_.set_automatic_check_for_updates(on ? 1 : 0);
  }
  void checkNow() override {
    if (started_) api_.check_update_with_ui();
  }
  void update() override {
    if (started_) api_.check_update_with_ui_and_install();
  }
  void dismiss() override {}
  UpdateModel snapshot() const override { return UpdateModel(); }  // WinSparkle shows its own dialogs
  bool ownsDialogs() const override { return true; }
  bool autoCheckEnabled() const override { return started_ && api_.get_automatic_check_for_updates() != 0; }
  int restartLatestAndWait(const std::vector<std::string>&) override { return -1; }  // the helper restarts it

 private:
  WinSparkleApi api_;
  bool started_ = false;
};

// ------------------------------------------------------------------ the finishing helper
bool copyWithRetry(const fs::path& from, const fs::path& to, std::string* why) {
  for (int i = 0; i < 50; ++i) {  // antivirus scanners / a slow exit can hold the files briefly
    if (CopyFileW(from.c_str(), to.c_str(), FALSE)) return true;
    Sleep(200);
  }
  *why = "cannot write " + narrow(to.wstring()) + " (error " + std::to_string(GetLastError()) + ")";
  return false;
}

}  // namespace

std::unique_ptr<UpdateService> makeWinSparkleUpdates(const std::vector<std::string>& argv) {
  if (std::strlen(RNW_UPDATE_PUBLIC_KEY) == 0) return nullptr;
  WinSparkleApi api;
  if (!api.load(exePath().parent_path() / L"WinSparkle.dll")) {
    std::fprintf(stderr, "update: WinSparkle.dll not found next to ReplayNES.exe - no in-app updates\n");
    return nullptr;
  }
  gRestartArgs = restartArguments(argv);
  return std::make_unique<WinSparkleUpdates>(api);
}

int finishUpdate(int argc, char** argv) {
  // argv: --finish-update <install dir> <pid> [-- args...]
  if (argc < 4) return 2;
  const fs::path installDir = fs::u8path(argv[2]);
  const DWORD pid = DWORD(std::strtoul(argv[3], nullptr, 10));
  std::vector<std::wstring> appArgs;
  for (int i = 4; i < argc; ++i)
    if (std::strcmp(argv[i], "--") != 0 || i != 4) appArgs.push_back(widen(argv[i]));
  const fs::path stage = exePath().parent_path();
  // 1. The old instance quits (WinSparkle asked it to): wait for it.
  if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
    if (WaitForSingleObject(h, 120000) != WAIT_OBJECT_0) {
      CloseHandle(h);
      logLine("the running ReplayNES did not quit: update cancelled");
      MessageBoxW(nullptr, L"ReplayNES did not quit, so the update was not installed.", L"ReplayNES", MB_ICONWARNING);
      return 1;
    }
    CloseHandle(h);
  }
  // 2. Copy the new files over the installation, keeping a backup of the ones replaced.
  std::error_code ec;
  const fs::path backup = stage.parent_path() / (stage.filename().wstring() + L"-backup");
  fs::remove_all(backup, ec);
  fs::create_directories(backup, ec);
  std::vector<fs::path> replaced, added;
  std::string why;
  bool ok = true;
  for (auto& e : fs::directory_iterator(stage, ec)) {
    if (!e.is_regular_file()) continue;
    const fs::path target = installDir / e.path().filename();
    if (fs::exists(target)) {
      if (!copyWithRetry(target, backup / e.path().filename(), &why)) {
        ok = false;
        break;
      }
      replaced.push_back(e.path().filename());
    } else {
      added.push_back(e.path().filename());
    }
    if (!copyWithRetry(e.path(), target, &why)) {
      ok = false;
      break;
    }
  }
  if (!ok) {
    for (const auto& n : replaced) CopyFileW((backup / n).c_str(), (installDir / n).c_str(), FALSE);
    for (const auto& n : added) DeleteFileW((installDir / n).c_str());
    logLine("update failed, previous version restored: " + why);
    MessageBoxW(nullptr, widen("The update could not be installed:\n" + why).c_str(), L"ReplayNES", MB_ICONWARNING);
  } else {
    logLine("updated " + narrow(installDir.wstring()));
  }
  fs::remove_all(backup, ec);
  // 3. Start ReplayNES (the new version, or the old one again) with the same arguments.
  if (run(installDir / L"ReplayNES.exe", appArgs, false, installDir) < 0) logLine("could not start ReplayNES again");
  return ok ? 0 : 1;
}

}  // namespace rnl
