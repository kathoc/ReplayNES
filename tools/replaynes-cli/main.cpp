// replaynes-cli: developer tool over the portable engine (no UI).
//   make-test-rom <out.nes>                 write the generated ReplayNES test ROM (own code)
//   sha256 <file>
//   info <project.nesrec> [--rom PATH]      manifest, takes, bookmarks, checkpoints
//   verify <project.nesrec> [--rom PATH]    replay active take from power-on on a fresh core and
//                                           compare against every stored checkpoint on that take
//   determinism <rom> [--frames N] [--runs R] [--mid F] [--seed S] [--reset-every K] [--mock]
//   record-random <rom> <project.nesrec> [--frames N] [--seed S] [--mock]
//   render-hash <project.nesrec> [--rom PATH]   run the export renderer, print hash + timing
//   screenshot <rom> --out PREFIX [--at F1,F2,..] [--press btn@F[+D],..] [--p2]
//                                           run from power-on, write PREFIX_<frame>.png after the
//                                           given frames; btn = a b select start up down left right,
//                                           held for D frames (default 6) starting at frame F
//   version
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/ICore.h"
#include "harness/DeterminismHarness.h"
#include "persist/ProjectStore.h"
#include "render/OfflineRenderer.h"
#include "replaynes/replaynes.h"
#include "session/Session.h"
#include "testrom/TestRom.h"
#include "util/Fs.h"
#include "util/Hash.h"

using namespace rn;

