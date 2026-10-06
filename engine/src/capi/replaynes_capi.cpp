// C API implementation: thin, exception-safe wrapper over the C++ engine.
#include "replaynes/replaynes.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <string>

#include "core/ICore.h"
#include "core/NestopiaCore.h"
#include "input/InputPipeline.h"
#include "persist/ProjectStore.h"
#include "render/OfflineRenderer.h"
#include "session/Session.h"
#include "testrom/TestRom.h"
#include "util/Fs.h"
#include "util/Hash.h"
#include "video/FlashFilter.h"

#ifndef RN_ENGINE_VERSION
#define RN_ENGINE_VERSION "0.0.0"
#endif

static_assert(int(RN_ERR_CORE_MISMATCH) == int(rn::Err::CoreMismatch), "status codes must match");
static_assert(int(RN_ERR_END_OF_TAKE) == int(rn::Err::EndOfTake), "status codes must match");
static_assert(int(RN_ERR_INTERNAL) == int(rn::Err::Internal), "status codes must match");
static_assert(RN_FPS_NUM == rn::kFpsNum && RN_FPS_DEN == rn::kFpsDen, "rate constants must match");

struct rn_session {
  std::unique_ptr<rn::Session> s;
  std::string projectDir;
};
struct rn_input {
  rn::InputPipeline p;
};
struct rn_renderer {
  std::unique_ptr<rn::OfflineRenderer> r;
};
struct rn_flash_filter {
  rn::FlashFilter f;
};

namespace {
thread_local std::string tlError;

rn_status fail(const rn::Status& st) {
  tlError = std::string(rn::errName(st.code)) + ": " + st.message;
  return rn_status(int(st.code));
}
rn_status ret(const rn::Status& st) {
  if (st.ok()) return RN_OK;
  return fail(st);
}
rn_status invalid(const char* m) { return fail(rn::Error(rn::Err::InvalidArg, m)); }

template <typename F>
rn_status guard(F&& f) {
  try {
    return f();
  } catch (const std::bad_alloc&) {
    return fail(rn::Error(rn::Err::Internal, "out of memory"));
  } catch (const std::exception& e) {
    return fail(rn::Error(rn::Err::Internal, e.what()));
  } catch (...) {
    return fail(rn::Error(rn::Err::Internal, "unknown exception"));
  }
}

rn::SessionOptions toOptions(const rn_session_options* o) {
  rn::SessionOptions so;
  if (!o) return so;
  so.core = o->core == RN_CORE_MOCK ? rn::CoreKind::Mock : rn::CoreKind::Nestopia;
  if (o->dense_interval) so.checkpoints.denseInterval = o->dense_interval;
  if (o->dense_capacity) so.checkpoints.denseCapacity = o->dense_capacity;
  if (o->sparse_interval) so.checkpoints.sparseInterval = o->sparse_interval;
  so.dropCorruptStates = (o->open_flags & RN_OPEN_DROP_CORRUPT_STATES) != 0;
  return so;
}

char* dupString(const std::string& s) {
  char* p = static_cast<char*>(std::malloc(s.size() + 1));
  if (!p) throw std::bad_alloc();
  std::memcpy(p, s.c_str(), s.size() + 1);
  return p;
}
}  // namespace

