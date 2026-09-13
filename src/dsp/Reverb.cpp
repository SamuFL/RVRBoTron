#include "rvrbotron/dsp/Reverb.h"

#include "rvrbotron/dsp/DelayLine.h"
#include "rvrbotron/dsp/Diffuser.h"
#include "rvrbotron/dsp/Downmix.h"
#include "rvrbotron/dsp/EarlyReflections.h"
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
  std::uint64_t blockSizeBound = 0;
  StageCaptureSink* captureSink = nullptr;
  bool mainEnabled = true;
  Sample mainGain{1};
  bool dryEnabled = false;
  Sample dryGain{1};
  Sample wetGain{1};
  std::uint64_t preDelaySamples = 0;
  std::unique_ptr<DelayLine> preDelayLine;

  std::unique_ptr<Split> split;
  std::unique_ptr<Diffuser> diffuser;
  std::unique_ptr<FeedbackLoop> feedbackLoop;
  std::unique_ptr<Downmix> downmix;
  std::unique_ptr<EarlyReflections> early;

  std::vector<Sample> splitValues;
  std::vector<Sample> midStageValues;
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
  state.dryEnabled = !config.composition.wetOnly;
  state.dryGain = static_cast<Sample>(config.composition.dryGain);
  state.wetGain = static_cast<Sample>(config.composition.wetGain);
  state.preDelaySamples = config.composition.preDelaySamples;
  if (state.preDelaySamples > 0) {
    state.preDelayLine = std::make_unique<DelayLine>(
        std::vector<std::uint64_t>(state.inputChannels, state.preDelaySamples),
        std::vector<std::uint64_t>(state.inputChannels, state.preDelaySamples));
  }
  state.split = std::make_unique<Split>(split);

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
  if (config.composition.early.has_value()) {
    state.early =
        std::make_unique<EarlyReflections>(*config.composition.early);
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

  assert(
      state.blockSizeBound == 0 ||
      frameCount <= state.blockSizeBound);

  Sample preDelayedFrame[2]{};
  const Sample* const preDelayedFramePointers[]{
      &preDelayedFrame[0], &preDelayedFrame[1]};

  for (std::size_t frame = 0; frame < frameCount; ++frame) {
    const Sample* const* splitInputs = inputs;
    std::size_t splitFrame = frame;
    if (state.preDelayLine != nullptr) {
      for (std::size_t channel = 0; channel < inputChannelCount; ++channel) {
        preDelayedFrame[channel] = state.preDelayLine->read(channel);
      }
      for (std::size_t channel = 0; channel < inputChannelCount; ++channel) {
        state.preDelayLine->write(channel, inputs[channel][frame]);
      }
      splitInputs = preDelayedFramePointers;
      splitFrame = 0;
    }
    state.split->processFrame(splitInputs, splitFrame, state.splitValues.data());
    if (state.captureSink != nullptr) {
      state.captureSink->captureFrame(
          StageCaptureBoundary::split,
          0,
          state.splitValues.data(),
          state.channels);
    }

    const DiffuserEarlyTap* earlyTaps = nullptr;
    std::size_t earlyTapCount = 0;
    if (state.early != nullptr && state.early->enabled()) {
      state.early->beginFrame();
      earlyTaps = state.early->taps();
      earlyTapCount = state.early->tapCount();
    }

    if (state.diffuser != nullptr && state.feedbackLoop != nullptr) {
      state.diffuser->processFrame(
          state.splitValues.data(),
          state.diffuserOutputValues.data(),
          state.captureSink != nullptr ? &state : nullptr,
          earlyTaps,
          earlyTapCount);
      state.feedbackLoop->processFrame(
          state.diffuserOutputValues.data(), state.midStageValues.data());
    } else if (state.diffuser != nullptr) {
      state.diffuser->processFrame(
          state.splitValues.data(),
          state.midStageValues.data(),
          state.captureSink != nullptr ? &state : nullptr,
          earlyTaps,
          earlyTapCount);
    } else {
      state.feedbackLoop->processFrame(
          state.splitValues.data(), state.midStageValues.data());
    }

    Sample mainLeft{0};
    Sample mainRight{0};
    if (state.mainEnabled) {
      Sample* const mainScratch[]{&mainLeft, &mainRight};
      state.downmix->processFrame(state.midStageValues.data(), mainScratch, 0);
      mainLeft *= state.mainGain;
      mainRight *= state.mainGain;
    }
    if (state.captureSink != nullptr) {
      const Sample mainStereoFrame[]{mainLeft, mainRight};
      state.captureSink->captureFrame(
          StageCaptureBoundary::mainStereo, 0, mainStereoFrame, 2);
    }

    Sample wetLeft{0};
    Sample wetRight{0};
    if (state.early != nullptr) {
      Sample earlyLeft{0};
      Sample earlyRight{0};
      if (state.early->enabled()) {
        state.early->processFrame(&earlyLeft, &earlyRight);
      }
      if (state.captureSink != nullptr) {
        const Sample earlyStereoFrame[]{earlyLeft, earlyRight};
        state.captureSink->captureFrame(
            StageCaptureBoundary::earlyStereo, 0, earlyStereoFrame, 2);
      }
      wetLeft = mainLeft + earlyLeft;
      wetRight = mainRight + earlyRight;
    } else {
      wetLeft = mainLeft;
      wetRight = mainRight;
    }

    if (state.wetGain != Sample{1}) {
      wetLeft *= state.wetGain;
      wetRight *= state.wetGain;
    }

    Sample dryLeft{0};
    Sample dryRight{0};
    if (state.dryEnabled) {
      dryLeft = inputs[0][frame];
      dryRight = inputChannelCount >= 2 ? inputs[1][frame] : inputs[0][frame];
      dryLeft *= state.dryGain;
      dryRight *= state.dryGain;
    }

    if (state.dryEnabled) {
      outputs[0][frame] = dryLeft + wetLeft;
      outputs[1][frame] = dryRight + wetRight;
    } else {
      outputs[0][frame] = wetLeft;
      outputs[1][frame] = wetRight;
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

std::uint64_t Reverb::preDelayFrames() const noexcept {
  return implementation_->preDelaySamples;
}

std::size_t Reverb::ownedBytes() const noexcept {
  const auto& state = *implementation_;
  std::size_t total = sizeof(state) + ownedVectorBytes(state.splitValues) +
                      ownedVectorBytes(state.midStageValues) +
                      ownedVectorBytes(state.diffuserOutputValues);
  if (state.preDelayLine != nullptr) {
    total += sizeof(DelayLine) + state.preDelayLine->ownedStorageBytes();
  }
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
  if (state.early != nullptr) {
    total += state.early->ownedBytes();
  }
  return total;
}

} // namespace rvrbotron::dsp
