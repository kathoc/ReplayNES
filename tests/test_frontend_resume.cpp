// Frontend core: session resume record (format, atomic write, corrupt records), the launch
// decision, the single-instance lock and resume on a real engine session (force quit / clean
// quit). Ported from the macOS SessionResumeTests.
#include <cstdio>
#include <fstream>
#include <vector>

#include "support/FrontendTestUtil.h"
#include "support/rn_test.h"

using namespace rnft;

namespace {

struct Folder {
  std::string tmp = rntest::tempDir("frontend-resume");
  std::string root = rn::fs::join(tmp, "Session");
  std::string temp = rn::fs::join(root, RNF_SESSION_TEMP_PROJECT);
  std::string resume = rn::fs::join(root, RNF_SESSION_RESUME_FILE);
  std::string lock = rn::fs::join(root, RNF_SESSION_LOCK_FILE);
  std::string rom;
  Folder() {
    rn::fs::createDirs(root);
    rom = rntest::writeTestRom(tmp, "Test ROM.nes");
  }
};

rnf_resume_record record(const char* project, bool isTemp) {
  rnf_resume_record r{};
  r.version = RNF_RESUME_VERSION;
  r.project_path = project;
  r.is_temp = isTemp;
  r.rom_path = "";
  r.rom_sha256 = "";
  r.mode = RNF_RESUME_MODE_NONE;
  r.has_content = 1;
  r.updated = 1800000000;
  return r;
}

void writeText(const std::string& path, const std::string& text) {
  std::ofstream(path, std::ios::binary) << text;
}

rn_session* newSession(const std::string& rom, const std::string& dir) {
  rn_session_options o;
  rn_session_options_init(&o);
  rn_session* s = nullptr;
  rn_session_new(rom.c_str(), dir.empty() ? nullptr : dir.c_str(), &o, &s);
  return s;
}

rn_session* openSession(const std::string& dir) {
  rn_session_options o;
  rn_session_options_init(&o);
  rn_session* s = nullptr;
  rn_session_open(dir.c_str(), nullptr, &o, &s);
  return s;
}

std::vector<uint64_t> recordFrames(rn_session* s, int frames) {
  std::vector<uint64_t> hashes;
  rn_set_mode(s, RN_MODE_RECORD);
  for (int f = 0; f < frames; ++f) {
    uint8_t p1 = uint8_t((f % 11 == 0 ? RN_BTN_A : 0) | (f % 5 == 0 ? RN_BTN_RIGHT : 0));
    rn_step(s, p1, 0, 0, nullptr);
    hashes.push_back(rn_state_hash(s));
  }
  return hashes;
}

}  // namespace

TEST_CASE("temp project detection") {
  Folder f;
  CHECK(rnf_paths_equal(f.temp.c_str(), f.temp.c_str()));
  CHECK(rnf_paths_equal(f.temp.c_str(), (f.temp + "/").c_str()));
  CHECK(rnf_paths_equal(f.temp.c_str(), (f.root + "/./" RNF_SESSION_TEMP_PROJECT).c_str()));
  CHECK_FALSE(rnf_paths_equal("", f.temp.c_str()));
  CHECK_FALSE(rnf_paths_equal(rn::fs::join(f.tmp, "Other.nesrec").c_str(), f.temp.c_str()));
}

