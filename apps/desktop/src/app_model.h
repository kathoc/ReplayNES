// Project lifecycle of the desktop frontend, exactly like the macOS AppModel
// (apps/macos/Sources/App/AppModel.swift + ProjectReset.swift):
//  * Library "Play" creates <ROM name> <yyyy-MM-dd HHmm>.nesrec in Projects/ (no save dialog);
//    "Continue" opens a library project; a ROM opened without a project (--rom, "Try") lives in
//    the temporary project $XDG_DATA_HOME/ReplayNES/Session/current.nesrec.
//  * Always-on session persistence (the shared core's resume record + single-instance lock): the
//    session is autosaved while playing, persisted at quit without asking (temporary project: full
//    save; project: journal) and reopened at the next launch where it was, paused.
//  * Before another ROM / project replaces the session: "Do you want to save?" (a temporary
//    session with recorded content: Save… / Don't Save; a project with unsaved changes: Save /
//    Don't Save); a temporary project left from an earlier run is offered for saving first.
//  * Save / Save As (file chooser rooted at ~/Documents/ReplayNES/Projects), Reset Project (backup
//    copy to the freedesktop Trash for saved projects), open errors with their fixes (Locate ROM,
//    discard damaged checkpoints / practice sections).
// Frame-loop thread only. Dialogs are asynchronous (DialogHost): flows continue in callbacks.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "dialogs.h"
#include "emulation.h"
#include "library.h"
#include "paths.h"
#include "platform/platform.h"
#include "replaynes/frontend.h"

namespace rnl {

/// rnf_resume_record with owned strings.
struct ResumeRec {
  std::string projectPath;
  bool isTemp = false;
  std::string romPath, romSHA256;
  std::optional<uint64_t> frame;
  std::optional<bool> atTakeEnd;
  int mode = RNF_RESUME_MODE_NONE;
  std::optional<int> practiceSlot;
  bool hasContent = true;
  double updated = 0;

  static ResumeRec from(const rnf_resume_record& r);
  /// The C record (strings point into this object).
  rnf_resume_record c() const;
  bool sameState(const ResumeRec& o) const;
};

class AppModel {
 public:
  AppModel(Paths paths, EmulationController* emu, LibraryModel* library, DialogHost* host);
  ~AppModel();

  /// Moves a project to the Trash / Recycle Bin (Reset Project backup, Save As replacing a
  /// project): trashItem (platform/platform.h); tests point it at a scratch folder.
  std::function<bool(const std::string& path, std::string* error)> trash = trashItem;

  /// Launch: the session folder lock (false: another instance owns it; nothing is persisted or
  /// resumed then and temporary sessions stay in memory).
  void setupSessionPersistence(bool enabled);
  bool persistSessions() const { return persist_; }
  /// Launch: --rom opens a temporary session (explicit open, no resume); otherwise the last
  /// session is resumed (paused) if there is one.
  void startup(const std::string& romArg, bool resume);

  // Library / projects.
  void playFromLibrary(const LibraryROM& rom);
  void tryRom(const std::string& romPath);  // temporary session
  void continueProject(const std::string& projectPath);
  void openProjectChooser();
  void save();
  void saveAs();
  void closeProject();
  void resetProjectPrompt();

  /// Quit from the menu: persists (no save prompt); asks only if persisting failed.
  void requestQuit(std::function<void()> quit);
  /// Window closed / SIGTERM (Steam closing the game): persists without asking.
  void quitNow();
  /// The window lost the focus (Steam overlay, another app): persist right away.
  void persistNow();
  /// Frame thread, in the slack: resume record (~1/s).
  void update(double now);

  bool hasSession() const { return emu_->session() != nullptr; }
  bool isTempSession() const { return current_ && current_->isTemp; }
  std::string windowTitle() const;
  std::string romName() const;  // of the current session
  const Paths& paths() const { return paths_; }
  /// A resumed session was left while practicing: the UI shows the practice panel (once).
  bool takePracticePanelRequest() {
    bool r = practicePanelRequested_;
    practicePanelRequested_ = false;
    return r;
  }

 private:
  struct Identity {
    std::string projectPath;
    bool isTemp = false;
    std::string romSHA256;
  };
  void confirmDiscardIfNeeded(std::function<void()> then);
  void createSession(const std::string& rom, const std::string& projectDir, bool isTemp);
  void openProject(const std::string& path, const std::string& romOverride, bool dropCorrupt, bool dropPractice,
                   std::optional<ResumeRec> resume);
  void handleOpenError(rn_status st, const std::string& message, const std::string& path, const std::string& romOverride,
                       bool dropCorrupt, bool dropPractice, std::optional<ResumeRec> resume);
  void install(rn_session* s, bool recovered, const std::optional<ResumeRec>& resume, bool resumeNotice);
  void releaseSession();
  void removeTempProject();
  void prepareTempSlot(std::function<void()> then);
  void askProjectDestination(const std::string& name, std::function<void(const std::string&)> then);
  void saveTempAs(std::function<void()> then);
  void resumeLastSession();
  void resumeFailed(const ResumeRec& r);
  void performProjectReset(bool keepSlots, bool backup);
  void updateResumeRecord();
  void writeResume(const std::optional<ResumeRec>& r);
  void error(const std::string& title, const std::string& message);
  bool isTempPath(const std::string& p) const;
  bool moveTempProject(const std::string& dest, std::string* err);

  Paths paths_;
  EmulationController* emu_;
  LibraryModel* library_;
  DialogHost* host_;
  bool persist_ = false;
  rnf_session_lock* lock_ = nullptr;
  std::optional<Identity> current_;
  std::optional<ResumeRec> lastResume_;
  bool practicePanelRequested_ = false;
  double nextResumeUpdate_ = 0;
  // resume.json writes off the frame thread (latest wins).
  std::thread writer_;
  std::mutex wMutex_;
  std::condition_variable wCv_;
  bool wPending_ = false, wStop_ = false;
  uint64_t wSeq_ = 0;
  std::optional<ResumeRec> wRecord_;
  void writerLoop();
  void flushWrites();
};

}  // namespace rnl