namespace {

struct Args {
  std::vector<std::string> pos;
  std::string get(const std::string& k, const std::string& def = "") const {
    for (size_t i = 0; i + 1 < flags.size(); ++i)
      if (flags[i] == k) return flags[i + 1];
    return def;
  }
  bool has(const std::string& k) const {
    for (auto& f : flags) if (f == k) return true;
    return false;
  }
  uint64_t num(const std::string& k, uint64_t def) const {
    std::string v = get(k);
    return v.empty() ? def : std::strtoull(v.c_str(), nullptr, 10);
  }
  std::vector<std::string> flags;
};

int fail(const Status& st) {
  std::fprintf(stderr, "error [%s]: %s\n", errName(st.code), st.message.c_str());
  return 2 + int(st.code);
}

int usage() {
  std::fprintf(stderr,
               "usage: replaynes-cli <command> ...\n"
               "  make-test-rom <out.nes>\n"
               "  sha256 <file>\n"
               "  info <project.nesrec> [--rom PATH]\n"
               "  verify <project.nesrec> [--rom PATH]\n"
               "  determinism <rom> [--frames N] [--runs R] [--mid F] [--seed S] [--reset-every K] [--mock]\n"
               "  record-random <rom> <project.nesrec> [--frames N] [--seed S] [--mock]\n"
               "  render-hash <project.nesrec> [--rom PATH]\n"
               "  screenshot <rom> --out PREFIX [--at F1,F2,..] [--press btn@F[+D],..]\n"
               "  version\n");
  return 1;
}

Status openProject(const Args& a, std::unique_ptr<Session>& s, OpenReport* rep = nullptr) {
  SessionOptions o;
  return ProjectStore::open(a.pos[1], a.get("--rom"), o, s, rep);
}

int cmdInfo(const Args& a) {
  std::unique_ptr<Session> s;
  OpenReport rep;
  Status st = openProject(a, s, &rep);
  if (!st.ok()) return fail(st);
  std::printf("project      %s\n", a.pos[1].c_str());
  std::printf("core         %s\n", s->core().compatId().c_str());
  std::printf("rom          %s (sha256 %s)\n", s->romPath().c_str(), s->romSha256().c_str());
  std::printf("recovered    %s%s\n", rep.journalApplied ? "yes (journal replayed)" : "no",
              rep.journalTornTail ? ", torn journal tail discarded" : "");
  std::printf("cursor       frame %llu / take length %llu, mode %s\n", (unsigned long long)s->frame(),
              (unsigned long long)s->takeLength(), s->mode() == Mode::Replay ? "replay" : "record");
  std::printf("takes        %zu (active %llu, undo depth %zu)\n", s->takes().size(),
              (unsigned long long)s->activeTake(), s->undoDepth());
  for (auto& t : s->takes())
    std::printf("  take %-4llu parent %-4llu branch@%-8llu length %-8llu%s\n", (unsigned long long)t.id,
                (unsigned long long)t.parent, (unsigned long long)t.branchFrame, (unsigned long long)t.length,
                t.active ? "  *active" : "");
  std::printf("bookmarks    %zu\n", s->bookmarks().size());
  for (auto& b : s->bookmarks())
    std::printf("  #%llu frame %llu take %llu \"%s\"\n", (unsigned long long)b.id, (unsigned long long)b.frame,
                (unsigned long long)b.owner, b.name.c_str());
  std::printf("checkpoints  %zu (%zu bytes in memory)\n", s->checkpoints().all().size(), s->checkpoints().memoryBytes());
  return 0;
}

int cmdVerify(const Args& a) {
  std::unique_ptr<Session> s;
  Status st = openProject(a, s);
  if (!st.ok()) return fail(st);
  auto recs = s->timeline().flattenActive();
  std::vector<const Checkpoint*> cps;
  for (auto& kv : s->checkpoints().all())
    if (kv.second.frame > 0 && s->timeline().stateValid(kv.second.frame, kv.second.owner)) cps.push_back(&kv.second);
  std::sort(cps.begin(), cps.end(), [](const Checkpoint* x, const Checkpoint* y) { return x->frame < y->frame; });
  auto core = createCore(s->coreKind());
  st = core->loadROM(s->rom().data(), s->rom().size());
  if (!st.ok()) return fail(st);
  size_t next = 0, bad = 0;
  auto t0 = std::chrono::steady_clock::now();
  for (uint64_t f = 0; f < recs.size(); ++f) {
    st = core->stepRecord(recs[f].p1, recs[f].p2, recs[f].events, false);
    if (!st.ok()) return fail(st);
    while (next < cps.size() && cps[next]->frame == f + 1) {
      std::vector<uint8_t> mine;
      core->saveState(mine);
      if (mine != cps[next]->data) {
        ++bad;
        std::printf("MISMATCH at frame %llu (checkpoint %llu)\n", (unsigned long long)(f + 1),
                    (unsigned long long)cps[next]->id);
      }
      ++next;
    }
  }
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("verified %zu checkpoints over %zu frames in %.2fs: %s\n", cps.size(), recs.size(), secs,
              bad ? "DIVERGENCE" : "OK");
  return bad ? 1 : 0;
}

int cmdDeterminism(const Args& a) {
  std::vector<uint8_t> rom;
  Status st = fs::readFile(a.pos[1], rom);
  if (!st.ok()) return fail(st);
  HarnessConfig cfg;
  cfg.core = a.has("--mock") ? CoreKind::Mock : CoreKind::Nestopia;
  uint64_t frames = a.num("--frames", 10000);
  cfg.runs = int(a.num("--runs", 3));
  cfg.midFrame = a.num("--mid", frames / 2);
  cfg.stateEvery = uint32_t(a.num("--state-every", 60));
  auto script = DeterminismHarness::randomScript(frames, a.num("--seed", 1), a.num("--reset-every", 0));
  HarnessReport rep;
  st = DeterminismHarness::run(cfg, rom, script, rep);
  if (!st.ok()) return fail(st);
  std::printf("frames emulated %llu in %.2fs (%.0f fps)\n", (unsigned long long)rep.frames, rep.seconds,
              rep.frames / (rep.seconds > 0 ? rep.seconds : 1));
  std::printf("runs: %s\n", rep.runs.found ? rep.runs.detail.c_str() : "identical");
  if (cfg.midFrame) std::printf("mid-savestate@%llu: %s\n", (unsigned long long)cfg.midFrame,
                                rep.mid.found ? rep.mid.detail.c_str() : "identical");
  return rep.runs.found || rep.mid.found ? 1 : 0;
}

int cmdRecordRandom(const Args& a) {
  if (a.pos.size() < 3) return usage();
  std::vector<uint8_t> rom;
  Status st = fs::readFile(a.pos[1], rom);
  if (!st.ok()) return fail(st);
  SessionOptions o;
  o.core = a.has("--mock") ? CoreKind::Mock : CoreKind::Nestopia;
  std::unique_ptr<Session> s;
  st = Session::create(rom, a.pos[1], o, s);
  if (!st.ok()) return fail(st);
  st = s->saveAs(a.pos[2]);
  if (!st.ok()) return fail(st);
  auto script = DeterminismHarness::randomScript(a.num("--frames", 3600), a.num("--seed", 1));
  for (size_t i = 0; i < script.size(); ++i) {
    st = s->step(script[i].p1, script[i].p2, 0);
    if (!st.ok()) return fail(st);
    if (i == script.size() / 2) s->bookmarkAdd("midpoint", nullptr);
  }
  st = s->save();
  if (!st.ok()) return fail(st);
  std::printf("recorded %zu frames into %s (state hash %016llx)\n", script.size(), a.pos[2].c_str(),
              (unsigned long long)s->stateHash());
  return 0;
}

int cmdRenderHash(const Args& a) {
  std::unique_ptr<Session> s;
  Status st = openProject(a, s);
  if (!st.ok()) return fail(st);
  std::unique_ptr<OfflineRenderer> r;
  st = OfflineRenderer::create(*s, 0, 0, r);
  if (!st.ok()) return fail(st);
  auto t0 = std::chrono::steady_clock::now();
  uint64_t samples = 0;
  for (;;) {
    size_t n = 0;
    st = r->next(nullptr, nullptr, &n, nullptr);
    if (st.code == Err::EndOfTake) break;
    if (!st.ok()) return fail(st);
    samples += n;
  }
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  double media = double(r->totalFrames()) * kFpsDen / kFpsNum;
  std::printf("rendered %llu frames (%.3fs media, %llu samples) in %.2fs, hash %016llx\n",
              (unsigned long long)r->totalFrames(), media, (unsigned long long)samples, secs,
              (unsigned long long)r->hash());
  return 0;
}

std::vector<std::string> splitList(const std::string& s) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p <= s.size()) {
    size_t q = s.find(',', p);
    if (q == std::string::npos) q = s.size();
    if (q > p) out.push_back(s.substr(p, q - p));
    p = q + 1;
  }
  return out;
}