TEST_CASE("record round trip; corrupt records are rejected") {
  Folder f;
  rnf_resume_record r = record("/x/y.nesrec", false);
  r.rom_path = "/r.nes";
  r.rom_sha256 = "ab";
  r.has_frame = 1;
  r.frame = 42;
  r.mode = RNF_RESUME_MODE_REPLAY;
  r.has_practice_slot = 1;
  r.practice_slot = 3;
  REQUIRE_EQ(rnf_resume_write(f.resume.c_str(), &r), RN_OK);
  rnf_resume_record back{};
  int found = 0;
  REQUIRE_EQ(rnf_resume_read(f.resume.c_str(), &back, &found), RN_OK);
  REQUIRE(found);
  CHECK(rnf_resume_same_state(&r, &back));
  CHECK_EQ(back.updated, 1800000000.0);
  CHECK_EQ(std::string(back.project_path), "/x/y.nesrec");
  CHECK_EQ(back.practice_slot, 3);
  CHECK_FALSE(back.has_at_take_end);
  rnf_resume_record later = r;
  later.updated = 1900000000.5;
  CHECK(rnf_resume_same_state(&later, &r));
  later.frame = 43;
  CHECK_FALSE(rnf_resume_same_state(&later, &r));
  rnf_resume_record_clear(&back);
  CHECK(back.project_path == nullptr);

  // The file is what Foundation's JSONEncoder wrote (sorted keys, ISO 8601, escaped slashes).
  std::string text;
  rn::fs::readText(f.resume, text);
  CHECK(text.find("\"updated\" : \"2027-01-15T08:00:00Z\"") != std::string::npos);
  CHECK(text.find("\"projectPath\" : \"\\/x\\/y.nesrec\"") != std::string::npos);
  CHECK(text.find("atTakeEnd") == std::string::npos);

  // A record written by the Swift app (fixed sample) reads back.
  writeText(f.resume, R"({
  "atTakeEnd" : true,
  "frame" : 716,
  "hasContent" : false,
  "isTemp" : true,
  "mode" : "record",
  "projectPath" : "\/p\/current.nesrec",
  "romPath" : "",
  "romSHA256" : "",
  "updated" : "2026-10-06T11:22:33+09:00",
  "version" : 1
})");
  REQUIRE_EQ(rnf_resume_read(f.resume.c_str(), &back, &found), RN_OK);
  CHECK(back.has_at_take_end && back.at_take_end && back.frame == 716 && !back.has_content);
  CHECK_EQ(back.mode, RNF_RESUME_MODE_RECORD);
  CHECK_EQ(back.updated, 1791253353.0);
  rnf_resume_record_clear(&back);

  writeText(f.resume, "{ not json");
  CHECK_EQ(rnf_resume_read(f.resume.c_str(), &back, &found), RN_ERR_CORRUPT);
  writeText(f.resume, R"({"version":1,"projectPath":"/p","isTemp":true})");  // romPath ... missing
  CHECK_EQ(rnf_resume_read(f.resume.c_str(), &back, &found), RN_ERR_CORRUPT);
  writeText(f.resume, R"({"version":2,"projectPath":"/p","isTemp":true,"romPath":"","romSHA256":"","hasContent":true,"updated":"2026-01-01T00:00:00Z"})");
  CHECK_EQ(rnf_resume_read(f.resume.c_str(), &back, &found), RN_ERR_CORRUPT);
  writeText(f.resume, R"({"version":1,"projectPath":"/p","isTemp":true,"romPath":"","romSHA256":"","hasContent":true,"updated":"2026-01-01T00:00:00.5Z"})");
  CHECK_EQ(rnf_resume_read(f.resume.c_str(), &back, &found), RN_ERR_CORRUPT);
  writeText(f.resume, R"({"version":1,"projectPath":"/p","isTemp":true,"romPath":"","romSHA256":"","hasContent":true,"updated":"2026-01-01T00:00:00Z","mode":null,"frame":null})");
  CHECK_EQ(rnf_resume_read(f.resume.c_str(), &back, &found), RN_OK);
  CHECK(found && !back.has_frame && back.mode == RNF_RESUME_MODE_NONE);
  rnf_resume_record_clear(&back);
  rnf_resume_clear(f.resume.c_str());
  CHECK_EQ(rnf_resume_read(f.resume.c_str(), &back, &found), RN_OK);
  CHECK_FALSE(found);
}

