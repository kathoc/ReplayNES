// Offline renderer (export path) and an end-to-end C API flow.
#include <thread>

#include "render/OfflineRenderer.h"
#include "replaynes/replaynes.h"
#include "support/TestUtil.h"
#include "support/rn_test.h"

using namespace rn;
using namespace rntest;

TEST_CASE("render: hashes equal a normal replay; session/timeline unchanged by rendering") {
  auto s = newSession(CoreKind::Nestopia);
  auto script = DeterminismHarness::randomScript(1500, 41, 600, 4);
  REQUIRE(recordScript(*s, script).ok());
  REQUIRE(s->rewind(700).ok());
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(400, 42)).ok());  // final take has a branch
  // Normal replay from frame 0 in the session.
  uint64_t take = s->activeTake(), len = s->takeLength();
  REQUIRE(s->seek(0).ok());
  s->setMode(Mode::Replay);
  std::vector<uint64_t> vh, ah;
  Hasher64 combined;
  for (uint64_t f = 0; f < len; ++f) {
    REQUIRE(s->step(0, 0, 0).ok());
    vh.push_back(s->videoHash());
    ah.push_back(s->audioHash());
    combined.u64(vh.back());
    combined.u64(ah.back());
  }
  auto logBefore = s->timeline().flattenActive();
  size_t takesBefore = s->takes().size();
  uint64_t frameBefore = s->frame(), stateBefore = s->stateHash();

  std::unique_ptr<OfflineRenderer> r;
  REQUIRE(OfflineRenderer::create(*s, 0, 0, r).ok());
  CHECK_EQ(r->totalFrames(), len);
  bool same = true;
  uint64_t idx = 0, samples = 0;
  const uint32_t* v;
  const int16_t* a;
  size_t n;
  uint64_t fi;
  while (true) {
    Status st = r->next(&v, &a, &n, &fi);
    if (st.code == Err::EndOfTake) break;
    REQUIRE(st.ok());
    CHECK_EQ(fi, idx);
    same = same && Hasher64::of(v, 256 * 240 * 4) == vh[idx] && Session::audioHashOf(a, n) == ah[idx];
    CHECK_EQ(samples, audioSamplesBefore(idx));  // audio timeline derived from frame count only
    samples += n;
    ++idx;
  }
  CHECK(same);
  CHECK_EQ(idx, len);
  CHECK_EQ(r->hash(), combined.digest());
  CHECK_EQ(r->framesDone(), len);
  // Session untouched.
  CHECK(s->timeline().flattenActive() == logBefore);
  CHECK_EQ(s->takes().size(), takesBefore);
  CHECK_EQ(s->activeTake(), take);
  CHECK_EQ(s->frame(), frameBefore);
  CHECK_EQ(s->stateHash(), stateBefore);

  // Sub-range render matches the same frames of the full render.
  std::unique_ptr<OfflineRenderer> part;
  REQUIRE(OfflineRenderer::create(*s, 500, 900, part).ok());
  CHECK_EQ(part->totalFrames(), 400u);
  bool partSame = true;
  for (uint64_t f = 500; f < 900; ++f) {
    REQUIRE(part->next(&v, &a, &n, &fi).ok());
    partSame = partSame && fi == f && Hasher64::of(v, 256 * 240 * 4) == vh[f] && Session::audioHashOf(a, n) == ah[f];
  }
  CHECK(partSame);
  CHECK_EQ(int(part->next(&v, &a, &n, &fi).code), int(Err::EndOfTake));
  CHECK_EQ(int(OfflineRenderer::create(*s, 10, len + 1, part).code), int(Err::OutOfRange));
}

TEST_CASE("render: runs on another thread while the session keeps recording") {
  auto s = newSession(CoreKind::Nestopia);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(1200, 7, 500)).ok());
  auto snapshot = s->timeline().flattenActive();
  Replay ref = straightReplay(CoreKind::Nestopia, snapshot);
  std::unique_ptr<OfflineRenderer> r;
  REQUIRE(OfflineRenderer::create(*s, 0, 0, r).ok());
  bool same = true;
  std::thread th([&] {
    const uint32_t* v;
    const int16_t* a;
    size_t n;
    uint64_t f;
    while (r->next(&v, &a, &n, &f).ok())
      same = same && Hasher64::of(v, 256 * 240 * 4) == ref.video[f] && Session::audioHashOf(a, n) == ref.audio[f];
  });
  // Meanwhile: rewind and branch, keep playing (the renderer copied the take).
  REQUIRE(s->rewind(600).ok());
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(900, 8)).ok());
  th.join();
  CHECK(same);
  CHECK_EQ(r->framesDone(), 1200u);
}

