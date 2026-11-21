// FinalBurn Neo behind ReplayNES's engine, for every target game: record with scripted input
// through the input pipeline (every button, turbo, coins, the stick), seek and replay, export on
// another thread while the session replays at the same time (two virtual machines sharing FBNeo),
// save and reopen. Every frame's picture and sound must match the recording.
//
// ROMs: $REPLAYARCADE_ROMS (a folder built by poc/make_romdir.py); games: $REPLAYARCADE_GAMES
// (space separated) or the 12 target games. A game whose zip is missing is reported and skipped.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/FbneoCore.h"
#include "replaynes/product.h"
#include "replaynes/replaynes.h"
#include "util/Hash.h"
#include "util/Json.h"

namespace {

int failures = 0;
#define EXPECT(cond, what)                                                           \
  do {                                                                               \
    if (!(cond)) {                                                                   \
      ++failures;                                                                    \
      std::printf("    FAILED: %s (%s:%d)\n", std::string(what).c_str(), __FILE__, __LINE__); \
    }                                                                                \
  } while (0)

struct Frame {
  uint64_t video, audio;
  bool operator==(const Frame& o) const { return video == o.video && audio == o.audio; }
};

uint64_t pictureHash(const rn_video_frame_info& v) {
  rn::Hasher64 h;
  h.u64(v.width);
  h.u64(v.height);
  for (uint32_t y = 0; y < v.height; ++y) h.update(v.pixels + size_t(y) * v.stride, size_t(v.width) * 4);
  return h.digest();
}

Frame sessionFrame(const rn_session* s) {
  rn_video_frame_info v{};
  rn_session_video_frame(s, &v);
  size_t n = 0;
  const int16_t* a = rn_audio(s, &n);
  return {pictureHash(v), rn::Hasher64::of(a, n * 2 * sizeof(int16_t), n)};
}

// Replays [from, to) from a seek and returns the frames.
std::vector<Frame> replayRange(rn_session* s, uint64_t from, uint64_t to) {
  std::vector<Frame> out;
  rn_set_mode(s, RN_MODE_REPLAY);
  if (rn_seek(s, from) != RN_OK) return out;
  for (uint64_t f = from; f < to; ++f) {
    rn_step_info info{};
    if (rn_step(s, 0, 0, 0, &info) != RN_OK || info.frame != f + 1) break;
    out.push_back(sessionFrame(s));
  }
  return out;
}

// Records `frames` frames: coin and start late enough for every game's boot (Gradius warms up for
// about 20 s), then `press` for 20 of every 60 frames from `pressFrom` on (After Burner II's
// take-off is not controllable); returns each frame's picture.
std::vector<uint64_t> playWith(const std::string& zip, const std::string& layoutJson, const std::string& press,
                               uint64_t pressFrom, uint64_t frames) {
  std::vector<uint64_t> out;
  rn_session_options opt;
  rn_session_options_init(&opt);
  opt.core = RN_CORE_EXTERNAL;
  rn_session* s = nullptr;
  if (rn_session_new(zip.c_str(), nullptr, &opt, &s) != RN_OK) return out;
  rn_input* in = rn_input_new();
  rn_input_set_layout_json(in, layoutJson.c_str());
  rn_input_bind(in, "t:coin", "p1.coin");
  rn_input_bind(in, "t:start", "p1.start");
  if (!press.empty()) rn_input_bind(in, "t:press", press.c_str());
  for (uint64_t f = 0; f < frames; ++f) {
    rn_input_set_pressed(in, "t:coin", (f >= 1500 && f < 1510) || (f >= 1560 && f < 1570));
    rn_input_set_pressed(in, "t:start", (f >= 1700 && f < 1710) || (f >= 1760 && f < 1770));
    if (!press.empty()) rn_input_set_pressed(in, "t:press", f >= pressFrom && (f - pressFrom) % 60 < 20);
    rn_input_record rec;
    rn_input_sample_record(in, f, &rec);
    rn_step_info info{};
    if (rn_step_record(s, &rec, &info) != RN_OK) break;
    rn_video_frame_info v{};
    rn_session_video_frame(s, &v);
    out.push_back(pictureHash(v));
  }
  rn_input_free(in);
  rn_session_close(s);
  return out;
}

// Every player-1 button and direction must reach the game: the picture after pressing it differs
// from the picture without it. A button whose effect depends on the game's state (Gradius's
// power-up needs a capsule) is reported, not failed; the fire buttons as a whole must act.
void testInputsAct(const std::string& romDir, const std::string& game) {
  std::string zip = romDir + "/" + game + ".zip";
  if (!std::filesystem::exists(zip)) return;
  rn_session_options opt;
  rn_session_options_init(&opt);
  opt.core = RN_CORE_EXTERNAL;
  rn_session* s = nullptr;
  if (rn_session_new(zip.c_str(), nullptr, &opt, &s) != RN_OK) return;
  char* lj = rn_session_input_layout_json(s);
  std::string layoutJson = lj;
  rn_string_free(lj);
  rn_session_close(s);
  rn::Json layout;
  rn::Json::parse(layoutJson, layout);
  const uint64_t kFrom = 2000, kFrames = 3600;
  std::vector<uint64_t> base = playWith(zip, layoutJson, "", 0, kFrames);
  std::string acted, inert;
  bool fireActs = false;
  std::string turboChecked, turboSame;
  bool turboActs = false;
  for (auto& b : layout["buttons"].items()) {
    std::string id = b["id"].asString();
    if (id.rfind("p1.", 0) != 0 || id == "p1.coin" || id == "p1.start" || id == "p1.service") continue;
    std::vector<uint64_t> got = playWith(zip, layoutJson, id, kFrom, kFrames);
    bool differs = got.size() == base.size() && !std::equal(got.begin() + long(kFrom), got.end(), base.begin() + long(kFrom));
    bool same = got.size() == base.size() && std::equal(got.begin(), got.begin() + long(kFrom), base.begin());
    EXPECT(same, id + ": frames before the press match");
    (differs ? acted : inert) += " " + id;
    if (differs && id.find(".b") != std::string::npos) {
      fireActs = true;
      // Turbo: holding a button's turbo action presses it again and again, which the game sees
      // (more shots), so the pictures differ from holding the button itself. A button the game
      // already repeats while held (Gradius II's shot) looks the same; the next one is tried.
      if (!turboActs) {
        std::string turboId = b.has("turbo") ? b["turbo"].asString() : id + ".turbo";
        std::vector<uint64_t> turbo = playWith(zip, layoutJson, turboId, kFrom, kFrames);
        turboActs = turbo.size() == got.size() && !std::equal(turbo.begin(), turbo.end(), got.begin());
        (turboActs ? turboChecked : turboSame) += " " + turboId;
      }
    }
  }
  EXPECT(turboActs, game + ": the turbo of a fire button differs from holding it");
  std::printf("    acts:%s\n    no effect:%s\n    turbo acts:%s; same as holding:%s\n", acted.c_str(),
              inert.empty() ? " (none)" : inert.c_str(), turboChecked.c_str(), turboSame.empty() ? " (none)" : turboSame.c_str());
  EXPECT(fireActs, game + ": a fire button changes the game");
}

void testGame(const std::string& romDir, const std::string& game, const std::string& work) {
  std::string zip = romDir + "/" + game + ".zip";
  std::printf("== %s\n", game.c_str());
  if (!std::filesystem::exists(zip)) {
    std::printf("    skipped: %s not found\n", zip.c_str());
    return;
  }
  auto t0 = std::chrono::steady_clock::now();
  rn_session_options opt;
  rn_session_options_init(&opt);
  opt.core = RN_CORE_EXTERNAL;
  rn_session* s = nullptr;
  rn_status st = rn_session_new(zip.c_str(), nullptr, &opt, &s);
  EXPECT(st == RN_OK, "session for " + game + ": " + rn_last_error());
  if (st != RN_OK) return;

  EXPECT(rac::contentIdForPath(zip) == rn_session_rom_sha256(s),
         "content id from the file name (" + rac::contentIdForPath(zip) + ") equals the loaded one (" +
             rn_session_rom_sha256(s) + ")");
  rn_media_info m{};
  rn_session_media_info(s, &m);
  char* layoutJson = rn_session_input_layout_json(s);
  rn::Json layout;
  rn::Json::parse(layoutJson, layout);
  std::printf("    media: max %ux%u, %llu/%llu fps, %u ch, pixel aspect %u:%u, rotation %u; samples/frame %llu\n",
              m.max_width, m.max_height, (unsigned long long)m.fps_num, (unsigned long long)m.fps_den, m.audio_channels,
              m.pixel_aspect_num, m.pixel_aspect_den, m.rotation,
              (unsigned long long)rn_session_samples_before(s, 1));
  std::string names;
  for (auto& b : layout["buttons"].items())
    names += " " + b["id"].asString() + (b["label"].asString().empty() ? "" : "(" + b["label"].asString() + ")");
  for (auto& a : layout["axes"].items()) names += " " + a["id"].asString() + "(" + a["label"].asString() + ")";
  std::printf("    input:%s\n", names.c_str());
  // Every target game has fire buttons, a start and a coin, also when it is not the first game
  // this process loads (FBNeo maps a game's inputs on its first frame).
  bool hasFire = false, hasStart = false, hasCoin = false;
  for (auto& b : layout["buttons"].items()) {
    std::string id = b["id"].asString();
    hasFire = hasFire || id == "p1.b1";
    hasStart = hasStart || id.find(".start") != std::string::npos;
    hasCoin = hasCoin || id.find(".coin") != std::string::npos;
  }
  EXPECT(hasFire && hasStart && hasCoin, "layout has p1.b1, a start and a coin");

  // Every button gets a virtual key "t:<id>", its turbo "t:<id>.turbo"; axes follow a stick.
  rn_input* in = rn_input_new();
  EXPECT(rn_input_set_layout_json(in, layoutJson) == RN_OK, "layout into the input pipeline");
  rn_string_free(layoutJson);
  std::vector<std::string> buttons, coins, starts;
  for (auto& b : layout["buttons"].items()) {
    std::string id = b["id"].asString();
    rn_input_bind(in, ("t:" + id).c_str(), id.c_str());
    std::string turbo = b.has("turbo") ? b["turbo"].asString() : id + ".turbo";
    rn_input_bind(in, ("t:" + turbo).c_str(), turbo.c_str());
    if (id.find(".coin") != std::string::npos) coins.push_back(id);
    else if (id.find(".start") != std::string::npos) starts.push_back(id);
    else if (id.find(".service") == std::string::npos) buttons.push_back(id);
  }
  for (auto& a : layout["axes"].items()) {
    std::string id = a["id"].asString();
    rn_input_bind(in, (id.find(".axis.r") != std::string::npos ? "t:rstick.-y" : id.find(".axis.y") != std::string::npos ? "t:lstick.-y" : "t:lstick.x"), id.c_str());
  }
  rn_input_set_turbo(in, 4, 2);

  const uint64_t kFrames = 3000;
  std::vector<Frame> recorded;
  std::set<uint32_t> colours;
  for (uint64_t f = 0; f < kFrames; ++f) {
    uint64_t c = f % 1200;
    for (auto& id : coins) rn_input_set_pressed(in, ("t:" + id).c_str(), (c >= 60 && c < 66) || (c >= 90 && c < 96));
    for (auto& id : starts) rn_input_set_pressed(in, ("t:" + id).c_str(), (c >= 120 && c < 126) || (c >= 150 && c < 156));
    uint64_t x = (f / 8 + 1) * 0x9E3779B97F4A7C15ull;
    x ^= x >> 31;
    for (size_t i = 0; i < buttons.size(); ++i)
      rn_input_set_pressed(in, ("t:" + buttons[i]).c_str(), f >= 200 && ((x >> (i * 3)) & 3) == 1);
    if (!buttons.empty()) rn_input_set_pressed(in, ("t:" + buttons.back() + ".turbo").c_str(), f >= 200 && (f / 300) % 2);
    rn_input_set_axis(in, "t:lstick", float(int(f % 41) - 20) / 20.f, float(int(f % 29) - 14) / 14.f);
    rn_input_set_axis(in, "t:rstick", 0.f, f % 600 < 300 ? -1.f : 0.3f);
    rn_input_record rec;
    rn_input_sample_record(in, f, &rec);
    rn_step_info info{};
    st = rn_step_record(s, &rec, &info);
    if (st != RN_OK) {
      EXPECT(false, "record frame " + std::to_string(f) + ": " + rn_last_error());
      break;
    }
    recorded.push_back(sessionFrame(s));
    if (f == 2000 || f == 2999) {
      rn_video_frame_info v{};
      rn_session_video_frame(s, &v);
      for (uint32_t y = 0; y < v.height; y += 4)
        for (uint32_t xx = 0; xx < v.width; xx += 4) colours.insert(v.pixels[size_t(y) * v.stride + xx]);
    }
  }
  rn_input_free(in);
  EXPECT(recorded.size() == kFrames, "recorded all frames");
  // One value for the whole recording (pictures and sound of every frame), to compare builds and
  // platforms: the same input must give the same value everywhere the recordings are meant to replay.
  rn::Hasher64 digest;
  for (const Frame& fr : recorded) {
    digest.u64(fr.video);
    digest.u64(fr.audio);
  }
  std::printf("    recording digest: %016llx\n", (unsigned long long)digest.digest());
  // $REPLAYARCADE_FRAME_DUMP=<dir>: every frame's picture and sound hash, one line each, to find
  // where two builds part.
  if (const char* dump = std::getenv("REPLAYARCADE_FRAME_DUMP")) {
    if (FILE* f = std::fopen((std::string(dump) + "/" + game + ".txt").c_str(), "w")) {
      for (size_t i = 0; i < recorded.size(); ++i)
        std::fprintf(f, "%zu %016llx %016llx\n", i, (unsigned long long)recorded[i].video,
                     (unsigned long long)recorded[i].audio);
      std::fclose(f);
    }
  }
  // The game runs and draws: more than one colour, and pictures that change (Space Invaders and Lode
  // Runner draw with three colours; a count of colours alone failed them).
  std::set<uint64_t> pictures;
  for (const Frame& fr : recorded) pictures.insert(fr.video);
  EXPECT(colours.size() > 1 && pictures.size() >= 10,  // Double Dragon's attract mode: 31 in 3000 frames
         "pictures have content (" + std::to_string(colours.size()) + " colours, " + std::to_string(pictures.size()) +
             " different pictures)");

  // Seek to checkpoints and arbitrary frames in a scrambled order and replay 120 frames from each.
  const uint64_t starts_[] = {2500, 0, 1777, 300, 2999 - 120, 1200, 31, 2400};
  for (uint64_t from : starts_) {
    std::vector<Frame> got = replayRange(s, from, from + 120);
    bool same = got.size() == 120 && std::equal(got.begin(), got.end(), recorded.begin() + long(from));
    EXPECT(same, "seek to " + std::to_string(from) + " replays the recording");
  }

  // Hold Rewind as the app does (2, 3, then 4 frames per tick) from the end: every picture on the
  // way back is the recorded one (the engine's rewind cache must not change anything).
  {
    rn_set_mode(s, RN_MODE_REPLAY);
    bool ok = rn_seek(s, kFrames) == RN_OK;
    int tick = 0, compared = 0, wrong = 0;
    while (ok && rn_frame(s) > 1000) {
      ++tick;
      uint64_t n = tick <= 30 ? 2 : tick <= 90 ? 3 : 4;
      if (rn_rewind(s, n) != RN_OK) { ok = false; break; }
      uint64_t f = rn_frame(s);
      rn_video_frame_info v{};
      rn_session_video_frame(s, &v);
      ++compared;
      if (f == 0 || pictureHash(v) != recorded[size_t(f - 1)].video) ++wrong;
    }
    EXPECT(ok && compared > 200 && wrong == 0,
           "rewind pictures equal the recording (" + std::to_string(wrong) + " of " + std::to_string(compared) + " differ)");
  }

  // Export on another thread while the session replays: two virtual machines take turns in FBNeo.
  rn_renderer* r = nullptr;
  EXPECT(rn_renderer_new(s, 1000, 2600, &r) == RN_OK, "renderer");
  std::vector<Frame> exported;
  std::thread exporter([&] {
    for (;;) {
      const uint32_t* video = nullptr;
      const int16_t* audio = nullptr;
      size_t n = 0;
      uint64_t index = 0;
      if (rn_renderer_next(r, &video, &audio, &n, &index) != RN_OK) break;
      rn_video_frame_info v{};
      rn_renderer_video_frame(r, &v);
      exported.push_back({pictureHash(v), rn::Hasher64::of(audio, n * 2 * sizeof(int16_t), n)});
    }
  });
  std::vector<Frame> concurrent = replayRange(s, 0, 1500);
  exporter.join();
  rn_renderer_free(r);
  EXPECT(concurrent.size() == 1500 && std::equal(concurrent.begin(), concurrent.end(), recorded.begin()),
         "session replay while exporting");
  EXPECT(exported.size() == 1600 && std::equal(exported.begin(), exported.end(), recorded.begin() + 1000),
         "export while the session replays (" + std::to_string(exported.size()) + " frames)");

  // Save, reopen, replay everything.
  std::string project = work + "/" + game + RN_PRODUCT_PROJECT_EXT;
  std::filesystem::remove_all(project);
  EXPECT(rn_session_save_as(s, project.c_str()) == RN_OK, std::string("save: ") + rn_last_error());
  rn_session_close(s);
  s = nullptr;
  st = rn_session_open(project.c_str(), zip.c_str(), &opt, &s);
  EXPECT(st == RN_OK, std::string("reopen: ") + rn_last_error());
  if (st == RN_OK) {
    std::vector<Frame> again = replayRange(s, 0, kFrames);
    EXPECT(again.size() == kFrames && std::equal(again.begin(), again.end(), recorded.begin()), "reopened replay");
    rn_session_close(s);
  }
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("    done in %.1f s\n", secs);
}

}  // namespace