TEST_CASE("launch decision") {
  Folder f;
  rnf_resume_record out{};
  CHECK_EQ(rnf_resume_decide(f.root.c_str(), 0, nullptr, &out), RNF_RESUME_NONE);

  std::string project = rn::fs::join(f.tmp, "P.nesrec");
  rn::fs::createDirs(project);
  rnf_resume_record r = record(project.c_str(), false);
  r.has_frame = 1;
  r.frame = 10;
  r.mode = RNF_RESUME_MODE_RECORD;
  rnf_resume_write(f.resume.c_str(), &r);
  CHECK_EQ(rnf_resume_decide(f.root.c_str(), 0, nullptr, &out), RNF_RESUME_RESUME);
  CHECK(rnf_resume_same_state(&out, &r));
  CHECK_EQ(out.updated, r.updated);
  rnf_resume_record_clear(&out);
  CHECK_EQ(rnf_resume_decide(f.root.c_str(), 1, nullptr, &out), RNF_RESUME_NONE);
  rn::fs::removeAll(project);
  CHECK_EQ(rnf_resume_decide(f.root.c_str(), 0, nullptr, &out), RNF_RESUME_PROJECT_MISSING);
  CHECK(rnf_resume_same_state(&out, &r));
  rnf_resume_record_clear(&out);

  // A temp record always means this folder's temporary project.
  rn::fs::createDirs(f.temp);
  rnf_resume_record t = record("/elsewhere/current.nesrec", true);
  t.has_frame = 1;
  t.frame = 5;
  rnf_resume_write(f.resume.c_str(), &t);
  REQUIRE_EQ(rnf_resume_decide(f.root.c_str(), 0, nullptr, &out), RNF_RESUME_RESUME);
  CHECK_EQ(std::string(out.project_path), f.temp);
  CHECK_EQ(out.frame, uint64_t(5));
  rnf_resume_record_clear(&out);

  // Unreadable record but a temporary project left: still resumed (at its own cursor).
  writeText(f.resume, "garbage");
  REQUIRE_EQ(rnf_resume_decide(f.root.c_str(), 0, nullptr, &out), RNF_RESUME_RESUME);
  CHECK(out.is_temp);
  CHECK_EQ(std::string(out.project_path), f.temp);
  CHECK_FALSE(out.has_frame);
  CHECK(out.has_content);
  rnf_resume_record_clear(&out);

  // Legacy crash marker.
  rn::fs::removeAll(f.temp);
  rnf_resume_clear(f.resume.c_str());
  rn::fs::createDirs(project);
  REQUIRE_EQ(rnf_resume_decide(f.root.c_str(), 0, project.c_str(), &out), RNF_RESUME_RESUME);
  CHECK_EQ(std::string(out.project_path), project);
  CHECK_FALSE(out.has_frame);
  CHECK_FALSE(out.is_temp);
  rnf_resume_record_clear(&out);
}

TEST_CASE("lock is exclusive and released with its owner") {
  Folder f;
  rnf_session_lock* first = rnf_session_lock_acquire(f.lock.c_str());
  CHECK(first != nullptr);
  CHECK(rnf_session_lock_acquire(f.lock.c_str()) == nullptr);
  rnf_session_lock_release(first);
  rnf_session_lock* again = rnf_session_lock_acquire(f.lock.c_str());
  CHECK(again != nullptr);
  rnf_session_lock_release(again);
}

TEST_CASE("target frame clamps") {
  rnf_resume_record r = record("/p", true);
  r.has_frame = 1;
  r.frame = 500;
  uint64_t f = 0;
  REQUIRE(rnf_resume_target_frame(&r, 300, &f));
  CHECK_EQ(f, uint64_t(300));
  REQUIRE(rnf_resume_target_frame(&r, 900, &f));
  CHECK_EQ(f, uint64_t(500));
  rnf_resume_record none = record("/p", true);
  CHECK_FALSE(rnf_resume_target_frame(&none, 9, &f));
  rnf_resume_record end = record("/p", true);
  end.has_frame = 1;
  end.frame = 716;
  end.has_at_take_end = end.at_take_end = 1;
  REQUIRE(rnf_resume_target_frame(&end, 727, &f));
  CHECK_EQ(f, uint64_t(727));
  REQUIRE(rnf_resume_target_frame(&end, 600, &f));
  CHECK_EQ(f, uint64_t(600));
}

