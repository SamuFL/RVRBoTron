#include "rvrbotron/dsp/Reverb.h"

#include "rvrbotron/dsp/DelayLine.h"
#include "rvrbotron/dsp/Diffuser.h"
#include "rvrbotron/dsp/Downmix.h"
#include "rvrbotron/dsp/EarlyReflections.h"
#include "rvrbotron/dsp/FeedbackLoop.h"
#include "rvrbotron/dsp/OwnedBytes.h"
#include "rvrbotron/dsp/ProcessChunk.h"
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

  // Frames per chunk: kMaxChunkFrames, below the Block-size bound, or 1
  // while capturing, so captures keep their frame-by-frame order.
  std::size_t chunkFrames = kMaxChunkFrames;
  // One Split frame, then planar chunks (Channel c at values + c * frames):
  // the Diffuser runs in place on splitValues, the Feedback Loop writes
  // midStageValues.
  std::vector<Sample> splitFrameValues;
  std::vector<Sample> splitValues;
  std::vector<Sample> midStageValues;
  std::vector<Sample> mainLeft;
  std::vector<Sample> mainRight;
  std::vector<Sample> earlyLeft;
  std::vector<Sample> earlyRight;

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
  if (state.feedbackLoop != nullptr) {
    // A Feedback Loop chunk reads before it writes, so it must end before
    // the loop's first read of a sample written within the chunk.
    state.chunkFrames = std::clamp<std::uint64_t>(
        state.blockSizeBound > 0 ? state.blockSizeBound - 1 : 1,
        1,
        kMaxChunkFrames);
  }
  // Sized before capturing narrows the chunk, so owned memory does not
  // depend on whether stages are captured.
  const auto bufferFrames = state.chunkFrames;
  if (captureSink != nullptr) {
    state.chunkFrames = 1;
  }
  state.splitFrameValues.resize(state.channels);
  state.splitValues.resize(state.channels * bufferFrames);
  if (state.feedbackLoop != nullptr) {
    state.midStageValues.resize(state.channels * bufferFrames);
  }
  state.mainLeft.resize(bufferFrames);
  state.mainRight.resize(bufferFrames);
  if (config.composition.early.has_value()) {
    state.early =
        std::make_unique<EarlyReflections>(*config.composition.early);
    state.earlyLeft.resize(bufferFrames);
    state.earlyRight.resize(bufferFrames);
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

  for (std::size_t start = 0; start < frameCount; start += state.chunkFrames) {
    const auto frames = std::min(state.chunkFrames, frameCount - start);

    for (std::size_t offset = 0; offset < frames; ++offset) {
      const auto frame = start + offset;
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
      state.split->processFrame(
          splitInputs, splitFrame, state.splitFrameValues.data());
      if (state.captureSink != nullptr) {
        state.captureSink->captureFrame(
            StageCaptureBoundary::split,
            0,
            state.splitFrameValues.data(),
            state.channels);
      }
      for (std::size_t channel = 0; channel < state.channels; ++channel) {
        state.splitValues[channel * frames + offset] =
            state.splitFrameValues[channel];
      }
    }

    const DiffuserEarlyTap* earlyTaps = nullptr;
    std::size_t earlyTapCount = 0;
    if (state.early != nullptr && state.early->enabled()) {
      state.early->beginBlock(frames);
      earlyTaps = state.early->taps();
      earlyTapCount = state.early->tapCount();
    }

    const Sample* midStageValues = state.splitValues.data();
    if (state.diffuser != nullptr) {
      state.diffuser->processBlock(
          state.splitValues.data(),
          frames,
          state.captureSink != nullptr ? &state : nullptr,
          earlyTaps,
          earlyTapCount);
    }
    if (state.feedbackLoop != nullptr) {
      state.feedbackLoop->processBlock(
          state.splitValues.data(), state.midStageValues.data(), frames);
      midStageValues = state.midStageValues.data();
    }

    auto* const mainLeft = state.mainLeft.data();
    auto* const mainRight = state.mainRight.data();
    if (state.mainEnabled) {
      state.downmix->processBlock(midStageValues, frames, mainLeft, mainRight);
      for (std::size_t offset = 0; offset < frames; ++offset) {
        mainLeft[offset] *= state.mainGain;
        mainRight[offset] *= state.mainGain;
      }
    } else {
      std::fill_n(mainLeft, frames, Sample{0});
      std::fill_n(mainRight, frames, Sample{0});
    }
    if (state.captureSink != nullptr) {
      const Sample mainStereoFrame[]{mainLeft[0], mainRight[0]};
      state.captureSink->captureFrame(
          StageCaptureBoundary::mainStereo, 0, mainStereoFrame, 2);
    }

    auto* const earlyLeft = state.earlyLeft.data();
    auto* const earlyRight = state.earlyRight.data();
    if (state.early != nullptr) {
      if (state.early->enabled()) {
        state.early->processBlock(earlyLeft, earlyRight, frames);
      } else {
        std::fill_n(earlyLeft, frames, Sample{0});
        std::fill_n(earlyRight, frames, Sample{0});
      }
      if (state.captureSink != nullptr) {
        const Sample earlyStereoFrame[]{earlyLeft[0], earlyRight[0]};
        state.captureSink->captureFrame(
            StageCaptureBoundary::earlyStereo, 0, earlyStereoFrame, 2);
      }
    }

    for (std::size_t offset = 0; offset < frames; ++offset) {
      const auto frame = start + offset;
      Sample wetLeft = mainLeft[offset];
      Sample wetRight = mainRight[offset];
      if (state.early != nullptr) {
        wetLeft = mainLeft[offset] + earlyLeft[offset];
        wetRight = mainRight[offset] + earlyRight[offset];
      }

      if (state.wetGain != Sample{1}) {
        wetLeft *= state.wetGain;
        wetRight *= state.wetGain;
      }

      if (state.dryEnabled) {
        auto dryLeft = inputs[0][frame];
        auto dryRight =
            inputChannelCount >= 2 ? inputs[1][frame] : inputs[0][frame];
        dryLeft *= state.dryGain;
        dryRight *= state.dryGain;
        outputs[0][frame] = dryLeft + wetLeft;
        outputs[1][frame] = dryRight + wetRight;
      } else {
        outputs[0][frame] = wetLeft;
        outputs[1][frame] = wetRight;
      }
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
  std::size_t total = sizeof(state) + ownedVectorBytes(state.splitFrameValues) +
                      ownedVectorBytes(state.splitValues) +
                      ownedVectorBytes(state.midStageValues) +
                      ownedVectorBytes(state.mainLeft) +
                      ownedVectorBytes(state.mainRight) +
                      ownedVectorBytes(state.earlyLeft) +
                      ownedVectorBytes(state.earlyRight);
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
