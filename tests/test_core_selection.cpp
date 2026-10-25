#include "support/TestUtil.h"
#include "support/rn_test.h"
#include "replaynes/replaynes.h"
#include "render/OfflineRenderer.h"
#include "core/MockCore.h"
using namespace rn;
using namespace rntest;

TEST_CASE("core selection: shared machine capability is opt in and queryable through C") {
  static_assert(unsigned(CapSharesMachine) == RN_CORE_CAP_SHARES_MACHINE, "C capability ABI");
  for (auto kind : {RN_CORE_NESTOPIA, RN_CORE_MOCK, RN_CORE_REPLAYNESCORE})
    CHECK_EQ(rn_core_capabilities(kind) & RN_CORE_CAP_SHARES_MACHINE, 0u);
  struct Registration {
    ~Registration() { unregisterExternalCore(); }
  } registration;
  registerExternalCore({"shared-test", MockCore::kCompatId,
                        uint32_t(CapAudio | CapState | CapRecording | CapSharesMachine),
                        [] { return std::make_unique<MockCore>(); }});
  CHECK_EQ(rn_core_capabilities(RN_CORE_EXTERNAL) & RN_CORE_CAP_SHARES_MACHINE,
           RN_CORE_CAP_SHARES_MACHINE);
}

static std::vector<uint8_t> idleRom() {
  std::vector<uint8_t> r(16 + 16384 + 8192, 0);
  r[0]='N'; r[1]='E'; r[2]='S'; r[3]=0x1a; r[4]=1; r[5]=1;
  r[16]=0x4c; r[17]=0; r[18]=0x80;
  for (int p : {0x3ffa,0x3ffc,0x3ffe}) { r[16+p]=0; r[17+p]=0x80; }
  return r;
}
TEST_CASE("core selection: names capabilities and invalid values never fall back") {
  CHECK_EQ(std::string(rn_core_name(RN_CORE_NESTOPIA)), std::string("nestopia"));
  CHECK_EQ(std::string(rn_core_name(RN_CORE_REPLAYNESCORE)), std::string("replaynescore"));
  CHECK_EQ(rn_core_capabilities(RN_CORE_NESTOPIA), RN_PRODUCT_NES ? 15u : 0u);
  CHECK_EQ(rn_core_available(RN_CORE_NESTOPIA), RN_PRODUCT_NES);
#ifdef REPLAYNES_HAS_REPLAYNESCORE
  CHECK_EQ(rn_core_available(RN_CORE_REPLAYNESCORE), 1);
  CHECK_EQ(rn_core_capabilities(RN_CORE_REPLAYNESCORE), unsigned(RN_CORE_CAP_AUDIO|RN_CORE_CAP_STATE));
#else
  // Not built: no capabilities, and a session request fails instead of falling back.
  CHECK_EQ(rn_core_available(RN_CORE_REPLAYNESCORE), 0);
  CHECK_EQ(rn_core_capabilities(RN_CORE_REPLAYNESCORE), 0u);
  {
    auto d=tempDir("core-selection-unbuilt"); auto p=writeTestRom(d);
    rn_session_options u; rn_session_options_init(&u); u.core=RN_CORE_REPLAYNESCORE;
    rn_session* us=nullptr;
    CHECK(rn_session_new(p.c_str(), nullptr, &u, &us) != RN_OK);
    CHECK(us == nullptr);
  }
#endif
  auto bad = static_cast<CoreKind>(99);
  CHECK(coreName(bad) == nullptr); CHECK_FALSE(coreAvailable(bad));
  CHECK(createCore(bad) == nullptr); CHECK(coreCompatId(bad).empty());
  SessionOptions opt; opt.core=bad;
  std::unique_ptr<Session> s;
  CHECK_EQ(int(Session::create(idleRom(), "idle", opt, s).code), int(Err::InvalidArg));
  auto dir=tempDir("core-selection"); auto path=writeTestRom(dir);
  rn_session_options o; rn_session_options_init(&o); o.core=static_cast<rn_core_kind>(99);
  rn_session* cs=nullptr;
  CHECK_EQ(rn_session_new(path.c_str(), nullptr, &o, &cs), RN_ERR_INVALID_ARG);
  CHECK(cs == nullptr);
}
TEST_CASE("core selection: project identity overrides new-session preference") {
  auto dir=tempDir("core-selection-project"); auto path=writeTestRom(dir);
  rn_session_options o; rn_session_options_init(&o); o.core=RN_CORE_MOCK;
  rn_session* s=nullptr;
  std::string project=dir+"/record.nesrec";
  REQUIRE_EQ(rn_session_new(path.c_str(), project.c_str(), &o, &s), RN_OK);
  REQUIRE_EQ(rn_step(s, 1, 0, 0, nullptr), RN_OK);
  REQUIRE_EQ(rn_session_save(s), RN_OK); rn_session_close(s);
  o.core=RN_CORE_REPLAYNESCORE;
  REQUIRE_EQ(rn_session_open(project.c_str(), nullptr, &o, &s), RN_OK);
  CHECK_EQ(int(rn_session_core_kind(s)), int(RN_CORE_MOCK));
  CHECK_EQ(int(rn_get_mode(s)), int(RN_MODE_RECORD)); rn_session_close(s);
}
TEST_CASE("core selection: experimental core has a genuinely nonrecording live session") {
  SessionOptions opt; opt.core=CoreKind::ReplayNEScore;
  std::unique_ptr<Session> s;
  Status st=Session::create(idleRom(), "idle", opt, s);
  if (!coreAvailable(opt.core)) {
    CHECK_EQ(int(st.code), int(Err::UnsupportedFormat)); CHECK(!s); return;
  }
  REQUIRE(st.ok()); CHECK_EQ(int(s->mode()), int(Mode::Live));
  REQUIRE(s->step(0x81, 0x40, 0).ok());
  CHECK_EQ(s->frame(), 1u); CHECK_EQ(s->takeLength(), 0u);
  CHECK(s->checkpoints().all().empty()); CHECK_FALSE(s->unsavedChanges());
  size_t n=99; CHECK(s->audio(&n) != nullptr); CHECK_EQ(n, size_t(audioSamplesForFrame(0)));
  auto unsupported=[](Status v) { CHECK_EQ(int(v.code), int(Err::UnsupportedFormat)); };
  unsupported(s->setMode(Mode::Record)); unsupported(s->setMode(Mode::Practice));
  unsupported(s->seek(0)); unsupported(s->rewind(1)); unsupported(s->save());
  unsupported(s->autosave()); unsupported(s->saveAs("/must-not-be-created"));
  unsupported(s->practiceSetA(0)); unsupported(s->practiceSetB(0));
  unsupported(s->practiceSetRange(0,0,1)); unsupported(s->practiceGotoA(0));
  unsupported(s->bookmarkAdd("bad",nullptr)); unsupported(s->truncateTake());
  unsupported(s->reset(false)); unsupported(s->step(0,0,kEvSoftReset));
  CHECK_EQ(s->frame(),1u);
  std::unique_ptr<OfflineRenderer> renderer;
  unsupported(OfflineRenderer::create(*s,0,1,renderer));
  REQUIRE(s->step(0,0,kEvPowerCycle).ok()); CHECK_EQ(s->frame(),2u);
  CHECK_EQ(s->takeLength(),0u); CHECK_FALSE(s->unsavedChanges());
}
TEST_CASE("core selection: C API live mode and project prohibition") {
  if (!rn_core_available(RN_CORE_REPLAYNESCORE)) return;
  auto dir=tempDir("core-selection-live-capi");
  auto rom=idleRom(); const auto path=dir+"/idle.nes";
  REQUIRE(fs::writeFileAtomic(path,rom.data(),rom.size()).ok());
  rn_session_options o; rn_session_options_init(&o); o.core=RN_CORE_REPLAYNESCORE;
  rn_session* s=nullptr;
  const auto project=dir+"/forbidden.nesrec";
  CHECK_EQ(rn_session_new(path.c_str(),project.c_str(),&o,&s),RN_ERR_UNSUPPORTED_FORMAT);
  CHECK(s==nullptr); CHECK_FALSE(fs::exists(project));
  REQUIRE_EQ(rn_session_new(path.c_str(),nullptr,&o,&s),RN_OK);
  CHECK_EQ(int(rn_session_core_kind(s)),int(RN_CORE_REPLAYNESCORE));
  CHECK_EQ(int(rn_get_mode(s)),int(RN_MODE_LIVE));
  rn_step_info info{};
  REQUIRE_EQ(rn_step(s,0x80,0,0,&info),RN_OK);
  CHECK_EQ(int(info.mode),int(RN_MODE_LIVE)); CHECK_EQ(info.frame,1u);
  CHECK_EQ(rn_take_count(s),size_t(0)); CHECK_EQ(rn_active_take(s),0u);
  CHECK_EQ(rn_session_has_unsaved_changes(s),0);
  CHECK_EQ(rn_set_mode(s,RN_MODE_RECORD),RN_ERR_UNSUPPORTED_FORMAT);
  CHECK_EQ(rn_set_mode(s,RN_MODE_LIVE),RN_OK);
  size_t n=99; CHECK(rn_audio(s,&n)!=nullptr); CHECK_EQ(n,size_t(rn_audio_samples_for_frame(0)));
  rn_session_close(s);
}

