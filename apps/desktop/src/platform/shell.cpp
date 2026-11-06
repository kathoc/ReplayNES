// openFolder / openFile (platform.h): Explorer / the shell on Windows, the desktop's file manager or
// default application through SDL elsewhere. On Android a folder opens in the system Files app through the
// app's documents provider (MainActivity.openFolder, docs/design/ANDROID.md): a file:// URI cannot leave
// the app there (FileUriExposedException, which SDL_OpenURL swallows) and Android/data is closed to others.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "replaynes/frontend.h"
#include "replaynes/product.h"

#include "platform/platform.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif
#include <SDL3/SDL.h>
#ifdef __ANDROID__
#include <jni.h>
#endif

namespace rnl {

#ifdef _WIN32
namespace {
bool shellExecute(const wchar_t* verb, const std::string& path) {
  int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  if (n <= 0) return false;
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
  for (wchar_t& c : w)
    if (c == L'/') c = L'\\';
  HINSTANCE r = ShellExecuteW(nullptr, verb, w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  return reinterpret_cast<INT_PTR>(r) > 32;
}
}  // namespace
#endif

#ifdef __ANDROID__
namespace {
// MainActivity.<method>(String) -> boolean, from any thread.
bool callActivity(const char* method, const std::string& arg) {
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity) return false;
  bool ok = false;
  jclass cls = env->GetObjectClass(activity);
  jmethodID open = env->GetMethodID(cls, method, "(Ljava/lang/String;)Z");
  if (open) {
    jstring path = env->NewStringUTF(arg.c_str());
    ok = env->CallBooleanMethod(activity, open, path);
    env->DeleteLocalRef(path);
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    ok = false;
  }
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
  return ok;
}
}  // namespace
#endif

bool openFolder(const std::string& dir) {
#ifdef _WIN32
  return shellExecute(L"explore", dir);
#elif defined(__ANDROID__)
  return callActivity("openFolder", dir);
#else
  return SDL_OpenURL(("file://" + dir).c_str());
#endif
}

bool openFile(const std::string& path) {
#ifdef _WIN32
  return shellExecute(L"open", path);
#else
  return SDL_OpenURL(("file://" + path).c_str());
#endif
}

#ifdef __ANDROID__
bool canImportFiles() { return true; }
bool canSetScreenOrientation() { return true; }
void setScreenOrientation(const std::string& orientation) { callActivity("setScreenOrientation", orientation); }
bool importFiles(const std::string& dir) { return callActivity("importFiles", dir); }
bool canShareLog() { return true; }
bool shareLog() {
  // The file main_android.cpp writes (docs/design/ANDROID.md, "Logs").
  const char* ext = SDL_GetAndroidExternalStoragePath();
  return ext && callActivity("shareLog", std::string(ext) + "/Logs/" RN_PRODUCT_NAME ".log");
}
#else
bool canImportFiles() { return false; }
bool canSetScreenOrientation() { return false; }
void setScreenOrientation(const std::string&) {}
bool importFiles(const std::string&) { return false; }
bool canShareLog() { return false; }
bool shareLog() { return false; }
#endif

#ifdef __ANDROID__
void setImportFolder(const std::string& dir) { callActivity("setRomDir", dir); }
#else
void setImportFolder(const std::string&) {}
#endif

// The folder picker's answer (any thread: SDL's callback, Java's UI thread), taken by the frame loop.
namespace {
std::mutex pickMutex;
bool pickDone = false;
std::string pickPath, pickError;

#if defined(_WIN32) || defined(__ANDROID__)  // a system picker answers (Linux: the app's own chooser)
void folderChosen(const std::string& path, const std::string& error) {
  std::lock_guard<std::mutex> lk(pickMutex);
  pickDone = true;
  pickPath = path;
  pickError = error;
}
#endif
}  // namespace

bool takeChosenFolder(std::string* path, std::string* error) {
  std::lock_guard<std::mutex> lk(pickMutex);
  if (!pickDone) return false;
  pickDone = false;
  *path = pickPath;
  *error = pickError;
  return true;
}

// ---- Search the Device's storage access (Android)
namespace {
std::mutex accessMutex;
bool accessAnswered = false, accessGranted = false;
}  // namespace

bool takeStorageAccessAnswer(bool* granted) {
  std::lock_guard<std::mutex> lk(accessMutex);
  if (!accessAnswered) return false;
  accessAnswered = false;
  *granted = accessGranted;
  return true;
}

#ifdef __ANDROID__
bool storageAccessGranted() { return callActivity("storageAccessGranted", ""); }
void requestStorageAccess() { callActivity("requestStorageAccess", ""); }

std::vector<std::string> deviceStorageRoots(std::vector<std::string>* skip) {
  std::vector<std::string> roots;
  // The device's own storage: the external files path up to /Android/data (/storage/emulated/<user>).
  const char* ext = SDL_GetAndroidExternalStoragePath();
  std::string own = ext ? ext : "";
  const size_t at = own.find("/Android/data/");
  own = at == std::string::npos ? std::string("/storage/emulated/0") : own.substr(0, at);
  roots.push_back(own);
  // Cards and USB drives: /storage/<volume id> (not "emulated", not "self").
  std::error_code ec;
  for (std::filesystem::directory_iterator it("/storage", ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name == "emulated" || name == "self") continue;
    std::error_code de;
    if (it->is_directory(de)) roots.push_back(it->path().string());
  }
  // Other apps' folders: closed to every app since Android 11, whatever the permission.
  if (skip)
    for (const std::string& r : roots) {
      skip->push_back(r + "/Android/data");
      skip->push_back(r + "/Android/obb");
    }
  return roots;
}
#else
bool storageAccessGranted() { return false; }
void requestStorageAccess() {}
std::vector<std::string> deviceStorageRoots(std::vector<std::string>*) { return {}; }
#endif

#if defined(_WIN32)
bool chooseFolder(const std::string& start) {
  SDL_ShowOpenFolderDialog(
      [](void*, const char* const* files, int) {
        if (!files) folderChosen("", SDL_GetError());
        else folderChosen(files[0] ? files[0] : "", "");
      },
      nullptr, nullptr, start.empty() ? nullptr : start.c_str(), false);
  return true;
}
#elif defined(__ANDROID__)
bool chooseFolder(const std::string& start) { return callActivity("chooseFolder", start); }
#else
bool chooseFolder(const std::string&) { return false; }
#endif

}  // namespace rnl

