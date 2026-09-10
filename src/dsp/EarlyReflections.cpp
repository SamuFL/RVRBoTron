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
  // Every tap shares this object's one accumulator (docs/design/reverb/
  // stages/07-early-reflections.md's "Early envelope": taps are shaped
  // and summed into a single N-Channel frame before Downmix), so
  // accumulator_ must already be sized and stable before taps_ captures
  // its data() pointer -- both true here, since accumulator_ is
  // constructed above and never resized afterward.
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
  // downmix_ is embedded by value, so its own in-place storage is already
  // part of sizeof(*this); downmix_.ownedStorageBytes() adds only its
  // backing-vector allocations, not a second sizeof(Downmix) (issue #111,
  // see Downmix::ownedStorageBytes()'s own declaration).
  return sizeof(*this) + downmix_.ownedStorageBytes() +
         ownedVectorBytes(accumulator_) + ownedVectorBytes(taps_);
}

} // namespace rvrbotron::dsp
