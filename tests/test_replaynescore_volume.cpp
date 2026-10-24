#include "core/ICore.h"
#include "support/AudioVolumeRom.h"
#include "support/rn_test.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr unsigned kFrames = 120;
constexpr unsigned kWarmupFrames = 60;
constexpr double kMinimumRatio = 0.90;
constexpr double kMaximumRatio = 1.10;

struct Measurement {
  double rms = 0, mean = 0;
  int peak = 0;
  size_t samples = 0, clipped = 0;
};

void writeWord(std::ostream& out, uint32_t value, unsigned bytes) {
  for (unsigned i = 0; i < bytes; ++i) out.put(char(value >> (8 * i)));
}

void saveArtifacts(const std::vector<uint8_t>& rom, const std::vector<int16_t>& pcm,
                   const char* test, rn::CoreKind kind) {
  const char* root = std::getenv("RN_VOLUME_ARTIFACT_DIR");
  if (!root || !*root) return;
  std::filesystem::create_directories(root);
  const std::string base = std::string(root) + "/" + test;
  std::ofstream nes(base + ".nes", std::ios::binary);
  REQUIRE(nes.good());
  nes.write(reinterpret_cast<const char*>(rom.data()), std::streamsize(rom.size()));
  REQUIRE(nes.good());
  std::ofstream wav(base + "-" + rn::coreName(kind) + ".wav", std::ios::binary);
  REQUIRE(wav.good());
  const uint32_t bytes = uint32_t(pcm.size() * 2);
  wav.write("RIFF", 4); writeWord(wav, 36 + bytes, 4); wav.write("WAVEfmt ", 8);
  writeWord(wav, 16, 4); writeWord(wav, 1, 2); writeWord(wav, 1, 2);
  writeWord(wav, rn::kSampleRate, 4); writeWord(wav, rn::kSampleRate * 2, 4);
  writeWord(wav, 2, 2); writeWord(wav, 16, 2); wav.write("data", 4); writeWord(wav, bytes, 4);
  for (int16_t sample : pcm) writeWord(wav, uint16_t(sample), 2);
  REQUIRE(wav.good());
}

Measurement measure(const rntest::VolumeCase& test, rn::CoreKind kind) {
  const auto rom = rntest::volumeRom(test);
  auto core = rn::createCore(kind);
  REQUIRE(core != nullptr);
  REQUIRE(core->loadROM(rom.data(), rom.size()).ok());
  std::vector<int16_t> pcm;
  pcm.reserve(size_t(rn::audioSamplesBefore(kFrames)));
  for (unsigned frame = 0; frame < kFrames; ++frame) {
    REQUIRE(core->stepFrame(0, 0, false).ok());
    size_t count = 0;
    const auto* samples = core->audio(&count);
    REQUIRE(samples != nullptr);
    REQUIRE_EQ(count, size_t(rn::audioSamplesForFrame(frame)));
    pcm.insert(pcm.end(), samples, samples + count);
  }
  saveArtifacts(rom, pcm, test.name, kind);
  // Include power-on transients: steady-state-only checks miss DC startup clipping.
  CHECK_EQ(std::count_if(pcm.begin(), pcm.end(), [](int16_t value) {
    return value == -32768 || value == 32767;
  }), 0);
  Measurement result;
  double squares = 0, sum = 0;
  const size_t begin = size_t(rn::audioSamplesBefore(kWarmupFrames));
  result.samples = pcm.size() - begin;
  for (size_t i = begin; i < pcm.size(); ++i) {
    const int value = pcm[i];
    squares += double(value) * value;
    sum += value;
    result.peak = std::max(result.peak, std::abs(value));
    if (value == -32768 || value == 32767) ++result.clipped;
  }
  result.rms = std::sqrt(squares / double(result.samples));
  result.mean = sum / double(result.samples);
  // Silence, DC-only output, clipping and missing samples must never pass a
  // ratio check. The AC check detects a cartridge that failed to start its tone.
  CHECK(result.samples > 47000);
  CHECK(result.rms > 100);
  CHECK(result.rms * result.rms - result.mean * result.mean > 10000);
  CHECK_EQ(result.clipped, size_t(0));
  return result;
}
} // namespace

TEST_CASE("known APU waveforms retain Nestopia volume across cores") {
  std::cout << "volume: frames=120 warmup=60 sample_rate=48000 ratio_limit=[0.90,1.10]\n";
  std::cout << "case,nestopia_rms,nestopia_peak,nestopia_mean,replaynescore_rms,"
               "replaynescore_peak,replaynescore_mean,ratio\n";
  for (const auto& test : rntest::kVolumeCases) {
    const auto reference = measure(test, rn::CoreKind::Nestopia);
    std::cout << std::fixed << std::setprecision(6) << test.name << ','
              << reference.rms << ',' << reference.peak << ',' << reference.mean;
#ifdef REPLAYNES_HAS_REPLAYNESCORE
    const auto candidate = measure(test, rn::CoreKind::ReplayNEScore);
    const double ratio = candidate.rms / reference.rms;
    std::cout << ',' << candidate.rms << ',' << candidate.peak << ',' << candidate.mean
              << ',' << ratio << std::endl;
    // Nonfatal checks allow every channel to be measured even when one fails.
    CHECK(ratio >= kMinimumRatio);
    CHECK(ratio <= kMaximumRatio);
#else
    std::cout << ",not-built,not-built,not-built,not-compared\n";
#endif
  }
}
