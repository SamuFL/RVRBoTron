#include "rvrbotron/dsp/DiffusionStep.h"

#include "rvrbotron/dsp/AllpassState.h"
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

  if (config.modulation.has_value() &&
      !config.modulation->channelModulated.empty()) {
    const auto& modulation = *config.modulation;
    if (modulation.channelSeeds.size() != channels_ ||
        modulation.channelTargetsPerSample.size() != channels_ ||
        modulation.channelPhases.size() != channels_ ||
        modulation.channelModulated.size() != channels_) {
      throw std::invalid_argument(
          "Diffusion Step requires one resolved Modulation seed, rate, "
          "phase, and bypass flag per Channel");
    }
    modulation_.emplace(modulation);
    interpolation_ = modulation.interpolation;

    if (interpolation_ == ModulationInterpolation::allpass) {
      buildAllpassState(
          modulation.channelModulated, allpassState_, allpassStateIndex_);
    }
  }
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
    if (modulation_.has_value() && modulation_->isModulated(channel)) {
      const auto lookback = modulation_->lookbackSamples(channel, delay);
      switch (interpolation_) {
      case ModulationInterpolation::linear:
        delayedValues_[channel] = delayLine_.readFractionLinear(channel, lookback);
        break;
      case ModulationInterpolation::allpass:
        delayedValues_[channel] = delayLine_.readFractionAllpass(
            channel, lookback, allpassState_[allpassStateIndex_[channel]]);
        break;
      case ModulationInterpolation::lagrange3:
        delayedValues_[channel] = delayLine_.readFraction(channel, lookback);
        break;
      }
    } else {
      delayedValues_[channel] = delayLine_.read(channel);
    }
    delayLine_.write(channel, inputs[channel]);
  }

  for (std::size_t channel = 0; channel < channels_; ++channel) {
    outputs[channel] =
        delayedValues_[permutation_[channel]] * polarity_[channel];
  }

  mix_->mix(outputs);

  if (modulation_.has_value()) {
    modulation_->advanceFrame();
  }
}

std::size_t DiffusionStep::channelCount() const noexcept {
  return channels_;
}

std::size_t DiffusionStep::ownedBytes() const noexcept {
  return sizeof(*this) + delayLine_.ownedStorageBytes() +
         ownedVectorBytes(permutation_) + ownedVectorBytes(polarity_) +
         ownedVectorBytes(delayedValues_) + mix_->ownedBytes() +
         (modulation_.has_value() ? modulation_->ownedBytes() : 0) +
         ownedVectorBytes(allpassState_) + ownedVectorBytes(allpassStateIndex_);
}

} // namespace rvrbotron::dsp