// A driver that draws 16-bit colour only (BDF_16BIT_ONLY: Cave's) asks for RGB565: the host must accept it.
// Until 2026-11-22 it accepted XRGB8888 alone; such a driver had a 0-byte frame buffer and DonPachi wrote
// its first frame's clear (320 x 240 x 2 bytes of 0xFF) over the heap: the app crashed making library
// pictures (Android, Legion Tab). Its picture must now be there and not blank.
void testSixteenBitDriver(const std::string& romDir) {
  for (const char* game : {"donpachij", "donpachi", "ddonpach"}) {
    std::string zip = romDir + "/" + game + ".zip";
    if (!std::filesystem::exists(zip)) continue;
    rn_session_options opt;
    rn_session_options_init(&opt);
    opt.core = RN_CORE_EXTERNAL;
    rn_session* s = nullptr;
    EXPECT(rn_session_new(zip.c_str(), nullptr, &opt, &s) == RN_OK && s, std::string(game) + ": opens");
    if (!s) continue;
    for (int f = 0; f < 5400; ++f) rn_step(s, 0, 0, 0, nullptr);
    rn_video_frame_info v{};
    EXPECT(rn_session_video_frame(s, &v) == RN_OK && v.pixels && v.width && v.height, std::string(game) + ": a picture");
    uint64_t lit = 0;
    for (uint32_t y = 0; v.pixels && y < v.height; ++y)
      for (uint32_t x = 0; x < v.width; ++x) lit += (v.pixels[size_t(y) * v.stride + x] & 0xFFFFFFu) != 0;
    EXPECT(lit > uint64_t(v.width) * v.height / 10, std::string(game) + ": the picture at frame 5400 is not blank (" + std::to_string(lit) + " lit pixels)");
    std::printf("%s: 16-bit driver, %llu lit pixels of %ux%u\n", game, (unsigned long long)lit, v.width, v.height);
    rn_session_close(s);
    return;
  }
  std::printf("no DonPachi set in REPLAYARCADE_ROMS: the 16-bit driver check did not run\n");
}

