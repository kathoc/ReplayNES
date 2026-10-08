// SPDX-License-Identifier: GPL-2.0-or-later
#include "app_model.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <system_error>

#include "host_clock.h"
#include "l10n.h"
#include "manifest.h"
#include "platform/platform.h"
#include "ui_features.h"

namespace fs = std::filesystem;

namespace rnl {

namespace {
std::string stem(const std::string& path) {
  fs::path p(path);
  return p.stem().string();
}
std::string fileName(const std::string& path) { return fs::path(path).filename().string(); }
double now1970() { return double(std::time(nullptr)); }
bool exists(const std::string& p) {
  std::error_code ec;
  return fs::exists(fs::symlink_status(p, ec));
}
}  // namespace

std::string chooserProjectFileName(const std::string& typed) {
  std::string out;
  for (unsigned char c : typed) {
    if (c < 0x20 || c == 0x7F || c == '/' || c == '\\') continue;
    out += char(c);
  }
  auto trim = [](std::string& s, const char* chars) {
    size_t a = s.find_first_not_of(chars);
    if (a == std::string::npos) {
      s.clear();
      return;
    }
    s = s.substr(a, s.find_last_not_of(chars) - a + 1);
  };
  trim(out, " ");
  const std::string ext = ".nesrec";
  if (out.size() >= ext.size()) {
    std::string tail = out.substr(out.size() - ext.size());
    for (char& ch : tail) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (tail == ext) out.resize(out.size() - ext.size());
  }
  trim(out, " .");
  return out.empty() ? std::string() : out + ext;
}

// ------------------------------------------------------------------ resume record

ResumeRec ResumeRec::from(const rnf_resume_record& r) {
  ResumeRec o;
  o.projectPath = r.project_path ? r.project_path : "";
  o.isTemp = r.is_temp != 0;
  o.romPath = r.rom_path ? r.rom_path : "";
  o.romSHA256 = r.rom_sha256 ? r.rom_sha256 : "";
  if (r.has_frame) o.frame = r.frame;
  if (r.has_at_take_end) o.atTakeEnd = r.at_take_end != 0;
  o.mode = r.mode;
  if (r.has_practice_slot) o.practiceSlot = r.practice_slot;
  o.hasContent = r.has_content != 0;
  o.updated = r.updated;
  return o;
}

rnf_resume_record ResumeRec::c() const {
  rnf_resume_record r{};
  r.version = RNF_RESUME_VERSION;
  r.project_path = projectPath.c_str();
  r.is_temp = isTemp;
  r.rom_path = romPath.c_str();
  r.rom_sha256 = romSHA256.c_str();
  r.has_frame = frame.has_value();
  r.frame = frame.value_or(0);
  r.has_at_take_end = atTakeEnd.has_value();
  r.at_take_end = atTakeEnd.value_or(false);
  r.mode = rnf_resume_mode(mode);
  r.has_practice_slot = practiceSlot.has_value();
  r.practice_slot = practiceSlot.value_or(0);
  r.has_content = hasContent;
  r.updated = updated;
  return r;
}

bool ResumeRec::sameState(const ResumeRec& o) const {
  rnf_resume_record a = c(), b = o.c();
  return rnf_resume_same_state(&a, &b) != 0;
}

// ------------------------------------------------------------------ setup

AppModel::AppModel(Paths paths, EmulationController* emu, LibraryModel* library, DialogHost* host)
    : paths_(std::move(paths)), emu_(emu), library_(library), host_(host) {
  writer_ = std::thread([this] { writerLoop(); });
}

AppModel::~AppModel() {
  {
    std::lock_guard<std::mutex> lk(wMutex_);
    wStop_ = true;
  }
  wCv_.notify_all();
  writer_.join();
  if (lock_) rnf_session_lock_release(lock_);
}

void AppModel::writerLoop() {
  for (;;) {
    std::optional<ResumeRec> r;
    uint64_t seq = 0;
    {
      std::unique_lock<std::mutex> lk(wMutex_);
      wCv_.wait(lk, [&] { return wStop_ || wPending_; });
      if (!wPending_ && wStop_) return;
      r = wRecord_;
      seq = wSeq_;
    }
    std::string file = paths_.resumeFile();
    if (r) {
      rnf_resume_record c = r->c();
      if (rnf_resume_write(file.c_str(), &c) != RN_OK)
        std::fprintf(stderr, "cannot write %s: %s\n", file.c_str(), rnf_last_error());
    } else {
      rnf_resume_clear(file.c_str());
    }
    {
      std::lock_guard<std::mutex> lk(wMutex_);
      if (wSeq_ == seq) wPending_ = false;  // else a newer record is written in the next round
    }
    wCv_.notify_all();
  }
}

void AppModel::flushWrites() {
  std::unique_lock<std::mutex> lk(wMutex_);
  wCv_.wait(lk, [&] { return !wPending_; });
}

void AppModel::writeResume(const std::optional<ResumeRec>& r) {
  if (!r) pendingResume_.reset();  // nothing to continue any more
  if (!persist_) return;
  lastResume_ = r;
  {
    std::lock_guard<std::mutex> lk(wMutex_);
    wRecord_ = r;
    if (wRecord_) wRecord_->updated = now1970();
    wSeq_ += 1;
    wPending_ = true;
  }
  wCv_.notify_all();
}

void AppModel::setupSessionPersistence(bool enabled) {
  if (!enabled) return;
  std::error_code ec;
  fs::create_directories(paths_.sessionRoot, ec);
  if (ec) {
    std::fprintf(stderr, "session folder unavailable (%s); sessions are not persisted\n", ec.message().c_str());
    return;
  }
  lock_ = rnf_session_lock_acquire(paths_.lockFile().c_str());
  if (!lock_) {
    std::fprintf(stderr, "another instance owns %s; this one does not resume\n", paths_.sessionRoot.c_str());
    return;
  }
  persist_ = true;
}

void AppModel::startup(const std::string& romArg, bool resume) {
  if (!romArg.empty()) {
    tryRom(romArg);
    return;
  }
  if (resume) loadPendingResume();
}

std::optional<AppModel::ContinueTarget> AppModel::continueTarget() const {
  if (emu_->session()) return std::nullopt;
  ContinueTarget t;
  if (pendingResume_) {
    t.projectPath = pendingResume_->projectPath;
    t.romPath = pendingResume_->romPath;
    t.romSHA256 = pendingResume_->romSHA256;
    t.isTemp = pendingResume_->isTemp;
    t.fromRecord = true;
    t.date = pendingResume_->updated;
    if (t.romSHA256.empty() || t.romPath.empty()) {
      ManifestInfo m = readManifest(t.projectPath);
      if (t.romSHA256.empty() && m.romSHA256 != "?") t.romSHA256 = m.romSHA256;
      if (t.romPath.empty() && m.romName != "?") t.projectName = stem(m.romName);
    }
    return t;
  }
  const LibraryProject* latest = nullptr;
  for (const LibraryProject& p : library_->allProjects())
    if (!latest || p.modified > latest->modified) latest = &p;
  if (!latest) return std::nullopt;
  t.projectPath = latest->path;
  t.romSHA256 = latest->romSHA256;
  t.projectName = latest->romName.empty() ? latest->name : stem(latest->romName);
  t.date = latest->modified;
  return t;
}

void AppModel::continueLast() {
  if (emu_->session()) return;
  if (pendingResume_) {
    ResumeRec r = *pendingResume_;
    openProject(r.projectPath, "", false, false, r);
    return;
  }
  if (std::optional<ContinueTarget> t = continueTarget()) continueProject(t->projectPath);
}

void AppModel::settlePendingTemp(std::function<void()> then) {
  // Idle on the library with a temporary session left (resumable by "Continue"): starting
  // something else asks first whether to save it (prepareTempSlot).
  if (emu_->session() || !persist_ || !exists(paths_.tempProject())) return then();
  prepareTempSlot(std::move(then));
}

// ------------------------------------------------------------------ play history

namespace {
double steadySeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

void AppModel::beginPlay(const std::string& sha256) {
  if (!playSHA_.empty() && playSHA_ == sha256) return flushPlay();  // the same session reopened (Save As)
  flushPlay();
  playSHA_ = sha256;
  playStart_ = now1970();
  playMark_ = steadySeconds();
  library_->recordPlay(playSHA_, playStart_, 0);
}

void AppModel::flushPlay() {
  if (playSHA_.empty()) return;
  double t = steadySeconds();
  library_->recordPlay(playSHA_, playStart_, t - playMark_);
  playMark_ = t;
}

bool AppModel::isTempPath(const std::string& p) const {
  return persist_ && !p.empty() && rnf_paths_equal(p.c_str(), paths_.tempProject().c_str());
}

void AppModel::error(const std::string& title, const std::string& message) {
  Dialog d;
  d.title = title;
  d.message = message;
  d.buttons = {TR("OK")};
  host_->showDialog(std::move(d));
}

std::string AppModel::romName() const {
  if (!emu_->session()) return "";
  return stem(rn_session_rom_path(emu_->session()));
}

std::string AppModel::windowTitle() const {
  const EmuStatus& st = emu_->status();
  if (!st.hasSession) return "ReplayNES";
  if (!st.projectPath.empty() && !isTempPath(st.projectPath)) return fileName(st.projectPath) + (st.unsaved ? " •" : "");
  return fileName(st.romPath) + TR(" (Unsaved)");
}

// ------------------------------------------------------------------ confirm / temp slot

void AppModel::confirmDiscardIfNeeded(std::function<void()> then) {
  rn_session* s = emu_->session();
  if (!s) return then();
  if (current_ && current_->isTemp) {
    if (!rnf_session_has_recorded_content(s)) return then();
    Dialog d;
    d.title = TR("Do you want to save?");
    d.message = TR("This session hasn’t been saved as a project yet (it is stored temporarily). If you don’t save, the "
                   "temporary data will be discarded.");
    d.buttons = {TR("Save…"), TR("Don’t Save"), TR("Cancel")};
    d.cancelIndex = 2;
    d.destructiveIndex = 1;
    d.onResult = [this, then](int b, bool) {
      if (b == 0) saveTempAs(then);
      else if (b == 1) then();
    };
    return host_->showDialog(std::move(d));
  }
  bool inMemory = !*rn_session_project_dir(s);
  if (!(rn_session_has_unsaved_changes(s) || (inMemory && rn_take_length(s) > 0))) return then();
  Dialog d;
  d.title = TR("The current project has unsaved changes");
  d.message = TR("Do you want to save?");
  d.buttons = {TR("Save"), TR("Don’t Save"), TR("Cancel")};
  d.cancelIndex = 2;
  d.destructiveIndex = 1;
  d.onResult = [this, then](int b, bool) {
    if (b == 0) {
      if (emu_->save()) then();
    } else if (b == 1) {
      then();
    }
  };
  host_->showDialog(std::move(d));
}

void AppModel::releaseSession() {
  flushPlay();
  playSHA_.clear();
  emu_->install(nullptr);
  current_.reset();
}

void AppModel::removeTempProject() {
  std::error_code ec;
  if (exists(paths_.tempProject())) {
    fs::remove_all(paths_.tempProject(), ec);
    if (ec) error(TR("Couldn’t delete the temporary data"), paths_.tempProject() + "\n" + ec.message());
  }
  if (lastResume_ && lastResume_->isTemp) writeResume(std::nullopt);
}

bool AppModel::moveTempProject(const std::string& dest, std::string* err) {
  std::error_code ec;
  fs::rename(paths_.tempProject(), dest, ec);
  if (!ec) return true;
  // Another file system (session folder vs. Documents): copy, then remove the temporary one.
  if (!copyTree(paths_.tempProject(), dest, err)) return false;
  fs::remove_all(paths_.tempProject(), ec);
  return true;
}

void AppModel::askProjectDestination(const std::string& name, std::function<void(const std::string&)> then) {
  ChooserRequest r;
  r.mode = ChooserRequest::Mode::saveProject;
  r.title = TR("Save Project");
  r.root = library_->projectsDir();
  r.defaultName = name;
  r.onChosen = [this, then](const std::string& dest) {
    if (isTempPath(dest)) {
      error(TR("Couldn’t save"), TR("You can’t save to the temporary location."));
      return;
    }
    if (exists(dest)) {
      std::string err;
      if (!trash(dest, &err)) {
        error(TR("Couldn’t save"), TRF("Couldn’t replace the existing item: %@", {dest}) + "\n" + err);
        return;
      }
    }
    then(dest);
  };
  host_->showChooser(std::move(r));
}

void AppModel::prepareTempSlot(std::function<void()> then) {
  if (current_ && current_->isTemp) {
    releaseSession();
    removeTempProject();
    return then();
  }
  if (!exists(paths_.tempProject())) return then();
  rnf_resume_record rec{};
  int found = 0;
  bool pointsHere = false, hasContent = true;
  if (rnf_resume_read(paths_.resumeFile().c_str(), &rec, &found) == RN_OK && found) {
    pointsHere = rec.is_temp != 0;
    hasContent = rec.has_content != 0;
  }
  rnf_resume_record_clear(&rec);
  if (pointsHere && !hasContent) {
    removeTempProject();
    writeResume(std::nullopt);
    return then();
  }
  ManifestInfo m = readManifest(paths_.tempProject());
  Dialog d;
  d.title = TR("An unsaved previous session remains");
  d.message = TRF("Do you want to save the previous session (ROM: %@)? If you don’t save, it will be discarded.", {m.romName});
  d.buttons = {TR("Save…"), TR("Don’t Save"), TR("Cancel")};
  d.cancelIndex = 2;
  d.destructiveIndex = 1;
  d.onResult = [this, then, pointsHere, m](int b, bool) {
    if (b == 0) {
      askProjectDestination(stem(m.romName), [this, then, pointsHere](const std::string& dest) {
        std::string err;
        if (!moveTempProject(dest, &err)) {
          error(TR("Couldn’t save"), dest + "\n" + err);
          return;
        }
        library_->refresh();
        if (pointsHere) writeResume(std::nullopt);
        then();
      });
    } else if (b == 1) {
      removeTempProject();
      if (pointsHere) writeResume(std::nullopt);
      then();
    }
  };
  host_->showDialog(std::move(d));
}

// ------------------------------------------------------------------ create / open / install

void AppModel::playFromLibrary(const LibraryROM& rom) {
  LibraryROM r = rom;
  confirmDiscardIfNeeded([this, r] {
    settlePendingTemp([this, r] {
      if (!library_->ensureFolders()) {
        error(TR("The library folder isn’t available"), library_->folderError());
        return;
      }
      char* dir = rnf_library_new_project_path(library_->projectsDir().c_str(), r.name.c_str(), now1970());
      std::string d = dir ? dir : "";
      rnf_string_free(dir);
      createSession(r.path, d, false);
    });
  });
}

void AppModel::tryRom(const std::string& romPath) {
  confirmDiscardIfNeeded([this, romPath] {
    if (!persist_) return createSession(romPath, "", true);
    prepareTempSlot([this, romPath] { createSession(romPath, paths_.tempProject(), true); });
  });
}

void AppModel::continueProject(const std::string& projectPath) {
  confirmDiscardIfNeeded([this, projectPath] {
    settlePendingTemp([this, projectPath] { openProject(projectPath, "", false, false, std::nullopt); });
  });
}

void AppModel::openProjectChooser() {
  confirmDiscardIfNeeded([this] {
    ChooserRequest r;
    r.mode = ChooserRequest::Mode::openProject;
    r.title = TR("Open Project");
    r.root = library_->root();
    r.startDir = library_->projectsDir();
    r.onChosen = [this](const std::string& path) { openProject(path, "", false, false, std::nullopt); };
    host_->showChooser(std::move(r));
  });
}

void AppModel::createSession(const std::string& rom, const std::string& projectDir, bool isTemp) {
  bool dirExisted = projectDir.empty() || exists(projectDir);
  if (isTemp && persist_) {
    std::error_code ec;
    fs::remove_all(projectDir, ec);  // made room by prepareTempSlot
    dirExisted = false;
  }
  rn_session* s = nullptr;
  rn_status st = rn_session_new(rom.c_str(), projectDir.empty() ? nullptr : projectDir.c_str(), nullptr, &s);
  if (st != RN_OK) {
    std::string msg = rn_last_error();
    // A project folder this call created is removed again; existing folders are never touched.
    if (!projectDir.empty() && !dirExisted) {
      std::error_code ec;
      fs::remove_all(projectDir, ec);
    }
    std::string hint = st == RN_ERR_ROM_INVALID ? std::string("\n") + TR("This ROM can’t be loaded (unsupported mapper or invalid file).") : "";
    error(TR("Couldn’t create the project"), msg + hint);
    return;
  }
  install(s, false, std::nullopt, true);
  if (!projectDir.empty() && !isTemp) library_->refresh();
}

void AppModel::openProject(const std::string& path, const std::string& romOverride, bool dropCorrupt, bool dropPractice,
                           std::optional<ResumeRec> resume) {
  rn_session_options opt;
  rn_session_options_init(&opt);
  opt.open_flags = (dropCorrupt ? RN_OPEN_DROP_CORRUPT_STATES : 0u) | (dropPractice ? RN_OPEN_DROP_CORRUPT_PRACTICE : 0u);
  rn_session* s = nullptr;
  rn_status st = rn_session_open(path.c_str(), romOverride.empty() ? nullptr : romOverride.c_str(), &opt, &s);
  if (st != RN_OK) {
    handleOpenError(st, rn_last_error(), path, romOverride, dropCorrupt, dropPractice, resume);
    return;
  }
  uint32_t dropped = rn_practice_dropped_slots(s);
  install(s, rn_session_recovered(s) != 0, resume, true);
  if (dropped) {
    std::string names;
    for (int i = 0; i < RN_PRACTICE_SLOTS; ++i) {
      if (!(dropped & (1u << i))) continue;
      if (!names.empty()) names += TR(", ");
      names += TRF("Section %lld", {i + 1});
    }
    error(TR("Discarded damaged practice sections"),
          TRF("These practice sections (A/B) couldn’t be loaded and were removed: %@\n\nYour takes (recordings) are not "
              "affected. Set A and B again if needed.",
              {names}));
  }
}

void AppModel::handleOpenError(rn_status st, const std::string& message, const std::string& path,
                               const std::string& romOverride, bool dropCorrupt, bool dropPractice,
                               std::optional<ResumeRec> resume) {
  ManifestInfo m = readManifest(path);
  auto failed = [this, resume] {
    if (resume) resumeFailed(*resume);
  };
  Dialog d;
  switch (st) {
    case RN_ERR_ROM_NOT_FOUND:
    case RN_ERR_ROM_MISMATCH: {
      if (st == RN_ERR_ROM_NOT_FOUND) {
        d.title = TR("ROM not found");
        d.message = TRF(
            "This project’s ROM “%@” is no longer at its original location.\nOriginal location: %@\n\nPlease locate the "
            "same ROM (a file with a matching SHA-256).\nSHA-256: %@",
            {m.romName, m.romPath, m.romSHA256});
      } else {
        d.title = TR("ROM doesn’t match");
        d.message = TRF(
            "The selected ROM differs from the one this project was recorded with.\nRequired ROM: %@\nSHA-256: "
            "%@\n\nDetails: %@",
            {m.romName, m.romSHA256, message});
      }
      d.buttons = {TR("Locate ROM…"), TR("Cancel")};
      d.cancelIndex = 1;
      d.onResult = [=](int b, bool) {
        if (b != 0) return failed();
        ChooserRequest r;
        r.mode = ChooserRequest::Mode::openROM;
        r.title = TRF("Locate “%@”", {m.romName});
        r.root = library_->root();
        r.startDir = library_->romDir();
        r.onChosen = [=](const std::string& rom) { openProject(path, rom, dropCorrupt, dropPractice, resume); };
        r.onCancel = failed;
        host_->showChooser(std::move(r));
      };
      break;
    }
    case RN_ERR_CORE_MISMATCH:
      d.title = TR("This project was recorded with a different emulation core");
      d.message = TRF(
          "To keep replays exact, this version can’t open it (no automatic conversion).\nProject core: %@\nThis app’s "
          "core: %@\n\nOpen it with the version of ReplayNES it was recorded with. See docs/COMPATIBILITY.md for details.",
          {m.coreCompatID, std::string(rn_core_compat_id())});
      d.buttons = {TR("OK")};
      d.onResult = [=](int, bool) { failed(); };
      break;
    case RN_ERR_CORRUPT:
      if (message.find("practice") != std::string::npos && !dropPractice) {
        d.title = TR("The practice section (A/B) data is damaged");
        d.message = message + "\n\n" +
                    TR("You can open it by discarding only the damaged practice sections (their A/B points are lost). "
                       "Your takes (recordings) are not changed.");
        d.buttons = {TR("Discard Damaged Sections and Open"), TR("Cancel")};
        d.cancelIndex = 1;
        d.onResult = [=](int b, bool) {
          if (b == 0) openProject(path, romOverride, dropCorrupt, true, resume);
          else failed();
        };
      } else if (!dropCorrupt) {
        d.title = TR("The project files are damaged");
        d.message = message + "\n\n" +
                    TR("If only checkpoints (states kept for speed) are damaged, you can open the project by discarding "
                       "them. The input history (the source of truth) is not changed.");
        d.buttons = {TR("Discard Damaged Checkpoints and Open"), TR("Cancel")};
        d.cancelIndex = 1;
        d.onResult = [=](int b, bool) {
          if (b == 0) openProject(path, romOverride, true, dropPractice, resume);
          else failed();
        };
      } else {
        d.title = TR("The project files are damaged");
        d.message = message;
        d.buttons = {TR("OK")};
        d.onResult = [=](int, bool) { failed(); };
      }
      break;
    case RN_ERR_UNSUPPORTED_FORMAT:
      d.title = TR("This project was made with a newer version of ReplayNES");
      d.message = std::string(TR("Please update the app.")) + "\n" + message;
      d.buttons = {TR("OK")};
      d.onResult = [=](int, bool) { failed(); };
      break;
    default:
      d.title = TR("Couldn’t open the project");
      d.message = std::string(rn_status_name(st)) + ": " + message;
      d.buttons = {TR("OK")};
      d.onResult = [=](int, bool) { failed(); };
      break;
  }
  host_->showDialog(std::move(d));
}

void AppModel::install(rn_session* s, bool recovered, const std::optional<ResumeRec>& resume, bool resumeNotice) {
  std::string dir = rn_session_project_dir(s);
  bool isTemp = isTempPath(dir);
  // A temporary session being replaced was confirmed (saved elsewhere, or Don't Save / empty).
  if (current_ && current_->isTemp && !isTemp) {
    releaseSession();
    removeTempProject();
  }
  ResumeRec record;
  record.projectPath = dir;
  record.isTemp = isTemp;
  record.romPath = rn_session_rom_path(s);
  record.romSHA256 = rn_session_rom_sha256(s);
  record.frame = rn_frame(s);
  record.atTakeEnd = rn_frame(s) >= rn_take_length(s);
  record.mode = rn_get_mode(s) == RN_MODE_REPLAY ? RNF_RESUME_MODE_REPLAY : RNF_RESUME_MODE_RECORD;
  record.hasContent = rnf_session_has_recorded_content(s) != 0;
  if (resume) {
    rnf_resume_record c = resume->c();
    uint64_t f = 0;
    if (rnf_resume_target_frame(&c, rn_take_length(s), &f)) record.frame = f;
    record.atTakeEnd = record.frame == rn_take_length(s);
    if (resume->mode != RNF_RESUME_MODE_NONE) record.mode = resume->mode;
    record.practiceSlot = resume->practiceSlot;
  }
  current_ = Identity{dir, isTemp, record.romSHA256};
  pendingResume_.reset();  // superseded by this session (its record is written below)
  beginPlay(record.romSHA256);
  // Practice is never persisted: a session left while practicing reopens with the panel shown.
  practicePanelRequested_ = resume && resume->practiceSlot.has_value();
  emu_->tempSession = isTemp;
  emu_->install(s);
  if (resume) {
    rnf_resume_record c = resume->c();
    emu_->applyResume(c);
  }
  writeResume(dir.empty() ? std::nullopt : std::optional<ResumeRec>(record));
  if (resume) {
    if (resumeNotice) host_->notice(TR("Resumed where you left off"));
  } else if (recovered) {
    Dialog d;
    d.title = TR("Unsaved work was restored");
    d.message = TR("ReplayNES didn’t quit normally last time, so the recording up to the last autosave was restored from "
                   "the journal. Please review it and save.");
    d.buttons = {TR("OK")};
    host_->showDialog(std::move(d));
  }
}

// ------------------------------------------------------------------ save

void AppModel::save() {
  if (!emu_->session()) return;
  if (isTempSession()) return saveTempAs([] {});
  if (!*rn_session_project_dir(emu_->session())) return saveAs();
  if (emu_->save()) host_->notice(TR("Saved"));
}

void AppModel::saveAs() {
  if (!emu_->session()) return;
  if (isTempSession()) return saveTempAs([] {});
  askProjectDestination(romName(), [this](const std::string& dir) {
    rn_session* s = emu_->session();
    if (!s) return;
    if (rn_session_save_as(s, dir.c_str()) != RN_OK) {
      error(TR("Couldn’t save"), rn_last_error());
      return;
    }
    if (current_) current_ = Identity{dir, false, current_->romSHA256};
    lastResume_.reset();  // rewritten with the new path on the next update
    library_->refresh();
    host_->notice(TR("Saved"));
  });
}

void AppModel::saveTempAs(std::function<void()> then) {
  if (!isTempSession()) return;
  askProjectDestination(romName(), [this, then](const std::string& dest) {
    rn_session* s = emu_->session();
    if (!s) return;
    bool practicing = emu_->status().practicing;
    if (rn_session_save(s) != RN_OK) {
      error(TR("Couldn’t save (your work is still in the temporary location)"), rn_last_error());
      return;
    }
    emu_->install(nullptr);
    current_.reset();
    std::string target = dest, moveError;
    if (!moveTempProject(dest, &moveError)) target = paths_.tempProject();
    // The full save above wrote the cursor and take mode: the project reopens where it was.
    rn_session* reopened = nullptr;
    if (rn_session_open(target.c_str(), nullptr, nullptr, &reopened) == RN_OK) {
      ResumeRec r;
      r.projectPath = target;
      r.isTemp = !moveError.empty();
      if (practicing) r.practiceSlot = -1;
      install(reopened, false, r, false);
    } else {
      error(TR("Couldn’t open the project"), target + "\n" + rn_last_error());
    }
    if (!moveError.empty()) {
      error(TR("Couldn’t save (your work is still in the temporary location)"), dest + "\n" + moveError);
      return;
    }
    library_->refresh();
    host_->notice(TR("Saved"));
    then();
  });
}

void AppModel::closeProject() {
  confirmDiscardIfNeeded([this] {
    bool wasTemp = isTempSession();
    releaseSession();
    if (wasTemp) removeTempProject();
    writeResume(std::nullopt);
  });
}

// ------------------------------------------------------------------ reset

void AppModel::resetProjectPrompt() {
  if (!emu_->session() || !current_) return;
  bool saved = !current_->projectPath.empty() && !current_->isTemp;
  Dialog d;
  d.title = TR("Reset this project?");
  d.message = TR("The recording starts over from power-on: every take, bookmark and the take history are deleted. The ROM "
                 "and the project location stay the same.");
  d.message += "\n\n";
  d.message += !saved     ? TR("This session isn’t saved as a project, so no backup is made.")
               : kRecycleBin ? TR("A backup copy of the project is moved to the Recycle Bin first, so you can still recover it from there.")
                             : TR("A backup copy of the project is moved to the Trash first, so you can still recover it from there.");
  d.checkbox = TR("Keep A/B repeat sections");
  d.checked = true;
  d.buttons = {TR("Reset"), TR("Cancel")};
  d.destructiveIndex = 0;
  d.cancelIndex = 1;
  d.onResult = [this, saved](int b, bool keep) {
    if (b == 0) performProjectReset(keep, saved);
  };
  host_->showDialog(std::move(d));
}

void AppModel::performProjectReset(bool keepSlots, bool backup) {
  std::function<std::string(const std::string&)> backupFn;
  if (backup) {
    backupFn = [this](const std::string& dir) -> std::string {
      char* p = rnf_backup_path(dir.c_str(), now1970(), nullptr, nullptr);
      std::string copy = p ? p : dir + " backup";
      rnf_string_free(p);
      std::string err;
      if (!copyTree(dir, copy, &err)) {
        // The project's folder is not writable: copy into the temporary folder instead.
        std::error_code ec;
        std::string tmp = fs::temp_directory_path(ec).string() + "/replaynes-backup-" +
                          std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        fs::create_directories(tmp, ec);
        copy = tmp + "/" + fileName(copy);
        if (!copyTree(dir, copy, &err)) return err;
      }
      if (!trash(copy, &err)) {
        std::error_code ec;
        fs::remove_all(copy, ec);
        return err;
      }
      return "";
    };
  }
  std::string err = emu_->resetProject(keepSlots, backupFn);
  if (!err.empty()) {
    error(TR("Couldn’t reset the project"), err);
    return;
  }
  // The resume record restarts like a new project's (frame 0, record mode).
  if (current_ && !current_->projectPath.empty()) {
    bool hasSlots = false;
    for (const SlotInfo& s : emu_->structure().slots) hasSlots = hasSlots || s.hasA;
    ResumeRec r;
    r.projectPath = current_->projectPath;
    r.isTemp = current_->isTemp;
    r.romPath = emu_->status().romPath;
    r.romSHA256 = current_->romSHA256;
    r.frame = 0;
    r.atTakeEnd = true;
    r.mode = RNF_RESUME_MODE_RECORD;
    r.hasContent = keepSlots && hasSlots;
    writeResume(r);
  }
  host_->notice(!backup     ? TR("Project reset")
                : kRecycleBin ? TR("Project reset (a backup is in the Recycle Bin)")
                              : TR("Project reset (a backup is in the Trash)"));
}

// ------------------------------------------------------------------ resume

void AppModel::loadPendingResume() {
  pendingResume_.reset();
  if (!persist_) return;
  rnf_resume_record r{};
  rnf_resume_decision d = rnf_resume_decide(paths_.sessionRoot.c_str(), 0, nullptr, &r);
  ResumeRec rec = ResumeRec::from(r);
  rnf_resume_record_clear(&r);
  switch (d) {
    case RNF_RESUME_NONE: return;
    case RNF_RESUME_PROJECT_MISSING:
      writeResume(std::nullopt);
      error(TR("The last project can’t be found"),
            TRF("“%@” was moved or deleted, so ReplayNES couldn’t resume where you left off.\nOriginal location: "
                "%@\n\nChoose it again from the library, or use “Open Project…”.",
                {fileName(rec.projectPath), rec.projectPath}));
      return;
    case RNF_RESUME_RESUME: pendingResume_ = rec; return;  // the library's "Continue" card
  }
}

void AppModel::resumeFailed(const ResumeRec& r) {
  error(TR("Couldn’t resume where you left off"),
        r.isTemp ? TRF("The unsaved previous session has been kept (ReplayNES will try to resume it again at the next "
                       "launch).\n\nMove the ROM back to its original location, or choose it from the library. When you "
                       "start another game, you can choose to save or discard the previous session.\nTemporary location: %@",
                       {paths_.tempProject()})
                 : TRF("The project “%@” has not been changed. You can open it again with “Continue” in the library or "
                       "“Open Project…”.",
                       {fileName(r.projectPath)}));
}

void AppModel::updateResumeRecord() {
  if (!persist_ || !current_) return;
  const EmuStatus& st = emu_->status();
  if (!st.hasSession || current_->projectPath.empty() || !rnf_paths_equal(st.projectPath.c_str(), current_->projectPath.c_str()))
    return;
  ResumeRec r;
  r.projectPath = current_->projectPath;
  r.isTemp = current_->isTemp;
  r.romPath = st.romPath;
  r.romSHA256 = current_->romSHA256;
  r.frame = st.frame;
  r.atTakeEnd = st.frame >= st.takeLength;
  r.mode = st.practicing ? RNF_RESUME_MODE_NONE : st.recording ? RNF_RESUME_MODE_RECORD : RNF_RESUME_MODE_REPLAY;
  if (st.practicing) r.practiceSlot = st.practiceSlot;
  r.hasContent = emu_->structure().hasContent();
  if (lastResume_ && lastResume_->sameState(r)) return;
  writeResume(r);
}

void AppModel::update(double now) {
  if (now >= nextPlayFlush_) {
    nextPlayFlush_ = now + 60.0;
    flushPlay();  // play time survives a crash (within a minute)
  }
  if (now < nextResumeUpdate_) return;
  nextResumeUpdate_ = now + 1.0;
  updateResumeRecord();
}

void AppModel::persistNow() {
  if (!persist_ || !current_) return;
  std::string err = emu_->flushForResume(false);
  if (!err.empty()) std::fprintf(stderr, "persist: %s\n", err.c_str());
  updateResumeRecord();
}

void AppModel::quitNow() {
  flushPlay();
  if (current_ && persist_) {
    std::string err = emu_->flushForResume(current_->isTemp);
    if (!err.empty()) std::fprintf(stderr, "saving at quit failed: %s\n", err.c_str());
    updateResumeRecord();
  } else if (emu_->session() && *rn_session_project_dir(emu_->session())) {
    emu_->flushForResume(false);  // second instance: at least the journal
  }
  flushWrites();
  emu_->closeSession();
}

void AppModel::requestQuit(std::function<void()> quit) {
  flushPlay();
  if (!persist_) {
    confirmDiscardIfNeeded([this, quit] {
      emu_->closeSession();
      quit();
    });
    return;
  }
  if (!current_) return quit();
  std::string err = emu_->flushForResume(current_->isTemp);
  updateResumeRecord();
  flushWrites();
  if (err.empty()) {
    emu_->closeSession();
    return quit();
  }
  Dialog d;
  d.title = TR("Your work couldn’t be saved");
  d.message = std::string(TR("If you quit now, anything recorded after the last autosave may be lost.")) + "\n\n" + err;
  d.buttons = {TR("Don’t Quit"), TR("Quit Anyway")};
  d.cancelIndex = 0;
  d.destructiveIndex = 1;
  d.onResult = [this, quit](int b, bool) {
    if (b == 1) {
      emu_->closeSession();
      quit();
    }
  };
  host_->showDialog(std::move(d));
}

}  // namespace rnl
