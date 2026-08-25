#include "rvrbotron/dsp/DiffusionStep.h"

#include "rvrbotron/dsp/MixMatrix.h"

#include <limits>
#include <stdexcept>
#include <vector>

namespace rvrbotron::dsp {

DiffusionStep::DiffusionStep(const ResolvedDiffusionStep& config)
    : channels_(config.delaysSamples.size()),
      delays_(config.delaysSamples),
      delayOffsets_(channels_),
      delayPositions_(channels_, 0),
      permutation_(config.permutation),
      mix_(nullptr),
      delayedValues_(channels_) {
  if (config.bufferSizes.size() != channels_) {
    throw std::invalid_argument(
        "Diffusion Step requires one resolved buffer size per Channel");
  }
  if (permutation_.size() != channels_ ||
      config.polaritySigns.size() != channels_) {
    throw std::invalid_argument(
        "Diffusion Step requires one mapping value per Channel");
  }

  std::vector<bool> seenPermutation(channels_, false);
  for (std::size_t channel = 0; channel < channels_; ++channel) {
    const auto source = permutation_[channel];
    if (source >= channels_ || seenPermutation[source]) {
      throw std::invalid_argument(
          "Diffusion Step requires a permutation of Channel indices");
    }
    seenPermutation[source] = true;
  }

  std::size_t totalDelayStorage = 0;
  for (std::size_t channel = 0; channel < channels_; ++channel) {
    delayOffsets_[channel] = totalDelayStorage;
    const auto delay = delays_[channel];
    const auto bufferSize = config.bufferSizes[channel];
    if (delay > std::numeric_limits<std::size_t>::max() ||
        bufferSize > std::numeric_limits<std::size_t>::max()) {
      throw std::length_error("Diffusion Step delay storage is too large");
    }
    if (bufferSize < delay) {
      throw std::invalid_argument(
          "Diffusion Step resolved buffer is shorter than its delay");
    }
    const auto storageSize = static_cast<std::size_t>(bufferSize);
    if (storageSize >
        std::numeric_limits<std::size_t>::max() - totalDelayStorage) {
      throw std::length_error("Diffusion Step delay storage is too large");
    }
    totalDelayStorage += storageSize;
  }
  delayStorage_.assign(totalDelayStorage, Sample{0});

  polarity_.reserve(config.polaritySigns.size());
  for (const auto sign : config.polaritySigns) {
    if (sign != -1 && sign != 1) {
      throw std::invalid_argument(
          "Diffusion Step polarity signs must be -1 or 1");
    }
    polarity_.push_back(static_cast<Sample>(sign));
  }
  mix_ = makeMixMatrix(config.mix, channels_, config.matrix);
}

DiffusionStep::~DiffusionStep() = default;

void DiffusionStep::processFrame(const Sample* const inputs,
                                 Sample* const outputs) noexcept {
  for (std::size_t channel = 0; channel < channels_; ++channel) {
    const auto delay = static_cast<std::size_t>(delays_[channel]);
    if (delay == 0) {
      delayedValues_[channel] = inputs[channel];
      continue;
    }
    const auto storageIndex =
        delayOffsets_[channel] + delayPositions_[channel];
    delayedValues_[channel] = delayStorage_[storageIndex];
    delayStorage_[storageIndex] = inputs[channel];
    delayPositions_[channel] = (delayPositions_[channel] + 1) % delay;
  }

  for (std::size_t channel = 0; channel < channels_; ++channel) {
    outputs[channel] =
        delayedValues_[permutation_[channel]] * polarity_[channel];
  }

  mix_->mix(outputs);
}

std::size_t DiffusionStep::channelCount() const noexcept {
  return channels_;
}

} // namespace rvrbotron::dsp
