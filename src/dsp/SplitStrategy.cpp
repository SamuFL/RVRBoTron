#include "rvrbotron/dsp/SplitStrategy.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rvrbotron::dsp {

DuplicateSplitStrategy::DuplicateSplitStrategy(
    const ResolvedSplit& config)
    : inputChannels_(config.inputChannels),
      channels_(config.channels),
      sourceGain_(static_cast<Sample>(config.sourceGain)),
      channelGain_(static_cast<Sample>(config.channelGain)) {
  if (inputChannels_ == 0 || inputChannels_ > 2) {
    throw std::invalid_argument(
        "Duplicate SplitStrategy requires mono or stereo input");
  }
  if (channels_ == 0) {
    throw std::invalid_argument(
        "Duplicate SplitStrategy requires at least one Channel");
  }
  if (!std::isfinite(config.sourceGain) || config.sourceGain <= 0.0 ||
      !std::isfinite(config.channelGain) || config.channelGain <= 0.0) {
    throw std::invalid_argument(
        "Duplicate SplitStrategy requires resolved positive mapping gains");
  }
}

void DuplicateSplitStrategy::processFrame(
    const Sample* const* inputs,
    const std::size_t frame,
    Sample* const channels) const noexcept {
  Sample selected = inputs[0][frame];
  if (inputChannels_ == 2) {
    selected += inputs[1][frame];
  }
  const auto mapped = selected * sourceGain_ * channelGain_;
  std::fill_n(channels, channels_, mapped);
}

std::size_t DuplicateSplitStrategy::inputChannelCount() const noexcept {
  return inputChannels_;
}

std::size_t DuplicateSplitStrategy::channelCount() const noexcept {
  return channels_;
}

std::unique_ptr<SplitStrategy> makeSplitStrategy(
    const ResolvedSplit& config) {
  switch (config.strategy) {
  case SplitStrategyType::duplicate:
    return std::make_unique<DuplicateSplitStrategy>(config);
  }
  throw std::invalid_argument("unsupported SplitStrategy type");
}

} // namespace rvrbotron::dsp