TEST_CASE("C API: end-to-end flow (new, record, bookmark, branch, undo, render, save, reopen)") {
  std::string root = tempDir("capi");
  std::string rom = root + "/game.nes";
  REQUIRE_EQ(rn_write_test_rom(rom.c_str()), RN_OK);
  char sha[65];
  REQUIRE_EQ(rn_sha256_file(rom.c_str(), sha), RN_OK);
  CHECK_EQ(std::string(sha).size(), size_t(64));
  std::string proj = root + "/play.nesrec";
  rn_session* s = nullptr;
  REQUIRE_EQ(rn_session_new(rom.c_str(), proj.c_str(), nullptr, &s), RN_OK);
  CHECK_EQ(std::string(rn_session_rom_sha256(s)), std::string(sha));
  CHECK_EQ(std::string(rn_session_project_dir(s)), proj);
  rn_input* in = rn_input_new();
  REQUIRE_EQ(rn_input_bind(in, "kb:x", "p1.a"), RN_OK);
  REQUIRE_EQ(rn_input_bind(in, "kb:t", "p1.turbo_b"), RN_OK);
  REQUIRE_EQ(rn_input_bind(in, "kb:b", "hk.bookmark"), RN_OK);
  for (int f = 0; f < 600; ++f) {
    rn_input_set_pressed(in, "kb:x", (f / 20) % 2);
    rn_input_set_pressed(in, "kb:t", (f / 45) % 2);
    rn_input_set_pressed(in, "kb:b", f == 300);
    uint32_t edges = 0, held = 0;
    rn_input_poll_hotkeys(in, &edges, &held);
    if (edges & RN_HK_BOOKMARK) REQUIRE_EQ(rn_bookmark_add(s, "boss", nullptr), RN_OK);
    uint8_t p1, p2;
    rn_input_sample_game(in, rn_frame(s), &p1, &p2);
    REQUIRE_EQ(rn_step(s, p1, p2, f == 450 ? RN_EV_SOFT_RESET : 0, nullptr), RN_OK);
  }
  const uint32_t* px = rn_video(s);
  REQUIRE(px != nullptr);
  CHECK_EQ(rn_bookmark_count(s), size_t(1));
  rn_bookmark_info bi;
  REQUIRE_EQ(rn_bookmark_get(s, 0, &bi), RN_OK);
  CHECK_EQ(bi.frame, 300u);
  CHECK_EQ(std::string(bi.name), std::string("boss"));
  CHECK(bi.on_active_take);
  REQUIRE_EQ(rn_rewind(s, 100), RN_OK);
  rn_step_info info;
  REQUIRE_EQ(rn_step(s, RN_BTN_START, 0, 0, &info), RN_OK);
  CHECK(info.branched);
  CHECK_EQ(rn_take_count(s), size_t(2));
  rn_take_info ti;
  REQUIRE_EQ(rn_take_get(s, 1, &ti), RN_OK);
  CHECK(ti.is_active);
  CHECK_EQ(ti.branch_frame, 500u);
  CHECK_EQ(ti.length, 501u);
  REQUIRE_EQ(rn_undo_take_switch(s), RN_OK);
  CHECK_EQ(rn_take_length(s), 600u);
  CHECK_EQ(rn_seek(s, 9999), RN_ERR_OUT_OF_RANGE);
  CHECK(std::string(rn_last_error()).find("out_of_range") != std::string::npos);
  rn_renderer* r = nullptr;
  REQUIRE_EQ(rn_renderer_new(s, 0, 0, &r), RN_OK);
  CHECK_EQ(rn_renderer_total_frames(r), 600u);
  const uint32_t* v;
  const int16_t* a;
  size_t n;
  uint64_t fi;
  rn_status st;
  while ((st = rn_renderer_next(r, &v, &a, &n, &fi)) == RN_OK) {}
  CHECK_EQ(st, RN_ERR_END_OF_TAKE);
  rn_renderer_free(r);
  REQUIRE_EQ(rn_seek(s, 600), RN_OK);
  uint64_t h = rn_state_hash(s);
  REQUIRE_EQ(rn_session_save(s), RN_OK);
  rn_session_close(s);
  rn_input_free(in);

  char* manifest = nullptr;
  REQUIRE_EQ(rn_project_manifest_json(proj.c_str(), &manifest), RN_OK);
  CHECK(std::string(manifest).find(rn_core_compat_id()) != std::string::npos);
  rn_string_free(manifest);
  REQUIRE_EQ(rn_session_open(proj.c_str(), nullptr, nullptr, &s), RN_OK);
  CHECK_EQ(rn_frame(s), 600u);
  CHECK_EQ(rn_state_hash(s), h);
  CHECK_EQ(rn_session_recovered(s), 0);
  rn_session_close(s);
  CHECK_EQ(rn_session_open((root + "/missing.nesrec").c_str(), nullptr, nullptr, &s), RN_ERR_NOT_FOUND);
}

