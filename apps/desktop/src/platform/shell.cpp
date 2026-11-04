// openFolder / openFile (platform.h): Explorer / the shell on Windows, the desktop's file manager or
// default application through SDL elsewhere. On Android a folder opens in the system Files app through the
// app's documents provider (MainActivity.openFolder, docs/design/ANDROID.md): a file:// URI cannot leave
// the app there (FileUriExposedException, which SDL_OpenURL swallows) and Android/data is closed to others.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <mutex>
#include <string>

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
bool importFiles(const std::string& dir) { return callActivity("importFiles", dir); }
bool canShareLog() { return true; }
bool shareLog() {
  // The file main_android.cpp writes (docs/design/ANDROID.md, "Logs").
  const char* ext = SDL_GetAndroidExternalStoragePath();
  return ext && callActivity("shareLog", std::string(ext) + "/Logs/" RN_PRODUCT_NAME ".log");
}
#else
bool canImportFiles() { return false; }
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
#endif
