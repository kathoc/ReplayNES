// JNI shim of the Android app (io.github.replaynes.NativeBridge).
//
// The native side owns the frame loop: one emulation thread owns the rn_session (the engine's
// handles are not thread-safe), paces rn_step to the NES frame rate (39375000/655171 Hz), blits
// every frame to the SurfaceView's ANativeWindow (BGRA -> RGBA, nearest-neighbour integer scale;
// the compositor scales that buffer into the letterboxed view) and feeds an AAudio stream
// (48 kHz mono s16) through a lock-free ring buffer. Kotlin only forwards input bits, the surface,
// pause/resume and start/stop.
#include <jni.h>

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "replaynes/replaynes.h"

#define LOG_TAG "ReplayNES"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr int kW = RN_VIDEO_WIDTH;
constexpr int kH = RN_VIDEO_HEIGHT;

// ------------------------------------------------------------------------------------ audio
// Single producer (emulation thread) / single consumer (AAudio callback) ring of int16 samples.
class AudioRing {
 public:
  static constexpr size_t kCap = 1u << 14;  // 16384 samples (~340 ms)
  size_t size() const { return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire); }
  void push(const int16_t* p, size_t n) {
    size_t w = w_.load(std::memory_order_relaxed);
    size_t r = r_.load(std::memory_order_acquire);
    n = std::min(n, kCap - (w - r));
    for (size_t i = 0; i < n; ++i) buf_[(w + i) & (kCap - 1)] = p[i];
    w_.store(w + n, std::memory_order_release);
  }
  size_t pop(int16_t* p, size_t n) {
    size_t r = r_.load(std::memory_order_relaxed);
    size_t w = w_.load(std::memory_order_acquire);
    n = std::min(n, w - r);
    for (size_t i = 0; i < n; ++i) p[i] = buf_[(r + i) & (kCap - 1)];
    r_.store(r + n, std::memory_order_release);
    return n;
  }
  void drain() { r_.store(w_.load(std::memory_order_acquire), std::memory_order_release); }

 private:
  int16_t buf_[kCap] = {};
  std::atomic<size_t> w_{0}, r_{0};
};

class Audio {
 public:
  // Start playing once this much is buffered (also after an underrun): ~40 ms.
  static constexpr size_t kPrime = 1920;
  // Never let latency grow beyond ~120 ms (device clock slightly slower than the emulation).
  static constexpr size_t kMaxFill = 5760;

  ~Audio() { close(); }

