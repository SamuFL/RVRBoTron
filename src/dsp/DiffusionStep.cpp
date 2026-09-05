#include "rvrbotron/dsp/DiffusionStep.h"

#include "rvrbotron/dsp/MixMatrix.h"
#include "rvrbotron/dsp/OwnedBytes.h"

#include <stdexcept>
#include <vector>

namespace rvrbotron::dsp {

DiffusionStep::DiffusionStep(const ResolvedDiffusionStep& config)
    : channels_(config.delaysSamples.size()),
      delayLine_(config.delaysSamples, config.bufferSizes),
      permutation_(config.permutation),
      mix_(nullptr),
      delayedValues_(channels_) {
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
    const auto delay = delayLine_.delaySamples(channel);
    if (delay == 0) {
      delayedValues_[channel] = inputs[channel];
      continue;
    }
    delayedValues_[channel] = delayLine_.read(channel);
    delayLine_.write(channel, inputs[channel]);
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

std::size_t DiffusionStep::ownedBytes() const noexcept {
  return sizeof(*this) + delayLine_.ownedStorageBytes() +
         ownedVectorBytes(permutation_) + ownedVectorBytes(polarity_) +
         ownedVectorBytes(delayedValues_) + mix_->ownedBytes();
}

} // namespace rvrbotron::dsp