#ifdef __ANDROID__
// MainActivity.chooseFolder's answer: the folder's path, or "" with why (cancelled: both empty).
extern "C" JNIEXPORT void JNICALL Java_io_github_replaynes_android_MainActivity_nativeFolderChosen(JNIEnv* env, jclass,
                                                                                                 jstring path,
                                                                                                 jstring error) {
  auto str = [env](jstring s) {
    if (!s) return std::string();
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
  };
  rnl::folderChosen(str(path), str(error));
}

// RomImport.copyInto: whether a received file's name is one the library lists (the product's ROM extensions and
// its own filter, which goes by the name too: ReplayArcade's arcade sets). A share takes any type of file
// (docs/design/ANDROID.md "Sharing"), so a photo shared to the app must not land in the ROM folder.
extern "C" JNIEXPORT jboolean JNICALL Java_io_github_replaynes_android_RomImport_nativeIsRom(JNIEnv* env, jclass,
                                                                                         jstring name) {
  if (!name) return JNI_FALSE;
  const char* c = env->GetStringUTFChars(name, nullptr);
  const bool rom = c && rnf_library_is_rom_file(c) != 0;
  if (c) env->ReleaseStringUTFChars(name, c);
  return rom ? JNI_TRUE : JNI_FALSE;
}

// MainActivity.requestStorageAccess's answer.
extern "C" JNIEXPORT void JNICALL Java_io_github_replaynes_android_MainActivity_nativeStorageAccess(JNIEnv*, jclass,
                                                                                                  jboolean granted) {
  std::lock_guard<std::mutex> lk(rnl::accessMutex);
  rnl::accessAnswered = true;
  rnl::accessGranted = granted;
}
#endif
