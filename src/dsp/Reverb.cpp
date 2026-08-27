#include "rvrbotron/dsp/Reverb.h"

#include "rvrbotron/dsp/Diffuser.h"
#include "rvrbotron/dsp/Downmix.h"
#include "rvrbotron/dsp/FeedbackLoop.h"
#include "rvrbotron/dsp/OwnedBytes.h"
#include "rvrbotron/dsp/Split.h"

#include <algorithm>
#include <cstddef>
#include <variant>
#include <vector>

namespace rvrbotron::dsp {

struct Reverb::Implementation final : DiffuserCaptureSink {
  bool identity = true;
  std::size_t inputChannels = 0;
  std::size_t outputChannels = 0;
  std::size_t channels = 0;
  std::uint64_t tailFrames = 0;
  StageCaptureSink* captureSink = nullptr;

  std::unique_ptr<Split> split;
  std::unique_ptr<Diffuser> diffuser;
  std::unique_ptr<FeedbackLoop> feedbackLoop;
  std::unique_ptr<Downmix> downmix;

  std::vector<Sample> splitValues;
  // Whatever the middle stage (Diffuser or Feedback Loop) writes: the
  // aligned diffused signal, or the unaligned circulating tail.
  std::vector<Sample> midStageValues;

  void captureDiffusionStepFrame(
      const std::uint32_t index,
      const Sample* const values,
      const std::size_t channelCount) noexcept override {
    if (captureSink != nullptr) {
      captureSink->captureFrame(
          StageCaptureBoundary::diffusionStep,
          index,
          values,
          channelCount);
    }
  }
};

Reverb::Reverb(const ResolvedConfig& config,
               StageCaptureSink* const captureSink)
    : implementation_(std::make_unique<Implementation>()) {
  auto& state = *implementation_;
  state.captureSink = captureSink;
  if (config.composition.stages.empty()) {
    return;
  }

  const auto& split =
      std::get<ResolvedSplit>(config.composition.stages[0]);
  const auto& downmix =
      std::get<ResolvedDownmix>(config.composition.stages[2]);

  state.identity = false;
  state.inputChannels = split.inputChannels;
  state.outputChannels = downmix.outputChannels;
  state.channels = split.channels;
  state.split = std::make_unique<Split>(split);

  if (const auto* diffuser =
          std::get_if<ResolvedDiffuser>(&config.composition.stages[1])) {
    state.diffuser = std::make_unique<Diffuser>(*diffuser);
    state.tailFrames = state.diffuser->totalSamples();
  } else {
    const auto& feedbackLoop =
        std::get<ResolvedFeedbackLoop>(config.composition.stages[1]);
    state.feedbackLoop = std::make_unique<FeedbackLoop>(feedbackLoop);
    state.tailFrames = state.feedbackLoop->tailBudgetSamples();
  }

  state.downmix = std::make_unique<Downmix>(downmix);
  state.splitValues.resize(state.channels);
  state.midStageValues.resize(state.channels);
}

Reverb::~Reverb() = default;
Reverb::Reverb(Reverb&&) noexcept = default;
Reverb& Reverb::operator=(Reverb&&) noexcept = default;

void Reverb::process(const Sample* const* inputs,
                     const std::size_t inputChannelCount,
                     Sample* const* outputs,
                     const std::size_t outputChannelCount,
                     const std::size_t frameCount) noexcept {
  auto& state = *implementation_;
  if (state.identity) {
    const auto channels = std::min(inputChannelCount, outputChannelCount);
    for (std::size_t channel = 0; channel < channels; ++channel) {
      std::copy_n(inputs[channel], frameCount, outputs[channel]);
    }
    return;
  }

  if (inputChannelCount != state.inputChannels ||
      outputChannelCount != state.outputChannels) {
    for (std::size_t channel = 0; channel < outputChannelCount; ++channel) {
      std::fill_n(outputs[channel], frameCount, Sample{0});
    }
    return;
  }

  for (std::size_t frame = 0; frame < frameCount; ++frame) {
    state.split->processFrame(inputs, frame, state.splitValues.data());
    if (state.captureSink != nullptr) {
      state.captureSink->captureFrame(
          StageCaptureBoundary::split,
          0,
          state.splitValues.data(),
          state.channels);
    }

    if (state.diffuser != nullptr) {
      state.diffuser->processFrame(
          state.splitValues.data(),
          state.midStageValues.data(),
          state.captureSink != nullptr ? &state : nullptr);
    } else {
      state.feedbackLoop->processFrame(
          state.splitValues.data(), state.midStageValues.data());
    }

    state.downmix->processFrame(
        state.midStageValues.data(), outputs, frame);
  }
}

std::size_t Reverb::inputChannelCount() const noexcept {
  return implementation_->inputChannels;
}

std::size_t Reverb::outputChannelCount() const noexcept {
  return implementation_->outputChannels;
}

std::uint64_t Reverb::tailBudgetFrames() const noexcept {
  return implementation_->tailFrames;
}

std::size_t Reverb::ownedBytes() const noexcept {
  const auto& state = *implementation_;
  std::size_t total = sizeof(state) + ownedVectorBytes(state.splitValues) +
                      ownedVectorBytes(state.midStageValues);
  if (state.split != nullptr) {
    total += state.split->ownedBytes();
  }
  if (state.diffuser != nullptr) {
    total += state.diffuser->ownedBytes();
  }
  if (state.feedbackLoop != nullptr) {
    total += state.feedbackLoop->ownedBytes();
  }
  if (state.downmix != nullptr) {
    total += state.downmix->ownedBytes();
  }
  return total;
}

} // namespace rvrbotron::dsp
