#include "rvrbotron/dsp/DiffusionStep.h"

#include "rvrbotron/dsp/AllpassState.h"
#include "rvrbotron/dsp/MixMatrix.h"
#include "rvrbotron/dsp/OwnedBytes.h"
#include "rvrbotron/dsp/ProcessChunk.h"

#include <algorithm>
#include <cassert>

#include <stdexcept>
#include <vector>

namespace rvrbotron::dsp {

DiffusionStep::DiffusionStep(const ResolvedDiffusionStep& config)
    : channels_(config.delaysSamples.size()),
      delayLine_(config.delaysSamples, config.bufferSizes),
      permutation_(config.permutation),
      mix_(nullptr),
      delayedValues_(channels_ * kMaxChunkFrames) {
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
  if (inputs != outputs) {
    std::copy_n(inputs, channels_, outputs);
  }
  processBlock(outputs, 1);
}

void DiffusionStep::processBlock(Sample* const channels,
                                 const std::size_t frames) noexcept {
  assert(frames <= kMaxChunkFrames);
  for (std::size_t channel = 0; channel < channels_; ++channel) {
    const auto* const input = channels + channel * frames;
    auto* const delayed = delayedValues_.data() + channel * frames;
    const auto delay = delayLine_.delaySamples(channel);
    if (delay == 0) {
      std::copy_n(input, frames, delayed);
    } else if (modulation_.has_value() && modulation_->isModulated(channel)) {
      // Channels are independent, so one Channel's frames can run back to
      // back: each read sees exactly the writes it would per frame.
      for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto lookback =
            modulation_->lookbackSamples(channel, delay, frame);
        switch (interpolation_) {
        case ModulationInterpolation::linear:
          delayed[frame] = delayLine_.readFractionLinear(channel, lookback);
          break;
        case ModulationInterpolation::allpass:
          delayed[frame] = delayLine_.readFractionAllpass(
              channel, lookback, allpassState_[allpassStateIndex_[channel]]);
          break;
        case ModulationInterpolation::lagrange3:
          delayed[frame] = delayLine_.readFraction(channel, lookback);
          break;
        }
        delayLine_.write(channel, input[frame]);
      }
    } else {
      delayLine_.delayBlock(channel, input, delayed, frames);
    }
  }

  for (std::size_t channel = 0; channel < channels_; ++channel) {
    const auto* const source =
        delayedValues_.data() + permutation_[channel] * frames;
    const auto sign = polarity_[channel];
    auto* const output = channels + channel * frames;
    for (std::size_t frame = 0; frame < frames; ++frame) {
      output[frame] = source[frame] * sign;
    }
  }

  mix_->mixBlock(channels, frames);

  if (modulation_.has_value()) {
    modulation_->advanceFrames(frames);
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