extern "C" {

const char* rn_version(void) { return RN_ENGINE_VERSION; }
const char* rn_core_compat_id(void) {
  static const std::string id = rn::NestopiaCore::staticCompatId();
  return id.c_str();
}
const char* rn_core_build_id(void) {
  static const std::string id = rn::NestopiaCore().buildId();
  return id.c_str();
}
const char* rn_last_error(void) { return tlError.c_str(); }
const char* rn_status_name(rn_status s) { return rn::errName(rn::Err(int(s))); }
void rn_string_free(char* s) { std::free(s); }
uint64_t rn_audio_samples_before(uint64_t frame) { return rn::audioSamplesBefore(frame); }
uint32_t rn_audio_samples_for_frame(uint64_t frame) { return rn::audioSamplesForFrame(frame); }

rn_status rn_sha256_file(const char* path, char out_hex[65]) {
  return guard([&] {
    if (!path || !out_hex) return invalid("null argument");
    std::vector<uint8_t> b;
    rn::Status st = rn::fs::readFile(path, b);
    if (!st.ok()) return fail(st);
    std::string h = rn::Sha256::hex(b.data(), b.size());
    std::memcpy(out_hex, h.c_str(), 65);
    return RN_OK;
  });
}

rn_status rn_write_test_rom(const char* path) {
  return guard([&] {
    if (!path) return invalid("null path");
    std::vector<uint8_t> rom = rn::buildTestRom();
    return ret(rn::fs::writeFileAtomic(path, rom.data(), rom.size()));
  });
}

// ------------------------------------------------------------------ session
void rn_session_options_init(rn_session_options* o) {
  if (!o) return;
  std::memset(o, 0, sizeof *o);
  o->struct_size = sizeof *o;
  o->core = RN_CORE_NESTOPIA;
  o->dense_interval = 30;
  o->dense_capacity = 1200;
  o->sparse_interval = 600;
}

rn_status rn_session_new(const char* rom_path, const char* project_dir, const rn_session_options* opt,
                         rn_session** out) {
  return guard([&] {
    if (!rom_path || !out) return invalid("null argument");
    *out = nullptr;
    std::vector<uint8_t> rom;
    rn::Status st = rn::fs::readFile(rom_path, rom);
    if (!st.ok()) return fail(st.code == rn::Err::NotFound ? rn::Error(rn::Err::RomNotFound, st.message) : st);
    std::unique_ptr<rn::Session> s;
    st = rn::Session::create(std::move(rom), rom_path, toOptions(opt), s);
    if (!st.ok()) return fail(st);
    if (project_dir && *project_dir) {
      st = s->saveAs(project_dir);
      if (!st.ok()) return fail(st);
    }
    *out = new rn_session{std::move(s), {}};
    (*out)->projectDir = (*out)->s->projectDir();
    return RN_OK;
  });
}

rn_status rn_session_open(const char* project_dir, const char* rom_override, const rn_session_options* opt,
                          rn_session** out) {
  return guard([&] {
    if (!project_dir || !out) return invalid("null argument");
    *out = nullptr;
    std::unique_ptr<rn::Session> s;
    rn::Status st = rn::ProjectStore::open(project_dir, rom_override ? rom_override : "", toOptions(opt), s);
    if (!st.ok()) return fail(st);
    *out = new rn_session{std::move(s), {}};
    (*out)->projectDir = (*out)->s->projectDir();
    return RN_OK;
  });
}

void rn_session_close(rn_session* s) { delete s; }

rn_status rn_session_save(rn_session* s) {
  return guard([&] { return s ? ret(s->s->save()) : invalid("null session"); });
}
rn_status rn_session_autosave(rn_session* s) {
  return guard([&] { return s ? ret(s->s->autosave()) : invalid("null session"); });
}
rn_status rn_session_save_as(rn_session* s, const char* dir) {
  return guard([&] {
    if (!s || !dir) return invalid("null argument");
    rn_status r = ret(s->s->saveAs(dir));
    if (r == RN_OK) s->projectDir = s->s->projectDir();
    return r;
  });
}
int rn_session_recovered(const rn_session* s) { return s && s->s->recovered() ? 1 : 0; }
int rn_session_has_unsaved_changes(const rn_session* s) { return s && s->s->unsavedChanges() ? 1 : 0; }
const char* rn_session_rom_sha256(const rn_session* s) { return s ? s->s->romSha256().c_str() : ""; }
const char* rn_session_rom_path(const rn_session* s) { return s ? s->s->romPath().c_str() : ""; }
const char* rn_session_project_dir(const rn_session* s) { return s ? s->projectDir.c_str() : ""; }

rn_status rn_project_manifest_json(const char* project_dir, char** out_json) {
  return guard([&] {
    if (!project_dir || !out_json) return invalid("null argument");
    std::string j;
    rn::Status st = rn::ProjectStore::readManifest(project_dir, j);
    if (!st.ok()) return fail(st);
    *out_json = dupString(j);
    return RN_OK;
  });
}

// ------------------------------------------------------------------ play
rn_status rn_set_mode(rn_session* s, rn_mode mode) {
  if (!s) return invalid("null session");
  if (mode != RN_MODE_RECORD && mode != RN_MODE_REPLAY) return invalid("bad mode");
  s->s->setMode(mode == RN_MODE_REPLAY ? rn::Mode::Replay : rn::Mode::Record);
  return RN_OK;
}
rn_mode rn_get_mode(const rn_session* s) {
  return s && s->s->mode() == rn::Mode::Replay ? RN_MODE_REPLAY : RN_MODE_RECORD;
}

rn_status rn_step(rn_session* s, uint8_t p1, uint8_t p2, uint8_t events, rn_step_info* info) {
  return guard([&] {
    if (!s) return invalid("null session");
    rn::StepInfo si;
    rn::Status st = s->s->step(p1, p2, events, &si);
    if (!st.ok()) return fail(st);
    if (info) {
      info->frame = si.frame;
      info->mode = si.mode == rn::Mode::Replay ? RN_MODE_REPLAY : RN_MODE_RECORD;
      info->branched = si.branched;
      info->end_of_take = si.endOfTake;
      info->p1 = si.applied.p1;
      info->p2 = si.applied.p2;
      info->events = si.applied.events;
      info->take_id = si.take;
    }
    return RN_OK;
  });
}

const uint32_t* rn_video(const rn_session* s) { return s ? s->s->video() : nullptr; }
const int16_t* rn_audio(const rn_session* s, size_t* count) {
  size_t n = 0;
  const int16_t* a = s ? s->s->audio(&n) : nullptr;
  if (count) *count = n;
  return a;
}
uint64_t rn_frame(const rn_session* s) { return s ? s->s->frame() : 0; }
uint64_t rn_take_length(const rn_session* s) { return s ? s->s->takeLength() : 0; }
rn_status rn_seek(rn_session* s, uint64_t frame) {
  return guard([&] { return s ? ret(s->s->seek(frame)) : invalid("null session"); });
}
rn_status rn_rewind(rn_session* s, uint64_t n) {
  return guard([&] { return s ? ret(s->s->rewind(n)) : invalid("null session"); });
}
uint64_t rn_state_hash(rn_session* s) { return s ? s->s->stateHash() : 0; }
uint64_t rn_video_hash(const rn_session* s) { return s ? s->s->videoHash() : 0; }
uint64_t rn_audio_hash(const rn_session* s) { return s ? s->s->audioHash() : 0; }

// ------------------------------------------------------------------ bookmarks
rn_status rn_bookmark_add(rn_session* s, const char* name, uint64_t* out_id) {
  return guard([&] { return s ? ret(s->s->bookmarkAdd(name ? name : "", out_id)) : invalid("null session"); });
}
rn_status rn_bookmark_remove(rn_session* s, uint64_t id) {
  return guard([&] { return s ? ret(s->s->bookmarkRemove(id)) : invalid("null session"); });
}
rn_status rn_bookmark_rename(rn_session* s, uint64_t id, const char* name) {
  return guard([&] { return s ? ret(s->s->bookmarkRename(id, name ? name : "")) : invalid("null session"); });
}
size_t rn_bookmark_count(const rn_session* s) { return s ? s->s->bookmarks().size() : 0; }
rn_status rn_bookmark_get(const rn_session* s, size_t index, rn_bookmark_info* out) {
  if (!s || !out) return invalid("null argument");
  const auto& v = s->s->bookmarks();
  if (index >= v.size()) return fail(rn::Error(rn::Err::OutOfRange, "bookmark index out of range"));
  const rn::Bookmark& b = v[index];
  out->id = b.id;
  out->frame = b.frame;
  out->take_id = b.owner;
  out->name = b.name.c_str();
  out->on_active_take = s->s->bookmarkOnActiveTake(b) ? 1 : 0;
  return RN_OK;
}
rn_status rn_bookmark_goto(rn_session* s, uint64_t id) {
  return guard([&] { return s ? ret(s->s->bookmarkGoto(id)) : invalid("null session"); });
}

// ------------------------------------------------------------------ takes
size_t rn_take_count(const rn_session* s) { return s ? s->s->timeline().segments().size() : 0; }
rn_status rn_take_get(const rn_session* s, size_t index, rn_take_info* out) {
  return guard([&] {
    if (!s || !out) return invalid("null argument");
    const auto& segs = s->s->timeline().segments();
    if (index >= segs.size()) return fail(rn::Error(rn::Err::OutOfRange, "take index out of range"));
    auto it = segs.begin();
    std::advance(it, ptrdiff_t(index));
    const rn::Segment& g = it->second;
    out->id = g.id;
    out->parent_id = g.parent;
    out->branch_frame = g.start;
    out->length = g.end();
    out->created_seq = g.createdSeq;
    out->is_active = g.id == s->s->activeTake();
    out->child_count = uint32_t(s->s->timeline().childCount(g.id));
    return RN_OK;
  });
}
uint64_t rn_active_take(const rn_session* s) { return s ? s->s->activeTake() : 0; }
rn_status rn_take_activate(rn_session* s, uint64_t id) {
  return guard([&] { return s ? ret(s->s->activateTake(id)) : invalid("null session"); });
}
rn_status rn_undo_take_switch(rn_session* s) {
  return guard([&] { return s ? ret(s->s->undoTakeSwitch()) : invalid("null session"); });
}
size_t rn_undo_depth(const rn_session* s) { return s ? s->s->undoDepth() : 0; }

// ------------------------------------------------------------------ input
rn_input* rn_input_new(void) {
  try {
    return new rn_input();
  } catch (...) {
    return nullptr;
  }
}
void rn_input_free(rn_input* in) { delete in; }
rn_status rn_input_bind(rn_input* in, const char* id, const char* action) {
  return guard([&] { return in && id && action ? ret(in->p.bind(id, action)) : invalid("null argument"); });
}
rn_status rn_input_unbind(rn_input* in, const char* id, const char* action) {
  return guard([&] { return in && id ? ret(in->p.unbind(id, action ? action : "")) : invalid("null argument"); });
}
void rn_input_clear_bindings(rn_input* in) { if (in) in->p.clearBindings(); }
rn_status rn_input_load_json(rn_input* in, const char* json) {
  return guard([&] { return in && json ? ret(in->p.fromJson(json)) : invalid("null argument"); });
}
char* rn_input_save_json(const rn_input* in) {
  if (!in) return nullptr;
  try {
    return dupString(in->p.toJson());
  } catch (...) {
    return nullptr;
  }
}
rn_status rn_input_set_turbo(rn_input* in, uint32_t period, uint32_t duty) {
  return guard([&] { return in ? ret(in->p.setTurbo(period, duty)) : invalid("null input"); });
}
rn_status rn_input_set_socd(rn_input* in, rn_socd policy) {
  if (!in) return invalid("null input");
  if (policy != RN_SOCD_NEUTRAL && policy != RN_SOCD_LAST_WINS && policy != RN_SOCD_ALLOW) return invalid("bad policy");
  in->p.setSocd(rn::Socd(int(policy)));
  return RN_OK;
}
rn_status rn_input_set_analog_threshold(rn_input* in, float t) {
  return guard([&] { return in ? ret(in->p.setAnalogThreshold(t)) : invalid("null input"); });
}
void rn_input_set_pressed(rn_input* in, const char* id, int pressed) {
  if (!in || !id) return;
  try { in->p.setPressed(id, pressed != 0); } catch (...) {}
}
void rn_input_set_axis(rn_input* in, const char* id, float x, float y) {
  if (!in || !id) return;
  try { in->p.setAxis(id, x, y); } catch (...) {}
}
void rn_input_release_prefix(rn_input* in, const char* prefix) {
  if (!in || !prefix) return;
  try { in->p.releasePrefix(prefix); } catch (...) {}
}
void rn_input_release_all(rn_input* in) { if (in) in->p.releaseAll(); }
void rn_input_sample_game(rn_input* in, uint64_t frame, uint8_t* p1, uint8_t* p2) {
  uint8_t a = 0, b = 0;
  if (in) {
    try { in->p.sampleGame(frame, a, b); } catch (...) {}
  }
  if (p1) *p1 = a;
  if (p2) *p2 = b;
}
void rn_input_poll_hotkeys(rn_input* in, uint32_t* edges, uint32_t* held) {
  uint32_t e = 0, h = 0;
  if (in) {
    try { in->p.pollHotkeys(e, h); } catch (...) {}
  }
  if (edges) *edges = e;
  if (held) *held = h;
}

// ------------------------------------------------------------------ renderer
rn_status rn_renderer_new(rn_session* s, uint64_t start, uint64_t end, rn_renderer** out) {
  return guard([&] {
    if (!s || !out) return invalid("null argument");
    *out = nullptr;
    std::unique_ptr<rn::OfflineRenderer> r;
    rn::Status st = rn::OfflineRenderer::create(*s->s, start, end, r);
    if (!st.ok()) return fail(st);
    *out = new rn_renderer{std::move(r)};
    return RN_OK;
  });
}
uint64_t rn_renderer_total_frames(const rn_renderer* r) { return r ? r->r->totalFrames() : 0; }
uint64_t rn_renderer_frames_done(const rn_renderer* r) { return r ? r->r->framesDone() : 0; }
rn_status rn_renderer_next(rn_renderer* r, const uint32_t** video, const int16_t** audio, size_t* n,
                           uint64_t* frame) {
  return guard([&] {
    if (!r) return invalid("null renderer");
    rn::Status st = r->r->next(video, audio, n, frame);
    if (st.code == rn::Err::EndOfTake) return RN_ERR_END_OF_TAKE;  // normal completion, not an error message
    return ret(st);
  });
}
uint64_t rn_renderer_hash(const rn_renderer* r) { return r ? r->r->hash() : 0; }
void rn_renderer_free(rn_renderer* r) { delete r; }

// ------------------------------------------------------------------ flash reduction
static bool validFlashLevel(rn_flash_level l) { return int(l) >= RN_FLASH_OFF && int(l) <= RN_FLASH_HIGH; }

rn_flash_filter* rn_flash_filter_new(rn_flash_level level) {
  if (!validFlashLevel(level)) {
    invalid("invalid flash reduction level");
    return nullptr;
  }
  try {
    return new rn_flash_filter{rn::FlashFilter(rn::FlashLevel(int(level)))};
  } catch (...) {
    fail(rn::Error(rn::Err::Internal, "out of memory"));
    return nullptr;
  }
}
void rn_flash_filter_free(rn_flash_filter* f) { delete f; }
void rn_flash_filter_reset(rn_flash_filter* f) {
  if (f) f->f.reset();
}
rn_status rn_flash_filter_set_level(rn_flash_filter* f, rn_flash_level level) {
  if (!f) return invalid("null filter");
  if (!validFlashLevel(level)) return invalid("invalid flash reduction level");
  f->f.setLevel(rn::FlashLevel(int(level)));
  return RN_OK;
}
rn_flash_level rn_flash_filter_get_level(const rn_flash_filter* f) {
  return f ? rn_flash_level(int(f->f.level())) : RN_FLASH_OFF;
}
rn_status rn_flash_filter_process(rn_flash_filter* f, const uint32_t* in, uint32_t* out, rn_flash_info* info) {
  return guard([&] {
    if (!f || !in || !out) return invalid("null argument");
    rn::FlashFrameInfo r = f->f.process(in, out);
    if (info) {
      info->altered = r.altered ? 1 : 0;
      info->altered_blocks = uint32_t(r.alteredBlocks);
      info->event_area_permille = uint32_t(r.eventAreaPermille);
      info->large_area = r.largeArea ? 1 : 0;
    }
    return RN_OK;
  });
}

}  // extern "C"
