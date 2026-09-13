#include "rvrbotron/dsp/Diffuser.h"

#include "rvrbotron/dsp/DiffusionStep.h"
#include "rvrbotron/dsp/OwnedBytes.h"

#include <algorithm>
#include <stdexcept>

namespace rvrbotron::dsp {

Diffuser::Diffuser(const ResolvedDiffuser& config)
    : channels_(0),
      totalSamples_(config.totalSamples) {
  if (config.steps.empty()) {
    throw std::invalid_argument(
        "Diffuser requires at least one resolved Diffusion Step");
  }

  channels_ = config.steps.front().delaysSamples.size();
  if (channels_ == 0) {
    throw std::invalid_argument(
        "Diffuser requires at least one Channel");
  }

  stepIndices_.reserve(config.steps.size());
  steps_.reserve(config.steps.size());
  for (std::size_t index = 0; index < config.steps.size(); ++index) {
    const auto& step = config.steps[index];
    if (step.index != index) {
      throw std::invalid_argument(
          "Diffuser requires ordered zero-based Diffusion Step indices");
    }
    if (step.delaysSamples.size() != channels_) {
      throw std::invalid_argument(
          "Diffuser requires the same Channel count in every Diffusion Step");
    }
    stepIndices_.push_back(step.index);
    steps_.push_back(std::make_unique<DiffusionStep>(step));
  }
}

Diffuser::~Diffuser() = default;

void Diffuser::processFrame(
    const Sample* const inputs,
    Sample* const outputs,
    DiffuserCaptureSink* const captureSink,
    const DiffuserEarlyTap* const earlyTaps,
    const std::size_t earlyTapCount) noexcept {
  if (inputs != outputs) {
    std::copy_n(inputs, channels_, outputs);
  }
  std::size_t nextTap = 0;
  for (std::size_t index = 0; index < steps_.size(); ++index) {
    steps_[index]->processFrame(outputs, outputs);
    if (captureSink != nullptr) {
      captureSink->captureDiffusionStepFrame(
          stepIndices_[index], outputs, channels_);
    }
    if (nextTap < earlyTapCount &&
        earlyTaps[nextTap].stepIndex == stepIndices_[index]) {
      const auto& tap = earlyTaps[nextTap];
      for (std::size_t channel = 0; channel < channels_; ++channel) {
        tap.accumulator[channel] += outputs[channel] * tap.gain;
      }
      ++nextTap;
    }
  }
}

std::size_t Diffuser::channelCount() const noexcept {
  return channels_;
}

std::size_t Diffuser::stepCount() const noexcept {
  return steps_.size();
}

std::uint64_t Diffuser::totalSamples() const noexcept {
  return totalSamples_;
}

std::size_t Diffuser::ownedBytes() const noexcept {
  std::size_t total = sizeof(*this) + ownedVectorBytes(stepIndices_) +
                      ownedVectorBytes(steps_);
  for (const auto& step : steps_) {
    total += step->ownedBytes();
  }
  return total;
}

} // namespace rvrbotron::dsp