TEST_CASE("force quit resumes near the last autosave") {
  Folder f;
  std::vector<uint64_t> hashes;
  {
    rn_session* s = newSession(f.rom, f.temp);
    REQUIRE(s != nullptr);
    hashes = recordFrames(s, 300);
    CHECK(rnf_session_has_recorded_content(s));
    REQUIRE_EQ(rn_session_autosave(s), RN_OK);
    for (int i = 0; i < 60; ++i) rn_step(s, 0, 0, 0, nullptr);
    rnf_resume_record r = record(rn_session_project_dir(s), true);
    r.has_frame = 1;
    r.frame = rn_frame(s);
    r.mode = RNF_RESUME_MODE_RECORD;
    rnf_resume_write(f.resume.c_str(), &r);
    rn_session_close(s);  // without saving = killed
  }
  rnf_resume_record r{};
  REQUIRE_EQ(rnf_resume_decide(f.root.c_str(), 0, nullptr, &r), RNF_RESUME_RESUME);
  CHECK(r.is_temp);
  rn_session* s = openSession(r.project_path);
  REQUIRE(s != nullptr);
  CHECK(rn_session_recovered(s));
  CHECK_EQ(rn_take_length(s), uint64_t(300));
  REQUIRE_EQ(rnf_resume_apply(&r, s), RN_OK);
  CHECK_EQ(rn_frame(s), uint64_t(300));
  CHECK_EQ(rn_get_mode(s), RN_MODE_RECORD);
  CHECK_EQ(rn_state_hash(s), hashes[299]);
  rn_seek(s, 120);
  CHECK_EQ(rn_state_hash(s), hashes[119]);
  rn_session_close(s);
  rnf_resume_record_clear(&r);
}

TEST_CASE("clean quit resumes the exact position and mode") {
  Folder f;
  std::vector<uint64_t> hashes;
  {
    rn_session* s = newSession(f.rom, f.temp);
    hashes = recordFrames(s, 240);
    rn_seek(s, 150);
    rn_set_mode(s, RN_MODE_REPLAY);
    REQUIRE_EQ(rn_session_save(s), RN_OK);
    rnf_resume_record r = record(rn_session_project_dir(s), true);
    r.has_frame = 1;
    r.frame = 150;
    r.mode = RNF_RESUME_MODE_REPLAY;
    r.has_practice_slot = 1;
    r.practice_slot = -1;
    rnf_resume_write(f.resume.c_str(), &r);
    rn_session_close(s);
  }
  rnf_resume_record r{};
  REQUIRE_EQ(rnf_resume_decide(f.root.c_str(), 0, nullptr, &r), RNF_RESUME_RESUME);
  CHECK(r.has_practice_slot && r.practice_slot == -1);
  rn_session* s = openSession(f.temp);
  REQUIRE(s != nullptr);
  CHECK_FALSE(rn_session_recovered(s));
  CHECK_EQ(rn_frame(s), uint64_t(150));
  REQUIRE_EQ(rnf_resume_apply(&r, s), RN_OK);
  CHECK_FALSE(rn_session_has_unsaved_changes(s));
  CHECK_EQ(rn_state_hash(s), hashes[149]);
  rnf_resume_record newer = record(r.project_path, true);
  newer.has_frame = 1;
  newer.frame = 60;
  newer.mode = RNF_RESUME_MODE_RECORD;
  REQUIRE_EQ(rnf_resume_apply(&newer, s), RN_OK);
  CHECK_EQ(rn_frame(s), uint64_t(60));
  CHECK_EQ(rn_get_mode(s), RN_MODE_RECORD);
  CHECK_EQ(rn_state_hash(s), hashes[59]);
  rn_session_close(s);
  rnf_resume_record_clear(&r);
}

TEST_CASE("an empty session has no content") {
  Folder f;
  rn_session* s = newSession(f.rom, f.temp);
  REQUIRE(s != nullptr);
  CHECK_FALSE(rnf_session_has_recorded_content(s));
  rn_bookmark_add(s, "b", nullptr);
  CHECK(rnf_session_has_recorded_content(s));
  rn_session_close(s);
}