void put32be(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 3; i >= 0; --i) v.push_back(uint8_t(x >> (8 * i)));
}

// Minimal RGB PNG writer (stored deflate blocks, no compression library needed).
Status writePng(const std::string& path, const uint32_t* bgra, int w, int h) {
  std::vector<uint8_t> raw;
  raw.reserve(size_t(h) * (1 + 3 * w));
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    for (int x = 0; x < w; ++x) {
      uint32_t px = bgra[size_t(y) * w + x];
      raw.push_back(uint8_t(px >> 16));
      raw.push_back(uint8_t(px >> 8));
      raw.push_back(uint8_t(px));
    }
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  for (size_t off = 0; off < raw.size() || off == 0;) {
    size_t n = std::min<size_t>(65535, raw.size() - off);
    bool last = off + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + long(off), raw.begin() + long(off + n));
    off += n;
    if (last) break;
  }
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
  put32be(z, (b << 16) | a);
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
    put32be(png, uint32_t(data.size()));
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    png.insert(png.end(), td.begin(), td.end());
    put32be(png, crc32(td.data(), td.size()));
  };
  std::vector<uint8_t> ihdr;
  put32be(ihdr, uint32_t(w));
  put32be(ihdr, uint32_t(h));
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
  chunk("IHDR", ihdr);
  chunk("IDAT", z);
  chunk("IEND", {});
  return fs::writeFileAtomic(path, png.data(), png.size());
}

