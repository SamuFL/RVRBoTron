#include "rvrbotron/dsp/DelayLine.h"

#include "rvrbotron/dsp/OwnedBytes.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace rvrbotron::dsp {

DelayLine::DelayLine(
    std::vector<std::uint64_t> delaysSamples,
    std::vector<std::uint64_t> bufferSizes)
    : delays_(std::move(delaysSamples)),
      offsets_(delays_.size()),
      positions_(delays_.size(), 0) {
  if (bufferSizes.size() != delays_.size()) {
    throw std::invalid_argument(
        "DelayLine requires one resolved buffer size per Channel");
  }

  std::size_t totalStorage = 0;
  for (std::size_t channel = 0; channel < delays_.size(); ++channel) {
    offsets_[channel] = totalStorage;
    const auto delay = delays_[channel];
    const auto bufferSize = bufferSizes[channel];
    if (delay > std::numeric_limits<std::size_t>::max() ||
        bufferSize > std::numeric_limits<std::size_t>::max()) {
      throw std::length_error("DelayLine delay storage is too large");
    }
    if (bufferSize < delay) {
      throw std::invalid_argument(
          "DelayLine resolved buffer is shorter than its delay");
    }
    const auto storageSize = static_cast<std::size_t>(bufferSize);
    if (storageSize >
        std::numeric_limits<std::size_t>::max() - totalStorage) {
      throw std::length_error("DelayLine delay storage is too large");
    }
    totalStorage += storageSize;
  }
  storage_.assign(totalStorage, Sample{0});
}

Sample DelayLine::read(const std::size_t channel) const noexcept {
  return storage_[offsets_[channel] + positions_[channel]];
}

void DelayLine::write(const std::size_t channel, const Sample value) noexcept {
  const auto index = offsets_[channel] + positions_[channel];
  storage_[index] = value;
  const auto delay = static_cast<std::size_t>(delays_[channel]);
  positions_[channel] = (positions_[channel] + 1) % delay;
}

std::uint64_t DelayLine::delaySamples(
    const std::size_t channel) const noexcept {
  return delays_[channel];
}

std::size_t DelayLine::ownedBytes() const noexcept {
  return ownedVectorBytes(delays_) + ownedVectorBytes(offsets_) +
         ownedVectorBytes(positions_) + ownedVectorBytes(storage_);
}

} // namespace rvrbotron::dsp
