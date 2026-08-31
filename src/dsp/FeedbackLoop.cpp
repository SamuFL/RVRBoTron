#include "rvrbotron/dsp/FeedbackLoop.h"

#include "rvrbotron/dsp/MixMatrix.h"
#include "rvrbotron/dsp/OwnedBytes.h"

#include <limits>
#include <stdexcept>

namespace rvrbotron::dsp {

FeedbackLoop::FeedbackLoop(const ResolvedFeedbackLoop& config)
    : channels_(config.delaysSamples.size()),
      tailBudgetSamples_(config.tailBudgetSamples),
      blockSizeBoundSamples_(config.blockSizeBoundSamples),
      delays_(config.delaysSamples),
      delayOffsets_(channels_),
      delayPositions_(channels_, 0),
      mix_(nullptr),
      fedBack_(channels_) {
  if (config.bufferSizes.size() != channels_ ||
      config.gains.size() != channels_) {
    throw std::invalid_argument(
        "Feedback Loop requires one resolved buffer size and gain per "
        "Channel");
  }

  std::size_t totalDelayStorage = 0;
  for (std::size_t channel = 0; channel < channels_; ++channel) {
    delayOffsets_[channel] = totalDelayStorage;
    const auto delay = delays_[channel];
    const auto bufferSize = config.bufferSizes[channel];
    if (delay > std::numeric_limits<std::size_t>::max() ||
        bufferSize > std::numeric_limits<std::size_t>::max()) {
      throw std::length_error("Feedback Loop delay storage is too large");
    }
    if (bufferSize < delay) {
      throw std::invalid_argument(
          "Feedback Loop resolved buffer is shorter than its delay");
    }
    const auto storageSize = static_cast<std::size_t>(bufferSize);
    if (storageSize >
        std::numeric_limits<std::size_t>::max() - totalDelayStorage) {
      throw std::length_error("Feedback Loop delay storage is too large");
    }
    totalDelayStorage += storageSize;
  }
  delayStorage_.assign(totalDelayStorage, Sample{0});

  gains_.reserve(config.gains.size());
  for (const auto gain : config.gains) {
    if (!(gain > 0.0) || !(gain < 1.0)) {
      throw std::invalid_argument(
          "Feedback Loop gains must be strictly between zero and one");
    }
    gains_.push_back(static_cast<Sample>(gain));
  }
  mix_ = makeMixMatrix(config.mix, channels_, config.matrix);
}

FeedbackLoop::~FeedbackLoop() = default;

void FeedbackLoop::processFrame(const Sample* const inputs,
                                Sample* const outputs) noexcept {
  for (std::size_t channel = 0; channel < channels_; ++channel) {
    const auto delay = static_cast<std::size_t>(delays_[channel]);
    const auto delayed = delay == 0
                              ? Sample{0}
                              : delayStorage_
                                    [delayOffsets_[channel] +
                                     delayPositions_[channel]];
    outputs[channel] = delayed;
    fedBack_[channel] = delayed * gains_[channel];
  }

  mix_->mix(fedBack_.data());

  for (std::size_t channel = 0; channel < channels_; ++channel) {
    const auto delay = static_cast<std::size_t>(delays_[channel]);
    if (delay == 0) {
      continue;
    }
    const auto storageIndex =
        delayOffsets_[channel] + delayPositions_[channel];
    delayStorage_[storageIndex] = inputs[channel] + fedBack_[channel];
    delayPositions_[channel] = (delayPositions_[channel] + 1) % delay;
  }
}

std::size_t FeedbackLoop::channelCount() const noexcept {
  return channels_;
}

std::uint64_t FeedbackLoop::tailBudgetSamples() const noexcept {
  return tailBudgetSamples_;
}

std::uint64_t FeedbackLoop::blockSizeBoundSamples() const noexcept {
  return blockSizeBoundSamples_;
}

std::size_t FeedbackLoop::ownedBytes() const noexcept {
  return sizeof(*this) + ownedVectorBytes(delays_) +
         ownedVectorBytes(delayOffsets_) +
         ownedVectorBytes(delayPositions_) +
         ownedVectorBytes(delayStorage_) + ownedVectorBytes(gains_) +
         ownedVectorBytes(fedBack_) + mix_->ownedBytes();
}

} // namespace rvrbotron::dsp
