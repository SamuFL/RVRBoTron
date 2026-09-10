#include "rvrbotron/dsp/EarlyReflections.h"

#include "rvrbotron/dsp/OwnedBytes.h"

#include <algorithm>
#include <stdexcept>

namespace rvrbotron::dsp {

EarlyReflections::EarlyReflections(const ResolvedEarlyReflections& config)
    : enabled_(config.enabled),
      tapStepIndex_(0),
      gain_(static_cast<Sample>(config.gain)),
      downmix_(config.downmix),
      accumulator_(config.downmix.inputChannels, Sample{0}) {
  if (config.taps.empty()) {
    throw std::invalid_argument(
        "EarlyReflections requires at least one resolved tap");
  }
  tapStepIndex_ = config.taps.front().stepIndex;
}

DiffuserEarlyTap EarlyReflections::beginFrame() noexcept {
  std::fill(accumulator_.begin(), accumulator_.end(), Sample{0});
  return {tapStepIndex_, accumulator_.data()};
}

void EarlyReflections::processFrame(
    Sample* const* const outputs, const std::size_t frame) const noexcept {
  Sample left{0};
  Sample right{0};
  Sample* const scratch[]{&left, &right};
  downmix_.processFrame(accumulator_.data(), scratch, 0);
  outputs[0][frame] += left * gain_;
  outputs[1][frame] += right * gain_;
}

bool EarlyReflections::enabled() const noexcept {
  return enabled_;
}

std::size_t EarlyReflections::ownedBytes() const noexcept {
  return sizeof(*this) + downmix_.ownedBytes() +
         ownedVectorBytes(accumulator_);
}

} // namespace rvrbotron::dsp