  void open() {
    close();
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 1);
    AAudioStreamBuilder_setSampleRate(b, RN_SAMPLE_RATE);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setDataCallback(b, &Audio::dataCb, this);
    AAudioStreamBuilder_setErrorCallback(b, &Audio::errorCb, this);
    aaudio_result_t rc = AAudioStreamBuilder_openStream(b, &stream_);
    AAudioStreamBuilder_delete(b);
    if (rc != AAUDIO_OK) {
      LOGW("AAudio open failed: %s (continuing without sound)", AAudio_convertResultToText(rc));
      stream_ = nullptr;
      return;
    }
    LOGI("AAudio: %d Hz, burst %d frames", AAudioStream_getSampleRate(stream_),
         AAudioStream_getFramesPerBurst(stream_));
    if (AAudioStream_getSampleRate(stream_) != RN_SAMPLE_RATE) LOGW("AAudio rate differs from 48000 Hz");
    restart_.store(false);
  }

  void close() {
    if (!stream_) return;
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
  }

  void start() {
    ring_.drain();
    primed_.store(false);
    if (stream_) AAudioStream_requestStart(stream_);
  }
  void pause() {
    if (stream_) AAudioStream_requestPause(stream_);
    ring_.drain();
  }

  void push(const int16_t* p, size_t n) {
    if (n == 0 || ring_.size() + n > kMaxFill) return;  // drop rather than grow latency
    ring_.push(p, n);
  }

  // The stream was disconnected (route change, headphones): reopen from the emulation thread.
  void serviceRestart() {
    if (!restart_.exchange(false)) return;
    LOGI("AAudio: reopening the stream after a disconnect");
    open();
    start();
  }

 private:
  static aaudio_data_callback_result_t dataCb(AAudioStream*, void* user, void* data, int32_t frames) {
    auto* self = static_cast<Audio*>(user);
    auto* out = static_cast<int16_t*>(data);
    size_t n = static_cast<size_t>(frames), got = 0;
    if (self->primed_.load() || self->ring_.size() >= kPrime) {
      self->primed_.store(true);
      got = self->ring_.pop(out, n);
      if (got < n) self->primed_.store(false);  // underrun: re-prime before playing again
    }
    // Underrun: fade the last sample to silence instead of a hard step.
    int16_t last = got ? out[got - 1] : self->last_;
    for (size_t i = got; i < n; ++i) {
      last = static_cast<int16_t>(last * 7 / 8);
      out[i] = last;
    }
    self->last_ = n ? out[n - 1] : last;
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }
  static void errorCb(AAudioStream*, void* user, aaudio_result_t err) {
    LOGW("AAudio error: %s", AAudio_convertResultToText(err));
    if (err == AAUDIO_ERROR_DISCONNECTED) static_cast<Audio*>(user)->restart_.store(true);
  }

  AAudioStream* stream_ = nullptr;
  AudioRing ring_;
  std::atomic<bool> primed_{false};
  std::atomic<bool> restart_{false};
  int16_t last_ = 0;  // consumer only
};

// ------------------------------------------------------------------------------------ video
// Presentation runs on its own thread: ANativeWindow_lock / unlockAndPost may block for one or
// more display refreshes (buffer queue back-pressure), which must never slow the emulation (and
// with it the audio). The emulation thread only drops its newest frame into a mailbox; the render
// thread blits the latest one (older ones are skipped if presenting is slow).
// The window is independent of the session (the surface can come and go while paused or before
// a game starts); the last frame is kept so a re-created surface shows the picture right away.
class Display {
 public:
  std::atomic<uint64_t> presented{0};  // frames posted to the window (diagnostics)

  // Emulation thread: hand over a finished frame (copied).
  void present(const uint32_t* video) {
    ensureThread();
    {
      std::lock_guard<std::mutex> g(m_);
      std::memcpy(pending_.data(), video, sizeof(uint32_t) * kW * kH);
      dirty_ = true;
    }
    cv_.notify_one();
  }

  // UI thread (SurfaceHolder callbacks). Returns after the old window is no longer used.
  void setWindow(ANativeWindow* w, int viewH) {
    ensureThread();
    std::lock_guard<std::mutex> wg(winMutex_);
    if (win_) ANativeWindow_release(win_);
    win_ = w;
    if (!win_) return;
    // Integer pre-scale close to the on-screen size so the compositor's filtering stays sharp.
    scale_ = std::clamp((viewH + kH - 1) / kH, 1, 4);
    ANativeWindow_setBuffersGeometry(win_, kW * scale_, kH * scale_, WINDOW_FORMAT_RGBA_8888);
    {
      std::lock_guard<std::mutex> g(m_);
      dirty_ = true;  // redraw the kept frame (black before the first one)
    }
    cv_.notify_one();
  }

  void clear() {
    std::lock_guard<std::mutex> g(m_);
    std::fill(pending_.begin(), pending_.end(), 0xFF000000u);
    dirty_ = true;
  }

 private:
  void ensureThread() {
    std::call_once(once_, [this] { std::thread([this] { run(); }).detach(); });  // process lifetime
  }

