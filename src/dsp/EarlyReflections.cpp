#include "rvrbotron/dsp/EarlyReflections.h"

#include "rvrbotron/dsp/OwnedBytes.h"

#include <algorithm>
#include <stdexcept>

namespace rvrbotron::dsp {

EarlyReflections::EarlyReflections(const ResolvedEarlyReflections& config)
    : enabled_(config.enabled),
      gain_(static_cast<Sample>(config.gain)),
      downmix_(config.downmix),
      accumulator_(config.downmix.inputChannels, Sample{0}) {
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
  std::fill(accumulator_.begin(), accumulator_.end(), Sample{0});
}

const DiffuserEarlyTap* EarlyReflections::taps() const noexcept {
  return taps_.data();
}

std::size_t EarlyReflections::tapCount() const noexcept {
  return taps_.size();
}

void EarlyReflections::processFrame(
    Sample* const left, Sample* const right) const noexcept {
  Sample* const scratch[]{left, right};
  downmix_.processFrame(accumulator_.data(), scratch, 0);
  *left *= gain_;
  *right *= gain_;
}

bool EarlyReflections::enabled() const noexcept {
  return enabled_;
}

std::size_t EarlyReflections::ownedBytes() const noexcept {
  return sizeof(*this) + downmix_.ownedStorageBytes() +
         ownedVectorBytes(accumulator_) + ownedVectorBytes(taps_);
}

} // namespace rvrbotron::dsp