TEST_CASE("video indices: raw PPU codes describe the same picture as rn_video; display-only") {
  auto s = newSession(CoreKind::Nestopia);
  auto script = DeterminismHarness::randomScript(240, 77, 0, 0);
  REQUIRE(recordScript(*s, script).ok());
  uint32_t phase = 99;
  uint64_t frame = 0;
  const uint16_t* codes = s->videoCodes(&phase, &frame);
  REQUIRE(codes != nullptr);
  CHECK(phase <= 2u);
  CHECK_EQ(frame, s->frame() - 1);  // the frame just emulated
  // Every code is a 9-bit palette index + emphasis, and the RGB picture is a function of it.
  std::vector<uint32_t> rgbOf(512, 0);
  std::vector<bool> seen(512, false);
  bool valid = true, consistent = true;
  const uint32_t* v = s->video();
  for (size_t i = 0; i < 256 * 240; ++i) {
    valid = valid && codes[i] <= 511;
    uint16_t c = codes[i] & 511;
    if (!seen[c]) { seen[c] = true; rgbOf[c] = v[i]; }
    consistent = consistent && rgbOf[c] == v[i];
  }
  CHECK(valid);
  CHECK(consistent);
  // Reading the side channel changes nothing: same hashes as a straight replay.
  Replay ref = straightReplay(CoreKind::Nestopia, s->timeline().flattenActive());
  CHECK_EQ(s->videoHash(), ref.video.back());
  CHECK_EQ(s->stateHash(), ref.finalState);
  // The burst phase follows the frame sequence (Nestopia advances it every frame).
  std::vector<uint32_t> phases;
  for (int i = 0; i < 6; ++i) {
    REQUIRE(s->step(0, 0, 0).ok());
    uint32_t p = 0;
    REQUIRE(s->videoCodes(&p, &frame) != nullptr);
    CHECK_EQ(frame, s->frame() - 1);
    phases.push_back(p);
  }
  bool changes = false;
  for (size_t i = 1; i < phases.size(); ++i) changes = changes || phases[i] != phases[i - 1];
  CHECK(changes);

  // C API: session and offline renderer report identical codes for the same frame.
  REQUIRE(s->seek(100).ok());
  std::unique_ptr<OfflineRenderer> r;
  REQUIRE(OfflineRenderer::create(*s, 99, 100, r).ok());
  const uint32_t* rv;
  const int16_t* ra;
  size_t n;
  uint64_t fi;
  REQUIRE(r->next(&rv, &ra, &n, &fi).ok());
  uint32_t rp = 0, sp = 0;
  uint64_t rf = 0, sf = 0;
  const uint16_t* rc = r->videoCodes(&rp, &rf);
  const uint16_t* sc = s->videoCodes(&sp, &sf);
  REQUIRE(rc != nullptr);
  REQUIRE(sc != nullptr);
  CHECK_EQ(rf, 99u);
  CHECK_EQ(sf, 99u);
  CHECK_EQ(rp, sp);
  CHECK(std::equal(rc, rc + 256 * 240, sc));

  // Mock core: explicit "unsupported", never a fake picture.
  auto m = newSession(CoreKind::Mock);
  REQUIRE(m->step(0, 0, 0).ok());
  CHECK(m->videoCodes(nullptr, nullptr) == nullptr);
}