int cmdScreenshot(const Args& a) {
  std::vector<uint8_t> rom;
  Status st = fs::readFile(a.pos[1], rom);
  if (!st.ok()) return fail(st);
  const std::string prefix = a.get("--out");
  if (prefix.empty()) return usage();
  std::vector<uint64_t> at;
  for (auto& f : splitList(a.get("--at", "300"))) at.push_back(std::strtoull(f.c_str(), nullptr, 10));
  struct Press { uint8_t mask; uint64_t from, to; };
  std::vector<Press> presses;
  static const char* kNames[] = {"a", "b", "select", "start", "up", "down", "left", "right"};
  for (auto& p : splitList(a.get("--press"))) {
    size_t atPos = p.find('@');
    if (atPos == std::string::npos) return usage();
    std::string name = p.substr(0, atPos);
    uint8_t mask = 0;
    for (int i = 0; i < 8; ++i) if (name == kNames[i]) mask = uint8_t(1u << i);
    if (!mask) { std::fprintf(stderr, "unknown button %s\n", name.c_str()); return 1; }
    std::string rest = p.substr(atPos + 1);
    size_t plus = rest.find('+');
    uint64_t from = std::strtoull(rest.c_str(), nullptr, 10);
    uint64_t dur = plus == std::string::npos ? 6 : std::strtoull(rest.c_str() + plus + 1, nullptr, 10);
    presses.push_back({mask, from, from + dur});
  }
  auto core = createCore(a.has("--mock") ? CoreKind::Mock : CoreKind::Nestopia);
  st = core->loadROM(rom.data(), rom.size());
  if (!st.ok()) return fail(st);
  uint64_t last = at.empty() ? 0 : *std::max_element(at.begin(), at.end());
  for (uint64_t f = 0; f < last; ++f) {
    uint8_t p1 = 0;
    for (auto& p : presses) if (f >= p.from && f < p.to) p1 |= p.mask;
    st = core->stepFrame(p1, 0, true);
    if (!st.ok()) return fail(st);
    if (std::find(at.begin(), at.end(), f + 1) != at.end()) {
      std::string path = prefix + "_" + std::to_string(f + 1) + ".png";
      st = writePng(path, core->video(), kVideoWidth, kVideoHeight);
      if (!st.ok()) return fail(st);
      // crude content summary so a blank frame is obvious without looking at it
      const uint32_t* v = core->video();
      std::vector<uint32_t> colors;
      for (int i = 0; i < kVideoWidth * kVideoHeight && colors.size() < 64; ++i)
        if (std::find(colors.begin(), colors.end(), v[i]) == colors.end()) colors.push_back(v[i]);
      std::printf("frame %llu -> %s (%zu%s distinct colors, video hash %016llx)\n", (unsigned long long)(f + 1),
                  path.c_str(), colors.size(), colors.size() >= 64 ? "+" : "", (unsigned long long)core->videoHash());
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    if (s.size() > 2 && s[0] == '-' && s[1] == '-') {
      a.flags.push_back(s);
      if (s != "--mock" && i + 1 < argc) a.flags.push_back(argv[++i]);
    } else {
      a.pos.push_back(s);
    }
  }
  const std::string cmd = a.pos[0];
  if (cmd == "version") {
    std::printf("replaynes %s\ncore %s\nbuild %s\n", rn_version(), rn_core_compat_id(), rn_core_build_id());
    return 0;
  }
  if (a.pos.size() < 2) return usage();
  if (cmd == "make-test-rom") {
    std::vector<uint8_t> rom = buildTestRom();
    Status st = fs::writeFileAtomic(a.pos[1], rom.data(), rom.size());
    if (!st.ok()) return fail(st);
    std::printf("%s  %s\n", Sha256::hex(rom.data(), rom.size()).c_str(), a.pos[1].c_str());
    return 0;
  }
  if (cmd == "sha256") {
    char hex[65];
    if (rn_sha256_file(a.pos[1].c_str(), hex) != RN_OK) {
      std::fprintf(stderr, "%s\n", rn_last_error());
      return 2;
    }
    std::printf("%s  %s\n", hex, a.pos[1].c_str());
    return 0;
  }
  if (cmd == "info") return cmdInfo(a);
  if (cmd == "verify") return cmdVerify(a);
  if (cmd == "determinism") return cmdDeterminism(a);
  if (cmd == "record-random") return cmdRecordRandom(a);
  if (cmd == "render-hash") return cmdRenderHash(a);
  if (cmd == "screenshot") return cmdScreenshot(a);
  return usage();
}