  void run() {
    pthread_setname_np(pthread_self(), "rn-present");
    std::vector<uint32_t> front(kW * kH, 0xFF000000u);
    for (;;) {
      {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return dirty_; });
        dirty_ = false;
        front = pending_;
      }
      std::lock_guard<std::mutex> wg(winMutex_);
      blit(front.data());
    }
  }

  void blit(const uint32_t* src) {  // winMutex_ held
    if (!win_) return;
    ANativeWindow_Buffer b;
    if (ANativeWindow_lock(win_, &b, nullptr) != 0) return;
    const int k = scale_;
    const int w = std::min(b.width, kW * k);
    const int h = std::min(b.height, kH * k);
    auto* dst = static_cast<uint32_t*>(b.bits);
    row_.resize(static_cast<size_t>(kW * k));
    for (int y = 0; y < kH && y * k < h; ++y) {
      const uint32_t* s = src + y * kW;
      uint32_t* r = row_.data();
      for (int x = 0; x < kW; ++x) {
        // Engine: B,G,R,A bytes (0xAARRGGBB as a little-endian word). Window: R,G,B,A bytes.
        uint32_t p = s[x];
        p = (p & 0xFF00FF00u) | ((p >> 16) & 0xFFu) | ((p & 0xFFu) << 16);
        for (int i = 0; i < k; ++i) *r++ = p;
      }
      for (int i = 0; i < k && y * k + i < h; ++i)
        std::memcpy(dst + static_cast<size_t>(y * k + i) * b.stride, row_.data(), static_cast<size_t>(w) * 4);
    }
    ANativeWindow_unlockAndPost(win_);
    presented.fetch_add(1, std::memory_order_relaxed);
  }

  std::once_flag once_;
  std::mutex m_;  // pending_, dirty_
  std::condition_variable cv_;
  std::vector<uint32_t> pending_ = std::vector<uint32_t>(kW * kH, 0xFF000000u);
  bool dirty_ = false;
  std::mutex winMutex_;  // win_, scale_, row_ (held while blitting)
  ANativeWindow* win_ = nullptr;
  int scale_ = 3;
  std::vector<uint32_t> row_;
};

Display g_display;

// -------------------------------------------------------------------------------- emulation
class Emulator {
 public:
  explicit Emulator(rn_session* s) : s_(s) {}

  void start() {
    audio_.open();
    thread_ = std::thread([this] { run(); });
  }