int main() {
  rac::registerFbneoCore();
  // Arcade games only: FinalBurn Neo's home systems get no content id (the app does not open them).
  // Set names alone, no ROM files needed.
  for (const char* home : {"md_sonic", "nes_smb", "pce_deepblue", "sms_alexkidd", "ngp_samegame", "astro_pacman", "cv_dkong"})
  {
    EXPECT(rac::contentIdForPath(std::string("/roms/") + home + ".zip").empty(), std::string(home) + ": a home system");
    EXPECT(!rac::isArcadeFile(std::string("/roms/") + home + ".zip"), std::string(home) + ": listed");
  }
  for (const char* arcade : {"sonic", "gradius", "1942"})
  {
    EXPECT(!rac::contentIdForPath(std::string("/roms/") + arcade + ".zip").empty(), std::string(arcade) + ": an arcade game");
    EXPECT(rac::isArcadeFile(std::string("/roms/") + arcade + ".zip"), std::string(arcade) + ": not listed");
  }
  EXPECT(rac::isArcadeFile("/roms/notaset.zip"), "an unknown name stays listed (opening it says why it fails)");
  const char* roms = std::getenv("REPLAYARCADE_ROMS");
  if (!roms || !*roms) {
    std::printf("REPLAYARCADE_ROMS is not set: only the home-system checks ran (%d failure(s))\n", failures);
    return failures ? 1 : 77;
  }
  std::string list = std::getenv("REPLAYARCADE_GAMES") ? std::getenv("REPLAYARCADE_GAMES")
                                                       : "aburner2 gforce2ja sharrier splatter ordyneje dspirit genpeitd "
                                                         "daimakai gradius gradius2 salamand aliens";
  std::string work = std::filesystem::temp_directory_path().string() + "/replayarcade-test-fbneo";
  std::filesystem::create_directories(work);
  testSixteenBitDriver(roms);
  std::istringstream games(list);
  for (std::string g; games >> g;) {
    testGame(roms, g, work);
    if (!std::getenv("REPLAYARCADE_SKIP_INPUT_CHECKS")) testInputsAct(roms, g);
  }
  std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
