#include "rvrbotron/dsp/FeedbackLoop.h"

#include "rvrbotron/dsp/AllpassState.h"
#include "rvrbotron/dsp/MixMatrix.h"
#include "rvrbotron/dsp/OwnedBytes.h"
#include "rvrbotron/dsp/ProcessChunk.h"

#include <algorithm>
#include <cassert>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace rvrbotron::dsp {
namespace {

constexpr Sample kDenormalFlushThreshold =
    std::numeric_limits<Sample>::min();

Sample flushDenormal(const Sample value) noexcept {
  return std::abs(value) < kDenormalFlushThreshold ? Sample{0} : value;
}

} // namespace

FeedbackLoop::FeedbackLoop(const ResolvedFeedbackLoop& config)
    : channels_(config.delaysSamples.size()),
      tailBudgetSamples_(config.tailBudgetSamples),
      blockSizeBoundSamples_(config.blockSizeBoundSamples),
      delayLine_(config.delaysSamples, config.bufferSizes),
      mix_(nullptr),
      fedBack_(channels_ * kMaxChunkFrames) {
  if (config.gains.size() != channels_) {
    throw std::invalid_argument(
        "Feedback Loop requires one resolved gain per Channel");
  }

  gains_.reserve(config.gains.size());
  for (const auto gain : config.gains) {
    if (!(gain > 0.0) || !(gain < 1.0)) {
      throw std::invalid_argument(
          "Feedback Loop gains must be strictly between zero and one");
    }
    gains_.push_back(static_cast<Sample>(gain));
  }
  mix_ = makeMixMatrix(config.mix, channels_, config.matrix);

  dampingEnabled_ = config.damping.has_value();
  if (dampingEnabled_) {
    const auto& damping = *config.damping;
    if (damping.highShelfGains.size() != channels_ ||
        damping.highShelfB0.size() != channels_ ||
        damping.highShelfB1.size() != channels_ ||
        damping.highShelfA1.size() != channels_ ||
        damping.lowShelfGains.size() != channels_ ||
        damping.lowShelfB0.size() != channels_ ||
        damping.lowShelfB1.size() != channels_ ||
        damping.lowShelfA1.size() != channels_) {
      throw std::invalid_argument(
          "Feedback Loop requires one resolved Damping coefficient set per "
          "Channel");
    }
    const auto loadShelf =
        [this](
            const std::vector<double>& gains,
            const std::vector<double>& b0,
            const std::vector<double>& b1,
            const std::vector<double>& a1,
            std::vector<Sample>& targetB0,
            std::vector<Sample>& targetB1,
            std::vector<Sample>& targetA1,
            std::vector<Sample>& prevInput,
            std::vector<Sample>& prevOutput) {
          targetB0.reserve(channels_);
          targetB1.reserve(channels_);
          targetA1.reserve(channels_);
          for (std::size_t channel = 0; channel < channels_; ++channel) {
            const auto gain = gains[channel];
            const auto coefficient0 = b0[channel];
            const auto coefficient1 = b1[channel];
            const auto coefficientA = a1[channel];
            if (!std::isfinite(gain) || !(gain > 0.0) ||
                !std::isfinite(coefficient0) ||
                !std::isfinite(coefficient1) ||
                !std::isfinite(coefficientA)) {
              throw std::invalid_argument(
                  "Feedback Loop requires finite resolved Damping "
                  "coefficients");
            }
            const auto sampleA1 = static_cast<Sample>(coefficientA);
            if (!(std::abs(sampleA1) < Sample{1})) {
              throw std::invalid_argument(
                  "Feedback Loop requires a stable resolved Damping shelf "
                  "pole (|a1| < 1)");
            }
            targetB0.push_back(static_cast<Sample>(coefficient0));
            targetB1.push_back(static_cast<Sample>(coefficient1));
            targetA1.push_back(sampleA1);
          }
          prevInput.assign(channels_, Sample{0});
          prevOutput.assign(channels_, Sample{0});
        };
    highShelfBypassed_ = damping.highRatio == 1.0;
    loadShelf(
        damping.highShelfGains,
        damping.highShelfB0,
        damping.highShelfB1,
        damping.highShelfA1,
        highShelfB0_,
        highShelfB1_,
        highShelfA1_,
        highShelfPrevInput_,
        highShelfPrevOutput_);
    lowShelfBypassed_ = damping.lowRatio == 1.0;
    loadShelf(
        damping.lowShelfGains,
        damping.lowShelfB0,
        damping.lowShelfB1,
        damping.lowShelfA1,
        lowShelfB0_,
        lowShelfB1_,
        lowShelfA1_,
        lowShelfPrevInput_,
        lowShelfPrevOutput_);
  }

  if (config.modulation.has_value() &&
      !config.modulation->channelModulated.empty()) {
    const auto& modulation = *config.modulation;
    if (modulation.channelSeeds.size() != channels_ ||
        modulation.channelTargetsPerSample.size() != channels_ ||
        modulation.channelPhases.size() != channels_ ||
        modulation.channelModulated.size() != channels_) {
      throw std::invalid_argument(
          "Feedback Loop requires one resolved Modulation seed, rate, "
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

FeedbackLoop::~FeedbackLoop() = default;

void FeedbackLoop::processFrame(const Sample* const inputs,
                                Sample* const outputs) noexcept {
  processBlock(inputs, outputs, 1);
}

void FeedbackLoop::processBlock(const Sample* const inputs,
                                Sample* const outputs,
                                const std::size_t frames) noexcept {
  assert(frames <= kMaxChunkFrames);
  assert(frames == 1 || frames < blockSizeBoundSamples_);
  for (std::size_t channel = 0; channel < channels_; ++channel) {
    auto* const delayed = outputs + channel * frames;
    auto* const fedBack = fedBack_.data() + channel * frames;
    const auto delay = delayLine_.delaySamples(channel);
    if (delay == 0) {
      std::fill_n(delayed, frames, Sample{0});
    } else if (modulation_.has_value() && modulation_->isModulated(channel)) {
      for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto lookback =
            modulation_->lookbackSamples(channel, delay, frame);
        switch (interpolation_) {
        case ModulationInterpolation::linear:
          delayed[frame] =
              delayLine_.readFractionLinear(channel, lookback, frame);
          break;
        case ModulationInterpolation::allpass:
          delayed[frame] = delayLine_.readFractionAllpass(
              channel,
              lookback,
              allpassState_[allpassStateIndex_[channel]],
              frame);
          break;
        case ModulationInterpolation::lagrange3:
          delayed[frame] = delayLine_.readFraction(channel, lookback, frame);
          break;
        }
      }
    } else {
      delayLine_.readBlock(channel, delayed, frames);
    }
    const auto gain = gains_[channel];
    for (std::size_t frame = 0; frame < frames; ++frame) {
      fedBack[frame] = delayed[frame] * gain;
    }
  }

  const auto shelve = [this, frames](
                          const std::vector<Sample>& b0,
                          const std::vector<Sample>& b1,
                          const std::vector<Sample>& a1,
                          std::vector<Sample>& prevInput,
                          std::vector<Sample>& prevOutput) noexcept {
    for (std::size_t channel = 0; channel < channels_; ++channel) {
      auto* const values = fedBack_.data() + channel * frames;
      const auto coefficientB0 = b0[channel];
      const auto coefficientB1 = b1[channel];
      const auto coefficientA1 = a1[channel];
      auto previousInput = prevInput[channel];
      auto previousOutput = prevOutput[channel];
      for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto input = values[frame];
        const auto output = coefficientB0 * input +
            coefficientB1 * previousInput - coefficientA1 * previousOutput;
        previousInput = input;
        previousOutput = output;
        values[frame] = output;
      }
      prevInput[channel] = previousInput;
      prevOutput[channel] = previousOutput;
    }
  };
  if (!highShelfBypassed_) {
    shelve(highShelfB0_, highShelfB1_, highShelfA1_,
           highShelfPrevInput_, highShelfPrevOutput_);
  }
  if (!lowShelfBypassed_) {
    shelve(lowShelfB0_, lowShelfB1_, lowShelfA1_,
           lowShelfPrevInput_, lowShelfPrevOutput_);
  }

  mix_->mixBlock(fedBack_.data(), frames);

  for (std::size_t channel = 0; channel < channels_; ++channel) {
    if (delayLine_.delaySamples(channel) == 0) {
      continue;
    }
    const auto* const input = inputs + channel * frames;
    auto* const fedBack = fedBack_.data() + channel * frames;
    for (std::size_t frame = 0; frame < frames; ++frame) {
      fedBack[frame] = flushDenormal(input[frame] + fedBack[frame]);
    }
    delayLine_.writeBlock(channel, fedBack, frames);
  }

  if (modulation_.has_value()) {
    modulation_->advanceFrames(frames);
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
  return sizeof(*this) + delayLine_.ownedStorageBytes() +
         ownedVectorBytes(gains_) + ownedVectorBytes(fedBack_) +
         mix_->ownedBytes() +
         ownedVectorBytes(highShelfB0_) + ownedVectorBytes(highShelfB1_) +
         ownedVectorBytes(highShelfA1_) +
         ownedVectorBytes(highShelfPrevInput_) +
         ownedVectorBytes(highShelfPrevOutput_) +
         ownedVectorBytes(lowShelfB0_) + ownedVectorBytes(lowShelfB1_) +
         ownedVectorBytes(lowShelfA1_) +
         ownedVectorBytes(lowShelfPrevInput_) +
         ownedVectorBytes(lowShelfPrevOutput_) +
         (modulation_.has_value() ? modulation_->ownedBytes() : 0) +
         ownedVectorBytes(allpassState_) + ownedVectorBytes(allpassStateIndex_);
}

} // namespace rvrbotron::dsp
