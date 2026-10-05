#include "persist/ProjectStore.h"

#include <algorithm>
#include <set>

#include "util/Bytes.h"
#include "util/Fs.h"
#include "util/Hash.h"
#include "util/Json.h"

#ifndef RN_ENGINE_VERSION
#define RN_ENGINE_VERSION "0.0.0"
#endif

namespace rn {

namespace {
const uint32_t kSegMagic = 0x47534E52;    // "RNSG"
const uint32_t kStateMagic = 0x54534E52;  // "RNST"
const uint32_t kJrnMagic = 0x4C4A4E52;    // "RNJL"
const uint32_t kRecMagic = 0x524A4E52;    // "RNJR" (per journal record, for resync/corruption detection)
const uint32_t kBinVersion = 1;
enum : uint8_t { J_SEG_NEW = 1, J_SEG_APPEND = 2, J_META = 3 };

const char* kindName(CpKind k) {
  switch (k) {
    case CpKind::Dense: return "dense";
    case CpKind::Sparse: return "sparse";
    case CpKind::Bookmark: return "bookmark";
    case CpKind::Head: return "head";
  }
  return "dense";
}
bool kindFromName(const std::string& s, CpKind& k) {
  if (s == "sparse") k = CpKind::Sparse;
  else if (s == "bookmark") k = CpKind::Bookmark;
  else if (s == "head") k = CpKind::Head;
  else return false;
  return true;
}
bool persistable(CpKind k) { return k != CpKind::Dense; }

std::string segRel(uint64_t id) { return "timeline/segments/" + std::to_string(id) + ".seg"; }
std::string stateRel(uint64_t id) { return "states/" + std::to_string(id) + ".state"; }
const char* kJournalRel = "journal/journal.bin";

std::vector<uint8_t> encodeSegment(const Segment& s) {
  ByteWriter w;
  w.u32(kSegMagic);
  w.u32(kBinVersion);
  w.u64(s.id);
  w.u64(s.parent);
  w.u64(s.start);
  w.u64(s.createdSeq);
  w.u64(s.records.size());
  encodeRecords(s.records.data(), s.records.size(), w.buf);
  w.u32(crc32(w.buf.data(), w.buf.size()));
  return w.buf;
}

Status decodeSegment(const std::string& name, const std::vector<uint8_t>& b, Segment& s) {
  if (b.size() < 4 + 4 + 8 * 5 + 4) return Error(Err::Corrupt, "segment file truncated: " + name);
  uint32_t stored = uint32_t(b[b.size() - 4]) | uint32_t(b[b.size() - 3]) << 8 | uint32_t(b[b.size() - 2]) << 16 |
                    uint32_t(b[b.size() - 1]) << 24;
  if (crc32(b.data(), b.size() - 4) != stored) return Error(Err::Corrupt, "segment checksum mismatch: " + name);
  ByteReader r(b.data(), b.size() - 4);
  if (r.u32() != kSegMagic) return Error(Err::Corrupt, "not a segment file: " + name);
  if (r.u32() != kBinVersion) return Error(Err::UnsupportedFormat, "unsupported segment version: " + name);
  s.id = r.u64();
  s.parent = r.u64();
  s.start = r.u64();
  s.createdSeq = r.u64();
  uint64_t count = r.u64();
  if (!r.ok() || count > (uint64_t(1) << 32)) return Error(Err::Corrupt, "bad segment header: " + name);
  size_t pos = r.pos();
  if (!decodeRecords(b.data() + pos, b.size() - 4 - pos, count, s.records))
    return Error(Err::Corrupt, "bad segment records: " + name);
  return Status::Ok();
}

std::vector<uint8_t> encodeState(const Checkpoint& c) {
  ByteWriter w;
  w.u32(kStateMagic);
  w.u32(kBinVersion);
  w.u64(c.id);
  w.u64(c.frame);
  w.u64(c.owner);
  w.u8(uint8_t(c.kind));
  w.u32(c.stateFormatVersion);
  w.str(c.compat);
  w.u64(c.data.size());
  w.bytes(c.data.data(), c.data.size());
  w.u32(crc32(w.buf.data(), w.buf.size()));
  return w.buf;
}

Status decodeState(const std::string& name, const std::vector<uint8_t>& b, Checkpoint& c) {
  if (b.size() < 16) return Error(Err::Corrupt, "state file truncated: " + name);
  uint32_t stored = uint32_t(b[b.size() - 4]) | uint32_t(b[b.size() - 3]) << 8 | uint32_t(b[b.size() - 2]) << 16 |
                    uint32_t(b[b.size() - 1]) << 24;
  if (crc32(b.data(), b.size() - 4) != stored) return Error(Err::Corrupt, "state checksum mismatch: " + name);
  ByteReader r(b.data(), b.size() - 4);
  if (r.u32() != kStateMagic) return Error(Err::Corrupt, "not a state file: " + name);
  if (r.u32() != kBinVersion) return Error(Err::UnsupportedFormat, "unsupported state file version: " + name);
  c.id = r.u64();
  c.frame = r.u64();
  c.owner = r.u64();
  c.kind = CpKind(r.u8());
  c.stateFormatVersion = r.u32();
  c.compat = r.str(256);
  uint64_t n = r.u64();
  const uint8_t* p = r.ptr(size_t(n));
  if (!r.ok() || !p || r.remaining()) return Error(Err::Corrupt, "bad state file: " + name);
  c.data.assign(p, p + n);
  c.crc = crc32(c.data.data(), c.data.size());
  return Status::Ok();
}

void putRecord(ByteWriter& out, uint8_t type, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> body;
  body.push_back(type);
  body.insert(body.end(), payload.begin(), payload.end());
  out.u32(kRecMagic);
  out.u32(uint32_t(body.size()));
  out.bytes(body.data(), body.size());
  out.u32(crc32(body.data(), body.size()));
}

// True if a complete, checksum-valid journal record starts anywhere after `from`. Used to tell a
// torn trailing write (expected after a crash) from damage in the middle of the journal.
bool validRecordAfter(const std::vector<uint8_t>& b, size_t from) {
  auto rd = [&](size_t o) {
    return uint32_t(b[o]) | uint32_t(b[o + 1]) << 8 | uint32_t(b[o + 2]) << 16 | uint32_t(b[o + 3]) << 24;
  };
  for (size_t p = from + 1; p + 13 <= b.size(); ++p) {
    if (rd(p) != kRecMagic) continue;
    uint32_t len = rd(p + 4);
    if (len == 0 || uint64_t(p) + 8 + len + 4 > b.size()) continue;
    if (crc32(b.data() + p + 8, len) == rd(p + 8 + len)) return true;
  }
  return false;
}

std::string baseName(const std::string& p) {
  size_t i = p.find_last_of("/\\");
  return i == std::string::npos ? p : p.substr(i + 1);
}

Status parseJsonFile(const std::string& path, Json& out) {
  std::string text;
  Status st = fs::readText(path, text);
  if (!st.ok()) return st.code == Err::NotFound ? Error(Err::Corrupt, "missing " + path) : st;
  std::string err;
  if (!Json::parse(text, out, &err) || !out.isObject()) return Error(Err::Corrupt, "invalid JSON in " + path + ": " + err);
  return Status::Ok();
}
}  // namespace

std::string ProjectStore::path(const std::string& rel) const { return fs::join(dir_, rel); }

Status ProjectStore::readManifest(const std::string& dir, std::string& json) {
  return fs::readText(fs::join(dir, "manifest.json"), json);
}

// ------------------------------------------------------------------ meta (head, cursor, bookmarks, undo)
std::string ProjectStore::metaJson(const Session& s) const {
  Json m = Json::object();
  m.set("activeHead", s.tl_.head());
  m.set("cursorFrame", s.frame_);
  m.set("mode", s.mode_ == Mode::Replay ? "replay" : "record");
  m.set("romPath", s.romPath_);
  m.set("nextBookmarkId", s.nextBookmarkId_);
  Json undo = Json::array();
  for (auto& u : s.tl_.undo()) {
    Json e = Json::object();
    e.set("head", u.head);
    e.set("frame", u.frame);
    undo.push(e);
  }
  m.set("undo", undo);
  Json bms = Json::array();
  for (auto& b : s.bookmarks_) {
    Json e = Json::object();
    e.set("id", b.id);
    e.set("name", b.name);
    e.set("frame", b.frame);
    e.set("owner", b.owner);
    e.set("stateId", b.stateId);
    bms.push(e);
  }
  m.set("bookmarks", bms);
  return m.dump(0);
}

Status ProjectStore::applyMeta(Session& s, const std::string& text) {
  Json m;
  std::string err;
  if (!Json::parse(text, m, &err) || !m.isObject()) return Error(Err::Corrupt, "bad meta record: " + err);
  s.tl_.undo().clear();
  for (auto& e : m["undo"].items()) {
    uint64_t h = uint64_t(e["head"].asInt());
    if (!s.tl_.segment(h)) return Error(Err::Corrupt, "undo entry refers to missing take");
    s.tl_.undo().push_back({h, uint64_t(e["frame"].asInt())});
  }
  s.bookmarks_.clear();
  for (auto& e : m["bookmarks"].items()) {
    Bookmark b;
    b.id = uint64_t(e["id"].asInt());
    b.name = e["name"].asString();
    b.frame = uint64_t(e["frame"].asInt());
    b.owner = uint64_t(e["owner"].asInt());
    b.stateId = uint64_t(e["stateId"].asInt());
    if (b.owner != 0 && !s.tl_.segment(b.owner)) return Error(Err::Corrupt, "bookmark refers to missing take");
    if (b.stateId && !s.cps_.get(b.stateId)) b.stateId = 0;  // state was never persisted; seek replays instead
    s.bookmarks_.push_back(b);
  }
  s.nextBookmarkId_ = uint64_t(m["nextBookmarkId"].asInt(1));
  for (auto& b : s.bookmarks_) if (b.id >= s.nextBookmarkId_) s.nextBookmarkId_ = b.id + 1;
  uint64_t head = uint64_t(m["activeHead"].asInt());
  RN_TRY(s.tl_.setHead(head));
  s.frame_ = uint64_t(m["cursorFrame"].asInt());
  s.mode_ = m["mode"].asString() == "replay" ? Mode::Replay : Mode::Record;
  // "romPath" is informational here: the ROM actually opened (manifest path or override) wins.
  return Status::Ok();
}

// ------------------------------------------------------------------ create / save
Status ProjectStore::create(const std::string& dir, Session& s, std::unique_ptr<ProjectStore>& out) {
  if (dir.empty()) return Error(Err::InvalidArg, "empty project path");
  if (fs::exists(dir)) {
    std::vector<std::string> names;
    if (!fs::isDir(dir)) return Error(Err::AlreadyExists, "path exists and is not a directory: " + dir);
    RN_TRY(fs::listDir(dir, names));
    if (!names.empty() || fs::exists(fs::join(dir, "timeline")))
      return Error(Err::AlreadyExists, "project directory is not empty: " + dir);
  }
  std::unique_ptr<ProjectStore> ps(new ProjectStore(dir));
  for (const char* sub : {"timeline/segments", "states", "metadata", "journal"}) RN_TRY(fs::createDirs(ps->path(sub)));
  RN_TRY(ps->fullSave(s));
  out = std::move(ps);
  return Status::Ok();
}

Status ProjectStore::resetJournal() {
  ByteWriter w;
  w.u32(kJrnMagic);
  w.u32(kBinVersion);
  w.u64(gen_);
  w.u32(crc32(w.buf.data(), w.buf.size()));
  RN_TRY(fs::writeFileAtomic(path(kJournalRel), w.buf.data(), w.buf.size()));
  journalBase_ = gen_;
  return Status::Ok();
}

Status ProjectStore::fullSave(Session& s) {
  // Exact-resume state at the cursor.
  std::vector<uint64_t> oldHeads;
  for (auto& kv : s.cps_.all()) if (kv.second.kind == CpKind::Head) oldHeads.push_back(kv.first);
  for (uint64_t id : oldHeads) s.cps_.remove(id);
  if (s.frame_ > 0) {
    uint64_t owner = s.tl_.stateOwner(s.frame_);
    bool have = false;
    for (auto& kv : s.cps_.all())
      if (kv.second.frame == s.frame_ && kv.second.owner == owner && persistable(kv.second.kind)) have = true;
    if (!have) {
      std::vector<uint8_t> st;
      RN_TRY(s.captureState(st));
      s.cps_.add(s.frame_, owner, CpKind::Head, s.core_->compatId(), s.core_->stateFormatVersion(), std::move(st));
    }
  }

  const uint64_t newGen = gen_ + 1;
  // 1. data files (new content only)
  for (auto& kv : s.tl_.mutableSegments()) {
    Segment& seg = kv.second;
    if (seg.fileCount == seg.records.size() && seg.journalCreated && fs::exists(path(segRel(seg.id)))) continue;
    std::vector<uint8_t> b = encodeSegment(seg);
    RN_TRY(fs::writeFileAtomic(path(segRel(seg.id)), b.data(), b.size()));
  }
  for (auto& kv : s.cps_.all()) {
    const Checkpoint& c = kv.second;
    if (!persistable(c.kind) || c.persisted) continue;
    std::vector<uint8_t> b = encodeState(c);
    RN_TRY(fs::writeFileAtomic(path(stateRel(c.id)), b.data(), b.size()));
  }

  // 2. index (commit point for timeline content)
  Json idx = Json::object();
  idx.set("generation", newGen);
  idx.set("nextSegmentId", s.tl_.nextId());
  idx.set("seq", s.tl_.seq());
  idx.set("nextCheckpointId", s.cps_.nextId());
  Json segs = Json::array();
  for (auto& kv : s.tl_.segments()) {
    const Segment& seg = kv.second;
    Json e = Json::object();
    e.set("id", seg.id);
    e.set("parent", seg.parent);
    e.set("start", seg.start);
    e.set("count", uint64_t(seg.records.size()));
    e.set("createdSeq", seg.createdSeq);
    e.set("file", segRel(seg.id));
    segs.push(e);
  }
  idx.set("segments", segs);
  Json cps = Json::array();
  std::set<std::string> liveStates;
  for (auto& kv : s.cps_.all()) {
    const Checkpoint& c = kv.second;
    if (!persistable(c.kind)) continue;
    Json e = Json::object();
    e.set("id", c.id);
    e.set("frame", c.frame);
    e.set("owner", c.owner);
    e.set("kind", kindName(c.kind));
    e.set("file", stateRel(c.id));
    e.set("crc32", c.crc);
    cps.push(e);
    liveStates.insert(std::to_string(c.id) + ".state");
  }
  idx.set("checkpoints", cps);
  std::string meta = metaJson(s);
  Json metaObj;
  Json::parse(meta, metaObj);
  idx.set("meta", metaObj);
  RN_TRY(fs::writeFileAtomic(path("timeline/index.json"), idx.dump()));

  // Committed: update bookkeeping before the remaining (recoverable) steps.
  gen_ = newGen;
  for (auto& kv : s.tl_.mutableSegments()) {
    kv.second.fileCount = kv.second.records.size();
    kv.second.journalCount = kv.second.records.size();
    kv.second.journalCreated = true;
  }
  for (auto& kv : s.cps_.all())
    if (persistable(kv.second.kind)) s.cps_.getMutable(kv.first)->persisted = true;
  lastMeta_ = meta;

  // 3. bookmark mirror, manifest (last), journal reset, GC
  Json bm = Json::object();
  bm.set("generation", newGen);
  bm.set("note", "mirror of timeline/index.json meta.bookmarks; index.json is authoritative");
  bm.set("bookmarks", metaObj["bookmarks"]);
  RN_TRY(fs::writeFileAtomic(path("metadata/bookmarks.json"), bm.dump()));

  Json man = Json::object();
  man.set("format", "replaynes-project");
  man.set("formatVersion", kProjectFormatVersion);
  man.set("appVersion", RN_ENGINE_VERSION);
  man.set("generation", newGen);
  man.set("coreCompatId", s.core_->compatId());
  man.set("coreBuild", s.core_->buildId());
  man.set("stateFormatVersion", s.core_->stateFormatVersion());
  man.set("inputFormatVersion", kInputFormatVersion);
  man.set("region", "NTSC");
  Json rom = Json::object();
  rom.set("sha256", s.romSha_);
  rom.set("size", uint64_t(s.rom_.size()));
  rom.set("lastPath", s.romPath_);
  rom.set("name", baseName(s.romPath_));
  man.set("rom", rom);
  RN_TRY(fs::writeFileAtomic(path("manifest.json"), man.dump()));

  RN_TRY(resetJournal());

  std::vector<std::string> names;
  if (fs::listDir(path("states"), names).ok())
    for (auto& n : names)
      if (!liveStates.count(n)) fs::removeFile(path("states/" + n));
  return Status::Ok();
}

Status ProjectStore::autosave(Session& s) {
  if (journalBase_ != gen_) RN_TRY(resetJournal());
  ByteWriter w;
  struct Pending { Segment* seg; uint64_t newCount; };
  std::vector<Pending> pend;
  for (auto& kv : s.tl_.mutableSegments()) {
    Segment& seg = kv.second;
    if (!seg.journalCreated) {
      ByteWriter p;
      p.u64(seg.id);
      p.u64(seg.parent);
      p.u64(seg.start);
      p.u64(seg.createdSeq);
      putRecord(w, J_SEG_NEW, p.buf);
    }
    if (seg.records.size() > seg.journalCount || !seg.journalCreated) {
      uint64_t off = seg.journalCount;
      uint64_t n = seg.records.size() - off;
      if (n) {
        ByteWriter p;
        p.u64(seg.id);
        p.u64(off);
        p.u64(n);
        encodeRecords(seg.records.data() + off, size_t(n), p.buf);
        putRecord(w, J_SEG_APPEND, p.buf);
      }
    }
    pend.push_back({&seg, seg.records.size()});
  }
  std::string meta = metaJson(s);
  bool metaChanged = meta != lastMeta_;
  if (metaChanged) putRecord(w, J_META, std::vector<uint8_t>(meta.begin(), meta.end()));
  if (w.buf.empty()) return Status::Ok();

  const std::string jp = path(kJournalRel);
  int64_t before = fs::fileSize(jp);
  if (before < 0) return Error(Err::Io, "journal missing: " + jp);
  Status st = fs::appendFileSync(jp, w.buf.data(), w.buf.size());
  if (!st.ok()) {
    fs::truncateFile(jp, uint64_t(before));  // drop the partial record so later appends stay parseable
    return st;
  }
  for (auto& p : pend) {
    p.seg->journalCreated = true;
    p.seg->journalCount = p.newCount;
  }
  if (metaChanged) lastMeta_ = meta;
  return Status::Ok();
}

// ------------------------------------------------------------------ open
Status ProjectStore::open(const std::string& dir, const std::string& romOverride, const SessionOptions& optIn,
                          std::unique_ptr<Session>& out, OpenReport* report) {
  OpenReport rep;
  if (!fs::isDir(dir)) return Error(Err::NotFound, "project not found: " + dir);
  std::unique_ptr<ProjectStore> ps(new ProjectStore(dir));

  Json man;
  RN_TRY(parseJsonFile(ps->path("manifest.json"), man));
  if (man["format"].asString() != "replaynes-project") return Error(Err::Corrupt, "not a ReplayNES project");
  int64_t fv = man["formatVersion"].asInt(-1);
  if (fv < 1) return Error(Err::Corrupt, "manifest has no formatVersion");
  if (fv > int64_t(kProjectFormatVersion))
    return Error(Err::UnsupportedFormat, "project format " + std::to_string(fv) + " is newer than supported " +
                                             std::to_string(kProjectFormatVersion));
  if (man["inputFormatVersion"].asInt(-1) != kInputFormatVersion)
    return Error(Err::UnsupportedFormat, "unsupported inputFormatVersion");
  if (man["region"].asString() != "NTSC") return Error(Err::UnsupportedFormat, "unsupported region");

  // Core compatibility: refuse anything that is not bit-exactly this core.
  const std::string compat = man["coreCompatId"].asString();
  SessionOptions opt = optIn;
  if (compat == coreCompatId(CoreKind::Nestopia)) opt.core = CoreKind::Nestopia;
  else if (compat == coreCompatId(CoreKind::Mock)) opt.core = CoreKind::Mock;
  else
    return Error(Err::CoreMismatch, "project was recorded with core '" + compat + "' (build '" +
                                        man["coreBuild"].asString() + "'); this build provides '" +
                                        coreCompatId(CoreKind::Nestopia) +
                                        "'. Open it with a matching ReplayNES build (see COMPATIBILITY.md).");

  // ROM: never stored; located by override or last path, verified by SHA-256.
  const Json& romJ = man["rom"];
  const std::string wantSha = romJ["sha256"].asString();
  std::string romPath = romOverride.empty() ? romJ["lastPath"].asString() : romOverride;
  std::vector<uint8_t> rom;
  Status rs = fs::readFile(romPath, rom);
  if (!rs.ok()) {
    if (rs.code == Err::NotFound)
      return Error(Err::RomNotFound, "ROM not found at '" + romPath + "' (expected SHA-256 " + wantSha + ")");
    return rs;
  }
  std::string gotSha = Sha256::hex(rom.data(), rom.size());
  if (gotSha != wantSha)
    return Error(Err::RomMismatch, "ROM '" + romPath + "' has SHA-256 " + gotSha + ", project expects " + wantSha);

  // Index (generation must equal manifest's, or be exactly one ahead = crash before manifest).
  Json idx;
  RN_TRY(parseJsonFile(ps->path("timeline/index.json"), idx));
  uint64_t mgen = uint64_t(man["generation"].asInt(0));
  uint64_t igen = uint64_t(idx["generation"].asInt(0));
  if (!(igen == mgen || igen == mgen + 1))
    return Error(Err::Corrupt, "index generation " + std::to_string(igen) + " inconsistent with manifest " +
                                   std::to_string(mgen));
  ps->gen_ = igen;

  std::unique_ptr<Session> s(new Session());
  s->opt_ = opt;
  s->cps_ = CheckpointStore(opt.checkpoints);
  s->core_ = createCore(opt.core);
  s->rom_ = std::move(rom);
  s->romPath_ = romPath;
  s->romSha_ = gotSha;

  for (auto& e : idx["segments"].items()) {
    uint64_t id = uint64_t(e["id"].asInt());
    uint64_t count = uint64_t(e["count"].asInt());
    std::string rel = segRel(id);
    std::vector<uint8_t> b;
    Status st = fs::readFile(ps->path(rel), b);
    if (!st.ok()) return Error(Err::Corrupt, "cannot read " + rel + ": " + st.message);
    Segment seg;
    RN_TRY(decodeSegment(rel, b, seg));
    if (seg.id != id || seg.parent != uint64_t(e["parent"].asInt()) || seg.start != uint64_t(e["start"].asInt()))
      return Error(Err::Corrupt, "segment header disagrees with index: " + rel);
    if (seg.records.size() < count) return Error(Err::Corrupt, "segment shorter than index says: " + rel);
    seg.records.resize(size_t(count));  // file may hold appends from an uncommitted save
    seg.fileCount = count;
    seg.journalCount = count;
    seg.journalCreated = true;
    RN_TRY(s->tl_.insertLoaded(std::move(seg)));
  }
  s->tl_.setCounters(std::max<uint64_t>(s->tl_.nextId(), uint64_t(idx["nextSegmentId"].asInt(1))),
                     std::max<uint64_t>(s->tl_.seq(), uint64_t(idx["seq"].asInt(0))));

  for (auto& e : idx["checkpoints"].items()) {
    Checkpoint c;
    std::string rel = e["file"].asString();
    std::vector<uint8_t> b;
    Status st = fs::readFile(ps->path(rel), b);
    if (st.ok()) st = decodeState(rel, b, c);
    if (st.ok() && (c.id != uint64_t(e["id"].asInt()) || c.frame != uint64_t(e["frame"].asInt()) ||
                    c.owner != uint64_t(e["owner"].asInt()) || c.crc != uint32_t(e["crc32"].asInt())))
      st = Error(Err::Corrupt, "state metadata disagrees with index: " + rel);
    if (st.ok() && c.compat != compat)
      st = Error(Err::CoreMismatch, "state " + rel + " was produced by core '" + c.compat + "'");
    CpKind k;
    if (st.ok() && (!kindFromName(e["kind"].asString(), k) || k != c.kind))
      st = Error(Err::Corrupt, "bad checkpoint kind: " + rel);
    if (!st.ok()) {
      if (opt.dropCorruptStates && st.code != Err::CoreMismatch) {
        rep.droppedStates.push_back(rel + ": " + st.message);
        continue;
      }
      if (st.code == Err::Corrupt || st.code == Err::NotFound)
        return Error(Err::Corrupt, st.message + " (states are a cache: reopen with drop-corrupt-states to discard)");
      return st;
    }
    c.persisted = true;
    RN_TRY(s->cps_.insertLoaded(std::move(c)));
  }
  s->cps_.setNextId(uint64_t(idx["nextCheckpointId"].asInt(1)));

  if (!idx["meta"].isObject()) return Error(Err::Corrupt, "index has no meta");
  RN_TRY(ps->applyMeta(*s, idx["meta"].dump(0)));
  ps->lastMeta_ = ps->metaJson(*s);

  // Journal replay (crash recovery).
  const std::string jp = ps->path(kJournalRel);
  std::vector<uint8_t> jb;
  Status js = fs::readFile(jp, jb);
  if (js.ok()) {
    ByteReader r(jb.data(), jb.size());
    uint32_t magic = r.u32(), ver = r.u32();
    uint64_t base = r.u64();
    uint32_t hcrc = r.u32();
    if (!r.ok() || magic != kJrnMagic || ver != kBinVersion || crc32(jb.data(), 16) != hcrc)
      return Error(Err::Corrupt, "journal header is corrupt");
    if (base > igen) return Error(Err::Corrupt, "journal is newer than the index");
    if (base == igen) {
      ps->journalBase_ = base;
      size_t goodEnd = r.pos();
      std::string lastMetaText;
      while (r.remaining() > 0) {
        size_t recStart = r.pos();
        bool torn = false;
        const uint8_t* body = nullptr;
        uint32_t len = 0;
        if (r.remaining() < 12) {
          torn = true;
        } else {
          uint32_t magic = r.u32();
          len = r.u32();
          if (magic != kRecMagic || len == 0 || uint64_t(len) + 4 > r.remaining()) {
            torn = true;
          } else {
            body = r.ptr(len);
            uint32_t crc = r.u32();
            if (crc32(body, len) != crc) torn = true;
          }
        }
        if (torn) {
          if (validRecordAfter(jb, recStart))
            return Error(Err::Corrupt, "journal record at offset " + std::to_string(recStart) +
                                           " is corrupt and is followed by valid records");
          rep.journalTornTail = true;
          break;
        }
        ByteReader p(body + 1, len - 1);
        switch (body[0]) {
          case J_SEG_NEW: {
            Segment seg;
            seg.id = p.u64();
            seg.parent = p.u64();
            seg.start = p.u64();
            seg.createdSeq = p.u64();
            if (!p.ok()) return Error(Err::Corrupt, "bad journal SEG_NEW");
            seg.journalCreated = true;
            if (!s->tl_.segment(seg.id)) RN_TRY(s->tl_.insertLoaded(std::move(seg)));
            break;
          }
          case J_SEG_APPEND: {
            uint64_t id = p.u64(), off = p.u64(), n = p.u64();
            if (!p.ok() || n > (uint64_t(1) << 32)) return Error(Err::Corrupt, "bad journal SEG_APPEND");
            std::vector<InputRecord> recs;
            if (!decodeRecords(body + 1 + p.pos(), len - 1 - p.pos(), n, recs))
              return Error(Err::Corrupt, "bad journal records");
            RN_TRY(s->tl_.appendLoaded(id, off, recs));
            Segment& seg = s->tl_.mutableSegments().at(id);
            seg.journalCount = seg.records.size();
            break;
          }
          case J_META:
            lastMetaText.assign(reinterpret_cast<const char*>(body + 1), len - 1);
            RN_TRY(ps->applyMeta(*s, lastMetaText));
            break;
          default:
            return Error(Err::Corrupt, "unknown journal record type");
        }
        rep.journalApplied = true;
        goodEnd = r.pos();
      }
      if (rep.journalTornTail) RN_TRY(fs::truncateFile(jp, goodEnd));
      if (rep.journalApplied) ps->lastMeta_ = ps->metaJson(*s);
    }
  } else if (js.code != Err::NotFound) {
    return js;
  }

  RN_TRY(s->tl_.validate());
  if (s->frame_ > s->tl_.length()) return Error(Err::Corrupt, "cursor beyond take end");
  for (auto& b : s->bookmarks_)
    if (b.owner != 0 && !s->tl_.segment(b.owner)) return Error(Err::Corrupt, "bookmark refers to missing take");

  // Bring the core to the cursor (power-on + nearest checkpoint + replay).
  uint64_t cursor = s->frame_;
  s->frame_ = 0;
  RN_TRY(s->powerOnFresh());
  RN_TRY(s->resync(cursor, true));

  s->recovered_ = rep.journalApplied;
  // Recovered journal data / a relocated ROM are not in a committed full save yet.
  if (rep.journalApplied || romPath != romJ["lastPath"].asString()) s->changeSeq_ = 1;
  s->store_ = std::move(ps);
  if (report) *report = rep;
  out = std::move(s);
  return Status::Ok();
}

}  // namespace rn
