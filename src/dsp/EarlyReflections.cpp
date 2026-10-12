#include "rvrbotron/dsp/EarlyReflections.h"

#include "rvrbotron/dsp/OwnedBytes.h"
#include "rvrbotron/dsp/ProcessChunk.h"

#include <algorithm>
#include <stdexcept>

namespace rvrbotron::dsp {

EarlyReflections::EarlyReflections(const ResolvedEarlyReflections& config)
    : enabled_(config.enabled),
      gain_(static_cast<Sample>(config.gain)),
      downmix_(config.downmix),
      accumulator_(config.downmix.inputChannels * kMaxChunkFrames, Sample{0}) {
  if (config.taps.empty()) {
    throw std::invalid_argument(
        "EarlyReflections requires at least one resolved tap");
  }
  taps_.reserve(config.taps.size());
  for (const auto& tap : config.taps) {
    taps_.push_back(
        {tap.stepIndex,
         static_cast<Sample>(tap.gain),
         accumulator_.data()});
  }
}

void EarlyReflections::beginFrame() noexcept {
  beginBlock(1);
}

void EarlyReflections::beginBlock(const std::size_t frames) noexcept {
  std::fill_n(accumulator_.begin(), downmix_.inputChannelCount() * frames,
              Sample{0});
}

const DiffuserEarlyTap* EarlyReflections::taps() const noexcept {
  return taps_.data();
}

std::size_t EarlyReflections::tapCount() const noexcept {
  return taps_.size();
}

void EarlyReflections::processFrame(
    Sample* const left, Sample* const right) const noexcept {
  processBlock(left, right, 1);
}

void EarlyReflections::processBlock(
    Sample* const left, Sample* const right, const std::size_t frames)
    const noexcept {
  downmix_.processBlock(accumulator_.data(), frames, left, right);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    left[frame] *= gain_;
    right[frame] *= gain_;
  }
}

bool EarlyReflections::enabled() const noexcept {
  return enabled_;
}

std::size_t EarlyReflections::ownedBytes() const noexcept {
  return sizeof(*this) + downmix_.ownedStorageBytes() +
         ownedVectorBytes(accumulator_) + ownedVectorBytes(taps_);
}

} // namespace rvrbotron::dsp