  // Joins the thread, then saves and closes the session on the calling thread (the thread no
  // longer touches it, so ownership has passed back).
  void stop() {
    {
      std::lock_guard<std::mutex> g(m_);
      quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    audio_.close();
    if (rn_session_save(s_) != RN_OK) {
      LOGE("save failed: %s; trying the journal", rn_last_error());
      rn_session_autosave(s_);
    }
    LOGI("session closed at frame %llu", static_cast<unsigned long long>(rn_frame(s_)));
    rn_session_close(s_);
    s_ = nullptr;
  }

  void setPaused(bool p) {
    {
      std::lock_guard<std::mutex> g(m_);
      paused_ = p;
    }
    cv_.notify_all();
  }

  std::atomic<uint32_t> touch{0}, keys{0};
  // Bits pressed since the last frame was sampled: a tap shorter than one frame still reaches the game.
  std::atomic<uint32_t> latched{0};
  std::atomic<bool> rewind{false};
  std::atomic<uint64_t> frames{0};

 private:
  void run() {
    pthread_setname_np(pthread_self(), "rn-emu");
    using clock = std::chrono::steady_clock;
    // NES NTSC frame period: 655171 / 39375000 s (~16.639 ms).
    const auto period = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(static_cast<double>(RN_FPS_DEN) / RN_FPS_NUM));
    auto next = clock::now();
    auto lastSave = next, lastLog = next;
    uint64_t framesAtLog = 0;
    bool audioRunning = false;
    int rewindTicks = 0;
    clock::duration stepTime{};
    uint64_t shownAtLog = 0;

    for (;;) {
      {
        std::unique_lock<std::mutex> lk(m_);
        if (paused_ && !quit_) {
          if (audioRunning) audio_.pause();
          audioRunning = false;
          rn_session_autosave(s_);
          cv_.wait(lk, [this] { return !paused_ || quit_; });
          next = clock::now();
        }
        if (quit_) break;
      }
      if (!audioRunning) {
        audio_.start();
        audioRunning = true;
      }
      audio_.serviceRestart();

      if (rewind.load()) {
        // Hold to rewind on the take: 1 frame per tick, faster after a second.
        ++rewindTicks;
        uint64_t n = rewindTicks < 60 ? 1 : (rewindTicks < 180 ? 2 : 4);
        if (rn_frame(s_) > 0 && rn_rewind(s_, n) == RN_OK) g_display.present(rn_video(s_));
      } else {
        rewindTicks = 0;
        const uint8_t p1 = static_cast<uint8_t>(touch.load() | keys.load() | latched.exchange(0));
        rn_step_info info{};
        const auto t0 = clock::now();
        const rn_status st = rn_step(s_, p1, 0, 0, &info);
        stepTime += clock::now() - t0;
        if (st != RN_OK) {
          LOGE("rn_step: %s", rn_last_error());
        } else {
          g_display.present(rn_video(s_));
          size_t n = 0;
          const int16_t* pcm = rn_audio(s_, &n);
          if (pcm) audio_.push(pcm, n);
          frames.fetch_add(1);
        }
      }

      const auto now = clock::now();
      if (now - lastSave > std::chrono::seconds(10)) {
        if (rn_session_autosave(s_) != RN_OK) LOGW("autosave: %s", rn_last_error());
        lastSave = now;
      }
      if (now - lastLog > std::chrono::seconds(5)) {
        const double secs = std::chrono::duration<double>(now - lastLog).count();
        const uint64_t f = frames.load();
        const double perFrame = f > framesAtLog ? 1000.0 / static_cast<double>(f - framesAtLog) : 0.0;
        const uint64_t shown = g_display.presented.load(std::memory_order_relaxed);
        LOGI("frame %llu (take frame %llu): emulated %.2f fps, displayed %.2f fps, step %.2f ms",
             static_cast<unsigned long long>(f), static_cast<unsigned long long>(rn_frame(s_)),
             (f - framesAtLog) / secs, (shown - shownAtLog) / secs,
             std::chrono::duration<double>(stepTime).count() * perFrame);
        shownAtLog = shown;
        stepTime = clock::duration{};
        framesAtLog = f;
        lastLog = now;
      }

      next += period;
      const auto after = clock::now();
      if (after > next + period * 4) next = after;  // fell far behind (debugger, slow device): no burst
      std::this_thread::sleep_until(next);
    }
    if (audioRunning) audio_.pause();
  }

  rn_session* s_;
  Audio audio_;
  std::thread thread_;
  std::mutex m_;
  std::condition_variable cv_;
  bool quit_ = false;
  bool paused_ = false;
};

std::mutex g_emuMutex;  // guards g_emu (JNI calls arrive on the UI thread, but be safe)
std::unique_ptr<Emulator> g_emu;

std::string jstr(JNIEnv* env, jstring s) {
  if (!s) return {};
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string r(c ? c : "");
  if (c) env->ReleaseStringUTFChars(s, c);
  return r;
}

jstring errorString(JNIEnv* env, rn_status st) {
  std::string msg = std::string(rn_status_name(st)) + ": " + rn_last_error();
  return env->NewStringUTF(msg.c_str());
}

bool projectExists(const std::string& dir) {
  std::error_code ec;
  return std::filesystem::is_directory(dir, ec) && !std::filesystem::is_empty(dir, ec);
}

void stopLocked() {
  if (!g_emu) return;
  g_emu->stop();
  g_emu.reset();
}

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL Java_io_github_replaynes_NativeBridge_engineVersion(JNIEnv* env, jobject) {
  return env->NewStringUTF(rn_version());
}

