#include "rvrbotron/dsp/Reverb.h"

#include "rvrbotron/dsp/Diffuser.h"
#include "rvrbotron/dsp/Downmix.h"
#include "rvrbotron/dsp/FeedbackLoop.h"
#include "rvrbotron/dsp/OwnedBytes.h"
#include "rvrbotron/dsp/Split.h"

#include <algorithm>
#include <cassert>
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
  // The Feedback Loop's shortest resolved delay, in samples: the maximum
  // legal block size (see CONTEXT.md's Block-size bound entry). Zero when
  // no Feedback Loop is present, meaning no bound applies.
  std::uint64_t blockSizeBound = 0;
  StageCaptureSink* captureSink = nullptr;
  // The Main wet path's own enablement and level (issue #109). Moot while
  // `identity` is true. Disabled skips the Downmix call entirely below,
  // contributing exact stereo zero rather than a zero-multiplied value.
  bool mainEnabled = true;
  Sample mainGain{1};

  std::unique_ptr<Split> split;
  std::unique_ptr<Diffuser> diffuser;
  std::unique_ptr<FeedbackLoop> feedbackLoop;
  std::unique_ptr<Downmix> downmix;

  std::vector<Sample> splitValues;
  // Whatever the last middle stage (Diffuser or Feedback Loop) writes: the
  // aligned diffused signal, or the unaligned circulating tail.
  std::vector<Sample> midStageValues;
  // Only sized/used when both a Diffuser and a Feedback Loop are present:
  // the Diffuser's aligned output, which becomes the loop's input. Kept
  // distinct from midStageValues because FeedbackLoop::processFrame does
  // not support processing in place.
  std::vector<Sample> diffuserOutputValues;

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
      std::get<ResolvedSplit>(config.composition.stages.front());
  const auto& downmix =
      std::get<ResolvedDownmix>(config.composition.stages.back());

  state.identity = false;
  state.inputChannels = split.inputChannels;
  state.outputChannels = downmix.outputChannels;
  state.channels = split.channels;
  state.mainEnabled = config.composition.mainEnabled;
  state.mainGain = static_cast<Sample>(config.composition.mainGain);
  state.split = std::make_unique<Split>(split);

  // Middle stages: none, a Diffuser alone, a Feedback Loop alone, or a
  // Diffuser followed by a Feedback Loop (see validateShape in
  // ResolveConfig.cpp for the shapes this can be). Total drain is the
  // Diffuser's own finite response plus the loop's Tail budget when both
  // are present.
  const auto downmixIndex = config.composition.stages.size() - 1;
  for (std::size_t stageIndex = 1; stageIndex < downmixIndex; ++stageIndex) {
    if (const auto* diffuser = std::get_if<ResolvedDiffuser>(
            &config.composition.stages[stageIndex])) {
      state.diffuser = std::make_unique<Diffuser>(*diffuser);
      state.tailFrames += state.diffuser->totalSamples();
    } else {
      const auto& feedbackLoop = std::get<ResolvedFeedbackLoop>(
          config.composition.stages[stageIndex]);
      state.feedbackLoop = std::make_unique<FeedbackLoop>(feedbackLoop);
      state.tailFrames += state.feedbackLoop->tailBudgetSamples();
      state.blockSizeBound = state.feedbackLoop->blockSizeBoundSamples();
    }
  }

  state.downmix = std::make_unique<Downmix>(downmix);
  state.splitValues.resize(state.channels);
  state.midStageValues.resize(state.channels);
  if (state.diffuser != nullptr && state.feedbackLoop != nullptr) {
    state.diffuserOutputValues.resize(state.channels);
  }
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

  // Debug-only: the CLI is the enforcement point for the block-size bound
  // (see main.cpp), rejecting an oversized block size before Reverb is ever
  // constructed. This assert documents and catches a violation of that
  // contract from any other caller, without making processing itself
  // validating in release builds.
  assert(
      state.blockSizeBound == 0 ||
      frameCount <= state.blockSizeBound);

  for (std::size_t frame = 0; frame < frameCount; ++frame) {
    state.split->processFrame(inputs, frame, state.splitValues.data());
    if (state.captureSink != nullptr) {
      state.captureSink->captureFrame(
          StageCaptureBoundary::split,
          0,
          state.splitValues.data(),
          state.channels);
    }

    if (state.diffuser != nullptr && state.feedbackLoop != nullptr) {
      state.diffuser->processFrame(
          state.splitValues.data(),
          state.diffuserOutputValues.data(),
          state.captureSink != nullptr ? &state : nullptr);
      state.feedbackLoop->processFrame(
          state.diffuserOutputValues.data(), state.midStageValues.data());
    } else if (state.diffuser != nullptr) {
      state.diffuser->processFrame(
          state.splitValues.data(),
          state.midStageValues.data(),
          state.captureSink != nullptr ? &state : nullptr);
    } else {
      state.feedbackLoop->processFrame(
          state.splitValues.data(), state.midStageValues.data());
    }

    // A disabled Main wet path skips Downmix processing entirely and
    // contributes exact stereo zero (issue #109), rather than a
    // zero-multiplied value.
    if (state.mainEnabled) {
      state.downmix->processFrame(
          state.midStageValues.data(), outputs, frame);
      outputs[0][frame] *= state.mainGain;
      outputs[1][frame] *= state.mainGain;
    } else {
      outputs[0][frame] = Sample{0};
      outputs[1][frame] = Sample{0};
    }
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
                      ownedVectorBytes(state.midStageValues) +
                      ownedVectorBytes(state.diffuserOutputValues);
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