TEST_CASE("video indices: burst phase of a frame is the same by straight play, seek, rewind and state load") {
  // Regression: Nestopia's Ppu::GetBurstPhase() is not in states and LoadState resets it to 0, so
  // a frame reached by a seek / rewind / take switch / load reported another phase than in
  // straight play (the CRT then modulated the same picture with another subcarrier phase).
  CheckpointPolicy pol;
  pol.denseInterval = 31;  // odd spacing: checkpoints at both frame parities and every phase
  pol.sparseInterval = 589;
  auto s = newSession(CoreKind::Nestopia, pol);
  auto script = DeterminismHarness::randomScript(1300, 91, 500, 4);  // includes soft/hard resets
  REQUIRE(recordScript(*s, script).ok());
  auto log = s->timeline().flattenActive();
  // Straight play on a fresh core: (phase, codes hash) of every frame.
  auto core = createCore(CoreKind::Nestopia);
  std::vector<uint8_t> rom = buildTestRom();
  REQUIRE(core->loadROM(rom.data(), rom.size()).ok());
  std::vector<uint32_t> phase;
  std::vector<uint64_t> codesHash;
  std::vector<std::vector<uint8_t>> states(log.size());
  for (size_t i = 0; i < log.size(); ++i) {
    if (i % 97 == 13) REQUIRE(core->saveState(states[i]).ok());  // state BEFORE frame i
    REQUIRE(core->stepRecord(log[i].p1, log[i].p2, log[i].events, true).ok());
    uint32_t p = 99;
    uint64_t f = 0;
    const uint16_t* c = core->videoCodes(&p, &f);
    REQUIRE(c != nullptr);
    CHECK_EQ(f, uint64_t(i));
    phase.push_back(p);
    codesHash.push_back(Hasher64::of(c, 256 * 240 * 2));
  }
  // Physical phase: every frame advances it by 1 (89342 dots) or 2 (89341, odd-frame dot skip),
  // never 0; with rendering on, short and long frames alternate (two-frame pattern).
  size_t same = 0, twoFramePeriod = 0;
  for (size_t i = 400; i + 2 < phase.size(); ++i) {
    if (log[i + 1].events || log[i + 2].events) continue;  // a reset restarts the clock count
    same += phase[i] == phase[i + 1];
    twoFramePeriod += phase[i] == phase[i + 2];
  }
  CHECK_EQ(same, 0u);
  CHECK(twoFramePeriod > (phase.size() - 402) * 9 / 10);

  // Seek (checkpoint load + replay) to frames of both parities, around checkpoint boundaries.
  for (uint64_t t : {1u, 2u, 31u, 32u, 33u, 63u, 64u, 95u, 96u, 334u, 335u, 499u, 500u, 501u, 502u, 777u, 778u, 1299u, 1300u}) {
    REQUIRE(s->seek(t).ok());
    uint32_t p = 99;
    uint64_t f = 0;
    const uint16_t* c = s->videoCodes(&p, &f);
    REQUIRE(c != nullptr);
    CHECK_EQ(f, t - 1);
    CHECK_EQ(p, phase[t - 1]);
    CHECK_EQ(Hasher64::of(c, 256 * 240 * 2), codesHash[t - 1]);
  }
  // Rewind by one and by many frames.
  REQUIRE(s->seek(900).ok());
  for (uint64_t n : {1u, 1u, 7u, 60u, 301u}) {
    REQUIRE(s->rewind(n).ok());
    uint32_t p = 99;
    uint64_t f = 0;
    REQUIRE(s->videoCodes(&p, &f) != nullptr);
    REQUIRE(s->frame() >= 1);
    CHECK_EQ(p, phase[s->frame() - 1]);
  }
  // Raw state load into a new core, then several frames of play (odd and even).
  for (size_t i = 0; i < states.size(); ++i) {
    if (states[i].empty()) continue;
    auto c2 = createCore(CoreKind::Nestopia);
    REQUIRE(c2->loadROM(rom.data(), rom.size()).ok());
    REQUIRE(c2->loadState(states[i].data(), states[i].size()).ok());
    for (size_t k = i; k < std::min(log.size(), i + 5); ++k) {
      REQUIRE(c2->stepRecord(log[k].p1, log[k].p2, log[k].events, true).ok());
      uint32_t p = 99;
      REQUIRE(c2->videoCodes(&p, nullptr) != nullptr);
      CHECK_EQ(p, phase[k]);
    }
  }
}

TEST_CASE("render: single frames seeded from checkpoints equal a straight replay (thumbnails)") {
  auto s = newSession(CoreKind::Nestopia);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(1300, 71, 600, 4)).ok());
  REQUIRE(s->rewind(500).ok());
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(900, 72)).ok());  // branch at 800
  auto log = s->timeline().flattenActive();
  Replay ref = straightReplay(CoreKind::Nestopia, log);
  std::vector<std::unique_ptr<OfflineRenderer>> rs;
  std::vector<uint64_t> at = {1, 59, 60, 299, 600, 777, 799, 800, 801, 1234, 1699};
  for (uint64_t f : at) {
    std::unique_ptr<OfflineRenderer> r;
    REQUIRE(OfflineRenderer::create(*s, f, f + 1, r).ok());
    rs.push_back(std::move(r));
  }
  // The renderers own copies: the session may move on (another branch) before they run.
  REQUIRE(s->seek(100).ok());
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(50, 73)).ok());
  std::vector<std::thread> threads;
  std::vector<int> ok(at.size(), 0);
  for (size_t i = 0; i < at.size(); ++i)
    threads.emplace_back([&, i] {
      const uint32_t* v;
      const int16_t* a;
      size_t n;
      uint64_t fi;
      ok[i] = rs[i]->next(&v, &a, &n, &fi).ok() && fi == at[i] && Hasher64::of(v, 256 * 240 * 4) == ref.video[size_t(at[i])] &&
                      Session::audioHashOf(a, n) == ref.audio[size_t(at[i])]
                  ? 1
                  : 0;
    });
  for (auto& t : threads) t.join();
  for (size_t i = 0; i < at.size(); ++i) CHECK_EQ(ok[i], 1);
}