JNIEXPORT jstring JNICALL Java_io_github_replaynes_NativeBridge_writeTestCartridge(JNIEnv* env, jobject,
                                                                                    jstring path) {
  rn_status st = rn_write_test_cartridge(jstr(env, path).c_str());
  return st == RN_OK ? nullptr : errorString(env, st);
}

// Opens the ROM's project (or creates it) and starts the emulation thread. Returns an error
// message, or null on success.
JNIEXPORT jstring JNICALL Java_io_github_replaynes_NativeBridge_start(JNIEnv* env, jobject, jstring romPath,
                                                                       jstring projectDir) {
  std::lock_guard<std::mutex> g(g_emuMutex);
  stopLocked();
  const std::string rom = jstr(env, romPath), dir = jstr(env, projectDir);
  rn_session_options opt;
  rn_session_options_init(&opt);
  opt.open_flags = RN_OPEN_DROP_CORRUPT_STATES;  // checkpoints are a cache
  rn_session* s = nullptr;
  rn_status st;
  if (projectExists(dir)) {
    st = rn_session_open(dir.c_str(), rom.c_str(), &opt, &s);
    if (st == RN_OK && rn_session_recovered(s)) LOGI("recovered unsaved work from the journal");
  } else {
    st = rn_session_new(rom.c_str(), dir.c_str(), &opt, &s);
  }
  if (st != RN_OK) {
    LOGE("open %s: %s %s", dir.c_str(), rn_status_name(st), rn_last_error());
    return errorString(env, st);
  }
  if (rn_get_mode(s) != RN_MODE_RECORD) rn_set_mode(s, RN_MODE_RECORD);
  LOGI("session %s at frame %llu (take length %llu)", dir.c_str(), static_cast<unsigned long long>(rn_frame(s)),
       static_cast<unsigned long long>(rn_take_length(s)));
  g_display.clear();
  g_emu = std::make_unique<Emulator>(s);
  g_emu->start();
  return nullptr;
}

JNIEXPORT void JNICALL Java_io_github_replaynes_NativeBridge_stop(JNIEnv*, jobject) {
  std::lock_guard<std::mutex> g(g_emuMutex);
  stopLocked();
}

JNIEXPORT jboolean JNICALL Java_io_github_replaynes_NativeBridge_isRunning(JNIEnv*, jobject) {
  std::lock_guard<std::mutex> g(g_emuMutex);
  return g_emu ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_io_github_replaynes_NativeBridge_setPaused(JNIEnv*, jobject, jboolean paused) {
  std::lock_guard<std::mutex> g(g_emuMutex);
  if (g_emu) g_emu->setPaused(paused == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_io_github_replaynes_NativeBridge_setInput(JNIEnv*, jobject, jint touch, jint keys) {
  std::lock_guard<std::mutex> g(g_emuMutex);
  if (!g_emu) return;
  g_emu->touch.store(static_cast<uint32_t>(touch) & 0xFFu);
  g_emu->keys.store(static_cast<uint32_t>(keys) & 0xFFu);
  g_emu->latched.fetch_or(static_cast<uint32_t>(touch | keys) & 0xFFu);
}

JNIEXPORT void JNICALL Java_io_github_replaynes_NativeBridge_setRewind(JNIEnv*, jobject, jboolean on) {
  std::lock_guard<std::mutex> g(g_emuMutex);
  if (g_emu) g_emu->rewind.store(on == JNI_TRUE);
}

JNIEXPORT jlong JNICALL Java_io_github_replaynes_NativeBridge_frameCount(JNIEnv*, jobject) {
  std::lock_guard<std::mutex> g(g_emuMutex);
  return g_emu ? static_cast<jlong>(g_emu->frames.load()) : 0;
}

JNIEXPORT void JNICALL Java_io_github_replaynes_NativeBridge_setSurface(JNIEnv* env, jobject, jobject surface,
                                                                         jint width, jint height) {
  g_display.setWindow(surface ? ANativeWindow_fromSurface(env, surface) : nullptr, height);
}

}  // extern "C"