#if !RN_PRODUCT_NES
TEST_CASE("core selection: disabled NES core is unavailable with an explicit error") {
  CHECK(createCore(CoreKind::Nestopia) == nullptr);
  CHECK(coreCompatId(CoreKind::Nestopia).empty());
  SessionOptions options;
  options.core = CoreKind::Nestopia;
  std::unique_ptr<Session> session;
  auto status = Session::create(idleRom(), "idle", options, session);
  CHECK_EQ(int(status.code), int(Err::UnsupportedFormat));
  CHECK(status.message.find("core not available in this product") != std::string::npos);
  CHECK(!session);
  auto dir = tempDir("disabled-nes");
  auto path = writeTestRom(dir);
  rn_session_options opt;
  rn_session_options_init(&opt);
  opt.core = RN_CORE_NESTOPIA;
  rn_session* cs = nullptr;
  CHECK_EQ(rn_session_new(path.c_str(), nullptr, &opt, &cs), RN_ERR_UNSUPPORTED_FORMAT);
  CHECK(cs == nullptr);
  CHECK(std::string(rn_last_error()).find("core not available in this product") != std::string::npos);
}
#endif

TEST_CASE("core selection: C and C++ defaults follow the product profile") {
  // An external core's display name depends on registration, not the product's selector.
  const std::string configured = RN_PRODUCT_DEFAULT_CORE;
  const CoreKind expected = configured == "nestopia" ? CoreKind::Nestopia
                          : configured == "mock" ? CoreKind::Mock
                          : configured == "replaynescore" ? CoreKind::ReplayNEScore
                          : configured == "external" ? CoreKind::External
                          : static_cast<CoreKind>(-1);
  CHECK(defaultCoreKind() == expected);
  CHECK(SessionOptions{}.core == defaultCoreKind());
  CHECK(HarnessConfig{}.core == defaultCoreKind());
  rn_session_options options;
  rn_session_options_init(&options);
  CHECK_EQ(int(options.core), int(defaultCoreKind()));
#if !RN_PRODUCT_NES
  CHECK_EQ(std::string(rn_core_compat_id()), std::string());
  CHECK_EQ(std::string(rn_core_build_id()), std::string());
#endif
}

#if !RN_PRODUCT_NES
TEST_CASE("harness: unavailable NES core reports a clear error before running") {
  Trace trace;
  const auto rom = idleRom();
  const std::vector<InputRecord> script(2);
  auto check = [](const Status& status) {
    CHECK_EQ(int(status.code), int(Err::UnsupportedFormat));
    CHECK(status.message.find("core not available in this product") != std::string::npos);
  };
  check(DeterminismHarness::runFromStart(CoreKind::Nestopia, rom, script, 1, trace));
  check(DeterminismHarness::runFromState(CoreKind::Nestopia, rom, script, {}, 1, trace));
}
#endif
