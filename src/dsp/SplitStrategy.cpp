#include "rvrbotron/dsp/SplitStrategy.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace rvrbotron::dsp {

namespace {

void validateStereoPreservingSplit(
    const ResolvedSplit& config, const char* const name) {
  if (config.inputChannels != 2) {
    throw std::invalid_argument(
        std::string(name) + " SplitStrategy requires stereo input");
  }
  if (config.channels == 0 || (config.channels % 2U) != 0U) {
    throw std::invalid_argument(
        std::string(name) + " SplitStrategy requires an even Channel count");
  }
  if (!std::isfinite(config.channelGain) || config.channelGain <= 0.0) {
    throw std::invalid_argument(
        std::string(name) +
        " SplitStrategy requires a resolved positive Channel gain");
  }
}

} // namespace

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

std::size_t DuplicateSplitStrategy::ownedBytes() const noexcept {
  return sizeof(*this);
}

std::unique_ptr<SplitStrategy> makeSplitStrategy(
    const ResolvedSplit& config) {
  // Mono input has no left/right dimension to preserve, so every requested
  // strategy resolves to the same mono duplication mapping.
  if (config.inputChannels == 1) {
    return std::make_unique<DuplicateSplitStrategy>(config);
  }
  switch (config.strategy) {
  case SplitStrategyType::duplicate:
    return std::make_unique<DuplicateSplitStrategy>(config);
  case SplitStrategyType::stereoHalves:
    return std::make_unique<StereoHalvesSplitStrategy>(config);
  case SplitStrategyType::stereoInterleave:
    return std::make_unique<StereoInterleaveSplitStrategy>(config);
  }
  throw std::invalid_argument("unsupported SplitStrategy type");
}

StereoHalvesSplitStrategy::StereoHalvesSplitStrategy(
    const ResolvedSplit& config)
    : channels_(config.channels),
      half_(config.channels / 2U),
      channelGain_(static_cast<Sample>(config.channelGain)) {
  validateStereoPreservingSplit(config, "stereo-halves");
}

void StereoHalvesSplitStrategy::processFrame(
    const Sample* const* inputs,
    const std::size_t frame,
    Sample* const channels) const noexcept {
  const auto left = inputs[0][frame] * channelGain_;
  const auto right = inputs[1][frame] * channelGain_;
  std::fill_n(channels, half_, left);
  std::fill_n(channels + half_, channels_ - half_, right);
}

std::size_t StereoHalvesSplitStrategy::inputChannelCount() const noexcept {
  return 2;
}

std::size_t StereoHalvesSplitStrategy::channelCount() const noexcept {
  return channels_;
}

std::size_t StereoHalvesSplitStrategy::ownedBytes() const noexcept {
  return sizeof(*this);
}

StereoInterleaveSplitStrategy::StereoInterleaveSplitStrategy(
    const ResolvedSplit& config)
    : channels_(config.channels),
      channelGain_(static_cast<Sample>(config.channelGain)) {
  validateStereoPreservingSplit(config, "stereo-interleave");
}

void StereoInterleaveSplitStrategy::processFrame(
    const Sample* const* inputs,
    const std::size_t frame,
    Sample* const channels) const noexcept {
  const auto left = inputs[0][frame] * channelGain_;
  const auto right = inputs[1][frame] * channelGain_;
  for (std::size_t channel = 0; channel < channels_; ++channel) {
    channels[channel] = (channel % 2U == 0U) ? left : right;
  }
}

std::size_t StereoInterleaveSplitStrategy::inputChannelCount() const noexcept {
  return 2;
}

std::size_t StereoInterleaveSplitStrategy::channelCount() const noexcept {
  return channels_;
}

std::size_t StereoInterleaveSplitStrategy::ownedBytes() const noexcept {
  return sizeof(*this);
}

} // namespace rvrbotron::dsp
