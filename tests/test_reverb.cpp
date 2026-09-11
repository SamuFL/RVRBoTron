#include "rvrbotron/HarnessError.h"
#include "rvrbotron/config/ModulationResolution.h"
#include "rvrbotron/config/ResolveConfig.h"
#include "rvrbotron/dsp/DelayLine.h"
#include "rvrbotron/dsp/DiffusionStep.h"
#include "rvrbotron/dsp/Diffuser.h"
#include "rvrbotron/dsp/Downmix.h"
#include "rvrbotron/dsp/EarlyReflections.h"
#include "rvrbotron/dsp/FeedbackLoop.h"
#include "rvrbotron/dsp/MathConstants.h"
#include "rvrbotron/dsp/MixMatrix.h"
#include "rvrbotron/dsp/Modulation.h"
#include "rvrbotron/dsp/Reverb.h"
#include "rvrbotron/dsp/Split.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

bool countAllocations = false;
std::size_t allocationCount = 0;

void recordAllocation() noexcept {
  if (countAllocations) {
    ++allocationCount;
  }
}

void* allocate(const std::size_t size) {
  recordAllocation();
  if (void* memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}

void beginAllocationCount() noexcept {
  allocationCount = 0;
  countAllocations = true;
}

std::size_t endAllocationCount() noexcept {
  countAllocations = false;
  return allocationCount;
}

bool close(const rvrbotron::dsp::Sample actual,
           const double expected) noexcept {
  return std::abs(static_cast<double>(actual) - expected) < 1.0e-6;
}

class FixedCapture final : public rvrbotron::dsp::StageCaptureSink {
public:
  void captureFrame(
      const rvrbotron::dsp::StageCaptureBoundary boundary,
      const std::uint32_t index,
      const rvrbotron::dsp::Sample* channels,
      const std::size_t channelCount) noexcept override {
    // Only Split/Diffusion-Step capture is this test double's own
    // concern; the Main/Early stereo captures Reverb also emits
    // (issue #113) are out of scope here and must not be misrouted into
    // the "diffusion" bucket below.
    if (boundary != rvrbotron::dsp::StageCaptureBoundary::split &&
        boundary != rvrbotron::dsp::StageCaptureBoundary::diffusionStep) {
      return;
    }
    if (index != 0 || channelCount != 2) {
      valid = false;
      return;
    }
    auto& destination =
        boundary == rvrbotron::dsp::StageCaptureBoundary::split
            ? split
            : diffusion;
    auto& frame =
        boundary == rvrbotron::dsp::StageCaptureBoundary::split
            ? splitFrames
            : diffusionFrames;
    if (frame >= 2) {
      valid = false;
      return;
    }
    destination[frame * 2] = channels[0];
    destination[frame * 2 + 1] = channels[1];
    ++frame;
  }

  bool valid{true};
  std::size_t splitFrames{0};
  std::size_t diffusionFrames{0};
  std::array<rvrbotron::dsp::Sample, 4> split{};
  std::array<rvrbotron::dsp::Sample, 4> diffusion{};
};

class EnergyCapture final : public rvrbotron::dsp::StageCaptureSink {
public:
  explicit EnergyCapture(const std::size_t channels)
      : channels_(channels) {}

  void captureFrame(
      const rvrbotron::dsp::StageCaptureBoundary boundary,
      const std::uint32_t index,
      const rvrbotron::dsp::Sample* channels,
      const std::size_t channelCount) noexcept override {
    // Only Split/Diffusion-Step capture is this test double's own
    // concern; see FixedCapture's own identical guard above.
    if (boundary != rvrbotron::dsp::StageCaptureBoundary::split &&
        boundary != rvrbotron::dsp::StageCaptureBoundary::diffusionStep) {
      return;
    }
    if (index != 0 || channelCount != channels_) {
      valid = false;
      return;
    }
    auto& energy =
        boundary == rvrbotron::dsp::StageCaptureBoundary::split
            ? splitEnergy
            : diffusionEnergy;
    auto& frames =
        boundary == rvrbotron::dsp::StageCaptureBoundary::split
            ? splitFrames
            : diffusionFrames;
    for (std::size_t channel = 0; channel < channelCount; ++channel) {
      const auto value = static_cast<double>(channels[channel]);
      energy += value * value;
    }
    ++frames;
  }

  bool valid{true};
  std::size_t splitFrames{0};
  std::size_t diffusionFrames{0};
  double splitEnergy{0.0};
  double diffusionEnergy{0.0};

private:
  std::size_t channels_;
};

// Records every Main-stereo/Early-stereo capture (issue #113), one frame
// at a time, ignoring Split/Diffusion-Step boundaries entirely -- the
// opposite scoping of FixedCapture/EnergyCapture above.
class BranchCapture final : public rvrbotron::dsp::StageCaptureSink {
public:
  void captureFrame(
      const rvrbotron::dsp::StageCaptureBoundary boundary,
      const std::uint32_t index,
      const rvrbotron::dsp::Sample* channels,
      const std::size_t channelCount) noexcept override {
    if (boundary != rvrbotron::dsp::StageCaptureBoundary::mainStereo &&
        boundary != rvrbotron::dsp::StageCaptureBoundary::earlyStereo) {
      return;
    }
    if (index != 0 || channelCount != 2) {
      valid = false;
      return;
    }
    auto& left = boundary == rvrbotron::dsp::StageCaptureBoundary::mainStereo
        ? mainLeft
        : earlyLeft;
    auto& right = boundary == rvrbotron::dsp::StageCaptureBoundary::mainStereo
        ? mainRight
        : earlyRight;
    left.push_back(channels[0]);
    right.push_back(channels[1]);
  }

  bool valid{true};
  std::vector<rvrbotron::dsp::Sample> mainLeft;
  std::vector<rvrbotron::dsp::Sample> mainRight;
  std::vector<rvrbotron::dsp::Sample> earlyLeft;
  std::vector<rvrbotron::dsp::Sample> earlyRight;
};

class OrderedDiffuserCapture final
    : public rvrbotron::dsp::DiffuserCaptureSink {
public:
  void captureDiffusionStepFrame(
      const std::uint32_t index,
      const rvrbotron::dsp::Sample* channels,
      const std::size_t channelCount) noexcept override {
    if (index != frames || channelCount != 1 || frames >= values.size()) {
      valid = false;
      return;
    }
    values[frames] = channels[0];
    ++frames;
  }

  bool valid{true};
  std::size_t frames{0};
  std::array<rvrbotron::dsp::Sample, 2> values{};
};

rvrbotron::dsp::ResolvedConfig twoChannelDiffusionConfig() {
  constexpr double scale = 0.70710678118654752440;
  rvrbotron::dsp::ResolvedDiffusionStep step;
  step.index = 0;
  step.lengthSamples = 1;
  step.lengthMs = 1.0;
  step.delaysSamples = {0, 1};
  step.delaysMs = {0.0, 1.0};
  step.bufferSizes = {0, 1};
  step.permutation = {1, 0};
  step.polaritySigns = {1, -1};
  step.matrix = {scale, scale, scale, -scale};

  rvrbotron::dsp::ResolvedDiffuser diffuser;
  diffuser.totalSamples = 1;
  diffuser.steps.push_back(std::move(step));

  rvrbotron::dsp::ResolvedConfig config;
  config.sampleRate = 1000;
  config.composition.stages.emplace_back(
      rvrbotron::dsp::ResolvedSplit{
          1,
          2,
          rvrbotron::dsp::SplitStrategyType::duplicate,
          rvrbotron::dsp::EnergyNormalisation::energy,
          1.0,
          scale,
      });
  config.composition.stages.emplace_back(std::move(diffuser));
  config.composition.stages.emplace_back(
      rvrbotron::dsp::ResolvedDownmix{
          2,
          2,
          rvrbotron::dsp::DownmixStrategy::select,
          0,
          1,
          rvrbotron::dsp::EnergyNormalisation::energy,
          1.0,
          {1.0, 0.0},
          {0.0, 1.0},
          {1.0, 0.0},
          {0.0, 1.0},
          rvrbotron::dsp::DownmixAlignment::aligned,
          90.0,
          {1.0, 0.0, 0.0, 1.0},
          false,
      });
  return config;
}

rvrbotron::dsp::ResolvedConfig twoChannelAblationConfig() {
  auto config = twoChannelDiffusionConfig();
  auto& split =
      std::get<rvrbotron::dsp::ResolvedSplit>(
          config.composition.stages[0]);
  split.normalisation =
      rvrbotron::dsp::EnergyNormalisation::none;
  split.channelGain = 1.0;

  auto& step =
      std::get<rvrbotron::dsp::ResolvedDiffuser>(
          config.composition.stages[1])
          .steps.front();
  step.delayStrategy = rvrbotron::dsp::DelayStrategy::even;
  step.shuffle = false;
  step.permutation = {0, 1};
  step.polarity = rvrbotron::dsp::PolarityStrategy::none;
  step.polaritySigns = {1, 1};

  auto& downmix =
      std::get<rvrbotron::dsp::ResolvedDownmix>(
          config.composition.stages[2]);
  downmix.normalisation =
      rvrbotron::dsp::EnergyNormalisation::none;
  downmix.compensation = 1.0;
  return config;
}

std::uint64_t nextInputBits(std::uint64_t& state) noexcept {
  state ^= state >> 12U;
  state ^= state << 25U;
  state ^= state >> 27U;
  return state * 0x2545f4914f6cdd1dULL;
}

rvrbotron::dsp::Sample nextInputSample(
    std::uint64_t& state) noexcept {
  const auto value =
      static_cast<int>((nextInputBits(state) >> 32U) % 2001U) - 1000;
  return static_cast<rvrbotron::dsp::Sample>(
      static_cast<double>(value) / 1000.0);
}

// Selects Channel 0 (and Channel 1, when there is one) so every helper
// below keeps rendering true stereo instead of falling back to this
// select Downmix's mono-duplication-on-omission default (issue #107).
rvrbotron::config::DownmixConfig referenceSelectDownmixConfig(
    const std::uint32_t channels) {
  rvrbotron::config::DownmixConfig downmix;
  downmix.strategy = rvrbotron::dsp::DownmixStrategy::select;
  downmix.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;
  downmix.leftChannel = 0;
  if (channels > 1) {
    downmix.rightChannel = 1;
  }
  return downmix;
}

// An independent re-derivation of resolveWidthMatrix (issue #109,
// docs/design/reverb/stages/08-downmix.md's "Width as a constant-power
// mid/side law"), shared by every Width test below so the trig formula
// is written once rather than copy-pasted at each call site.
std::array<double, 4> expectedWidthMatrix(const double widthDeg) {
  const auto half = 1.0 / std::sqrt(2.0);
  if (widthDeg == 0.0) {
    return {half, half, half, half};
  }
  if (widthDeg == 90.0) {
    return {1.0, 0.0, 0.0, 1.0};
  }
  if (widthDeg == 180.0) {
    return {half, -half, -half, half};
  }
  const auto halfAngleRad = widthDeg * (rvrbotron::dsp::kPi / 180.0) / 2.0;
  const auto cosHalf = std::cos(halfAngleRad);
  const auto sinHalf = std::sin(halfAngleRad);
  const auto a = (cosHalf + sinHalf) * half;
  const auto b = (cosHalf - sinHalf) * half;
  return {a, b, b, a};
}

rvrbotron::dsp::ResolvedConfig resolvedDiffusionConfig(
    const std::uint32_t channels,
    const rvrbotron::dsp::MixMatrixType mix =
        rvrbotron::dsp::MixMatrixType::hadamard) {
  rvrbotron::config::SplitConfig split;
  split.channels = channels;
  split.strategy = rvrbotron::dsp::SplitStrategyType::duplicate;
  split.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;

  rvrbotron::config::DiffusionStepConfig step;
  step.delayStrategy = rvrbotron::dsp::DelayStrategy::segmentedRandom;
  step.mix = mix;
  step.shuffle = true;
  step.polarity = rvrbotron::dsp::PolarityStrategy::seededRandom;

  rvrbotron::config::DiffuserConfig diffuser;
  diffuser.steps = 1;
  diffuser.totalMs = 2.0;
  diffuser.distribution = rvrbotron::config::DiffusionDistribution::even;
  diffuser.step = step;

  auto downmix = referenceSelectDownmixConfig(channels);

  rvrbotron::config::CompositionConfig composition;
  composition.stagesSpecified = true;
  composition.stages.emplace_back(split);
  composition.stages.emplace_back(diffuser);
  composition.stages.emplace_back(downmix);

  rvrbotron::config::ReverbConfig requested;
  requested.formatVersion = 2;
  requested.seed = 0xd1b54a32d192ed03ULL;
  requested.composition = std::move(composition);
  return rvrbotron::config::resolveConfig(requested, 48000, 1);
}

rvrbotron::dsp::ResolvedConfig resolvedFeedbackLoopConfig(
    const std::uint32_t channels,
    const double rt60Sec,
    const double delayMinMs,
    const double delayMaxMs,
    const rvrbotron::dsp::MixMatrixType mix =
        rvrbotron::dsp::MixMatrixType::householder,
    const rvrbotron::dsp::DelayStrategy delayStrategy =
        rvrbotron::dsp::DelayStrategy::even,
    const std::uint32_t sampleRate = 48000,
    const rvrbotron::dsp::GainMode gainMode =
        rvrbotron::dsp::GainMode::perChannel) {
  rvrbotron::config::SplitConfig split;
  split.channels = channels;
  split.strategy = rvrbotron::dsp::SplitStrategyType::duplicate;
  split.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;

  rvrbotron::config::FeedbackLoopConfig loop;
  loop.delayMinMs = delayMinMs;
  loop.delayMaxMs = delayMaxMs;
  loop.delayStrategy = delayStrategy;
  loop.rt60Sec = rt60Sec;
  loop.mix = mix;
  loop.gainMode = gainMode;

  auto downmix = referenceSelectDownmixConfig(channels);

  rvrbotron::config::CompositionConfig composition;
  composition.stagesSpecified = true;
  composition.stages.emplace_back(split);
  composition.stages.emplace_back(loop);
  composition.stages.emplace_back(downmix);

  rvrbotron::config::ReverbConfig requested;
  requested.formatVersion = 2;
  requested.seed = 0x9e3779b97f4a7c15ULL;
  requested.composition = std::move(composition);
  return rvrbotron::config::resolveConfig(requested, sampleRate, 1);
}

// Same shape as resolvedFeedbackLoopConfig above, with an optional
// Modulation object attached to the Feedback Loop (issue #89).
// `modulation` is nullopt when the caller wants Modulation omitted
// entirely, as distinct from an included object at zero depth.
rvrbotron::dsp::ResolvedConfig resolvedModulatedLoopConfig(
    const std::uint32_t channels,
    const double rt60Sec,
    const double delayMinMs,
    const double delayMaxMs,
    const std::optional<rvrbotron::config::ModulationConfig>& modulation,
    const rvrbotron::dsp::DelayStrategy delayStrategy =
        rvrbotron::dsp::DelayStrategy::even,
    const std::uint32_t sampleRate = 48000,
    const std::uint64_t seed = 0x9e3779b97f4a7c15ULL) {
  rvrbotron::config::SplitConfig split;
  split.channels = channels;
  split.strategy = rvrbotron::dsp::SplitStrategyType::duplicate;
  split.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;

  rvrbotron::config::FeedbackLoopConfig loop;
  loop.delayMinMs = delayMinMs;
  loop.delayMaxMs = delayMaxMs;
  loop.delayStrategy = delayStrategy;
  loop.rt60Sec = rt60Sec;
  loop.mix = rvrbotron::dsp::MixMatrixType::householder;
  loop.gainMode = rvrbotron::dsp::GainMode::perChannel;
  loop.modulation = modulation;

  auto downmix = referenceSelectDownmixConfig(channels);

  rvrbotron::config::CompositionConfig composition;
  composition.stagesSpecified = true;
  composition.stages.emplace_back(split);
  composition.stages.emplace_back(loop);
  composition.stages.emplace_back(downmix);

  rvrbotron::config::ReverbConfig requested;
  requested.formatVersion = 2;
  requested.seed = seed;
  requested.composition = std::move(composition);
  return rvrbotron::config::resolveConfig(requested, sampleRate, 1);
}

// A Diffuser-only Composition (no Feedback Loop) whose shared step
// defaults and/or a single step override may each carry their own
// Modulation object (issue #91). `stepDefaultsModulation` is nullopt when
// every step's Modulation should come solely from `stepOverride` (or be
// omitted entirely, if that is nullopt too); `stepOverride`, when
// present, is (step index, that step's own Modulation).
rvrbotron::dsp::ResolvedConfig resolvedDiffuserStepModulatedConfig(
    const std::uint32_t channels,
    const double totalMs,
    const std::uint32_t stepCount,
    const std::optional<rvrbotron::config::ModulationConfig>&
        stepDefaultsModulation,
    const std::optional<
        std::pair<std::uint32_t, rvrbotron::config::ModulationConfig>>&
        stepOverride,
    const std::uint64_t seed = 0x9e3779b97f4a7c15ULL) {
  rvrbotron::config::SplitConfig split;
  split.channels = channels;
  split.strategy = rvrbotron::dsp::SplitStrategyType::duplicate;
  split.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;

  rvrbotron::config::DiffusionStepConfig step;
  step.delayStrategy = rvrbotron::dsp::DelayStrategy::segmentedRandom;
  step.mix = rvrbotron::dsp::MixMatrixType::hadamard;
  step.shuffle = true;
  step.polarity = rvrbotron::dsp::PolarityStrategy::seededRandom;
  step.modulation = stepDefaultsModulation;

  rvrbotron::config::DiffuserConfig diffuser;
  diffuser.steps = stepCount;
  diffuser.totalMs = totalMs;
  diffuser.distribution = rvrbotron::config::DiffusionDistribution::even;
  diffuser.step = step;
  if (stepOverride.has_value()) {
    rvrbotron::config::DiffusionStepOverride override;
    override.index = stepOverride->first;
    override.step.modulation = stepOverride->second;
    diffuser.stepOverrides =
        std::vector<rvrbotron::config::DiffusionStepOverride>{override};
  }

  auto downmix = referenceSelectDownmixConfig(channels);

  rvrbotron::config::CompositionConfig composition;
  composition.stagesSpecified = true;
  composition.stages.emplace_back(split);
  composition.stages.emplace_back(diffuser);
  composition.stages.emplace_back(downmix);

  rvrbotron::config::ReverbConfig requested;
  requested.formatVersion = 2;
  requested.seed = seed;
  requested.composition = std::move(composition);
  return rvrbotron::config::resolveConfig(requested, 48000, 1);
}

// Same seed as resolvedFeedbackLoopConfig above, and the same Feedback Loop
// parameters, but with a Diffuser placed in front -- so a direct comparison
// between the two configs' resolved Feedback Loop stages proves loop time
// is unaffected by the Diffuser (#55).
rvrbotron::dsp::ResolvedConfig resolvedDiffuserThenLoopConfig(
    const std::uint32_t channels,
    const double rt60Sec,
    const double delayMinMs,
    const double delayMaxMs,
    const rvrbotron::dsp::MixMatrixType mix =
        rvrbotron::dsp::MixMatrixType::householder,
    const rvrbotron::dsp::DelayStrategy delayStrategy =
        rvrbotron::dsp::DelayStrategy::even,
    const std::uint32_t sampleRate = 48000) {
  rvrbotron::config::SplitConfig split;
  split.channels = channels;
  split.strategy = rvrbotron::dsp::SplitStrategyType::duplicate;
  split.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;

  rvrbotron::config::DiffusionStepConfig step;
  step.delayStrategy = rvrbotron::dsp::DelayStrategy::segmentedRandom;
  step.mix = rvrbotron::dsp::MixMatrixType::hadamard;
  step.shuffle = true;
  step.polarity = rvrbotron::dsp::PolarityStrategy::seededRandom;

  rvrbotron::config::DiffuserConfig diffuser;
  diffuser.steps = 2;
  diffuser.totalMs = 2.0;
  diffuser.distribution = rvrbotron::config::DiffusionDistribution::even;
  diffuser.step = step;

  rvrbotron::config::FeedbackLoopConfig loop;
  loop.delayMinMs = delayMinMs;
  loop.delayMaxMs = delayMaxMs;
  loop.delayStrategy = delayStrategy;
  loop.rt60Sec = rt60Sec;
  loop.mix = mix;

  auto downmix = referenceSelectDownmixConfig(channels);

  rvrbotron::config::CompositionConfig composition;
  composition.stagesSpecified = true;
  composition.stages.emplace_back(split);
  composition.stages.emplace_back(diffuser);
  composition.stages.emplace_back(loop);
  composition.stages.emplace_back(downmix);

  rvrbotron::config::ReverbConfig requested;
  requested.formatVersion = 2;
  requested.seed = 0x9e3779b97f4a7c15ULL;
  requested.composition = std::move(composition);
  return rvrbotron::config::resolveConfig(requested, sampleRate, 1);
}

// Processes a whole mono-in/stereo-out render in chunks no larger than
// blockSize, so a Feedback Loop fixture with a short resolved delay can
// still be exercised over many frames without violating the block-size
// bound (#53): a single oversized process() call would trip its debug
// assert. input/left/right must already be sized to the same frameCount.
void processMonoToStereoInChunks(
    rvrbotron::dsp::Reverb& reverb,
    const std::vector<rvrbotron::dsp::Sample>& input,
    std::vector<rvrbotron::dsp::Sample>& left,
    std::vector<rvrbotron::dsp::Sample>& right,
    const std::size_t blockSize) {
  if (blockSize == 0) {
    throw std::invalid_argument("blockSize must be positive");
  }
  const auto frameCount = input.size();
  for (std::size_t offset = 0; offset < frameCount; offset += blockSize) {
    const auto frames = std::min(blockSize, frameCount - offset);
    const rvrbotron::dsp::Sample* offsetInputs[]{input.data() + offset};
    rvrbotron::dsp::Sample* offsetOutputs[]{
        left.data() + offset, right.data() + offset};
    reverb.process(offsetInputs, 1, offsetOutputs, 2, frames);
  }
}

bool reverbDiffusionStepIsAllPass(
    const std::uint32_t channels,
    const rvrbotron::dsp::MixMatrixType mix =
        rvrbotron::dsp::MixMatrixType::hadamard) {
  constexpr std::size_t inputFrames = 257;
  const auto config = resolvedDiffusionConfig(channels, mix);
  EnergyCapture capture(channels);
  rvrbotron::dsp::Reverb reverb(config, &capture);
  const auto responseFrames =
      inputFrames + static_cast<std::size_t>(reverb.tailBudgetFrames());
  std::vector<rvrbotron::dsp::Sample> input(
      responseFrames, rvrbotron::dsp::Sample{0});
  std::vector<rvrbotron::dsp::Sample> left(responseFrames);
  std::vector<rvrbotron::dsp::Sample> right(responseFrames);
  std::uint64_t randomState = 0x713b2c9d845e6fa1ULL ^ channels;
  double inputEnergy = 0.0;
  for (std::size_t frame = 0; frame < inputFrames; ++frame) {
    input[frame] = nextInputSample(randomState);
    const auto value = static_cast<double>(input[frame]);
    inputEnergy += value * value;
  }

  const rvrbotron::dsp::Sample* inputs[]{input.data()};
  rvrbotron::dsp::Sample* outputs[]{left.data(), right.data()};
  beginAllocationCount();
  reverb.process(inputs, 1, outputs, 2, responseFrames);
  const auto allocations = endAllocationCount();
  if (allocations != 0) {
    std::cerr << "Reverb allocated while processing "
              << channels << " Channels\n";
    return false;
  }
  if (!capture.valid || capture.splitFrames != responseFrames ||
      capture.diffusionFrames != responseFrames) {
    return false;
  }

  const auto relativeTolerance =
      sizeof(rvrbotron::dsp::Sample) == sizeof(float)
          ? 2.0e-5
          : 1.0e-12;
  return std::abs(capture.splitEnergy - inputEnergy) <=
             relativeTolerance * inputEnergy &&
         std::abs(capture.diffusionEnergy - inputEnergy) <=
             relativeTolerance * inputEnergy;
}

} // namespace

void* operator new(const std::size_t size) {
  return allocate(size);
}

void* operator new[](const std::size_t size) {
  return allocate(size);
}

void operator delete(void* memory) noexcept {
  std::free(memory);
}

void operator delete[](void* memory) noexcept {
  std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
  std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
  std::free(memory);
}

int main() {
  beginAllocationCount();
  void* observedAllocation = ::operator new(1);
  ::operator delete(observedAllocation);
  if (endAllocationCount() != 1) {
    std::cerr << "test allocation counter did not observe operator new\n";
    return 1;
  }

  rvrbotron::dsp::ResolvedDiffusionStep fwhtConfig;
  fwhtConfig.delaysSamples = {0, 0, 0, 0};
  fwhtConfig.bufferSizes = {0, 0, 0, 0};
  fwhtConfig.permutation = {0, 1, 2, 3};
  fwhtConfig.polaritySigns = {1, 1, 1, 1};
  fwhtConfig.matrix = {
      0.5, 0.5, 0.5, 0.5,
      0.5, -0.5, 0.5, -0.5,
      0.5, 0.5, -0.5, -0.5,
      0.5, -0.5, -0.5, 0.5,
  };
  rvrbotron::dsp::DiffusionStep fwht(fwhtConfig);
  const std::array<rvrbotron::dsp::Sample, 4> fwhtInput{
      1.0F, 2.0F, 3.0F, 4.0F};
  std::array<rvrbotron::dsp::Sample, 4> fwhtOutput{};
  beginAllocationCount();
  fwht.processFrame(fwhtInput.data(), fwhtOutput.data());
  const auto fwhtAllocations = endAllocationCount();
  if (fwhtAllocations != 0) {
    std::cerr << "fast Walsh-Hadamard transform allocated while processing\n";
    return 1;
  }
  const std::array<rvrbotron::dsp::Sample, 4> expectedFwht{
      5.0F, -1.0F, -2.0F, 0.0F};
  if (fwhtOutput != expectedFwht) {
    std::cerr << "fast Walsh-Hadamard transform was not normalized\n";
    return 1;
  }
  std::array<rvrbotron::dsp::Sample, 4> fwhtRoundTrip{};
  fwht.processFrame(fwhtOutput.data(), fwhtRoundTrip.data());
  if (fwhtRoundTrip != fwhtInput) {
    std::cerr << "normalized Hadamard transform was not involutory\n";
    return 1;
  }

  // Householder reflection off the all-ones vector, N=4: diagonal 0.5,
  // off-diagonal -0.5. Mixed via the O(N) closed form rather than a dense
  // multiply, so this exercises that formula directly.
  const std::vector<double> householderMatrix4{
      0.5,  -0.5, -0.5, -0.5, -0.5, 0.5,  -0.5, -0.5,
      -0.5, -0.5, 0.5,  -0.5, -0.5, -0.5, -0.5, 0.5,
  };
  rvrbotron::dsp::HouseholderMixMatrix householder(4, householderMatrix4);
  std::array<rvrbotron::dsp::Sample, 4> householderChannels{
      1.0F, 2.0F, 3.0F, 4.0F};
  beginAllocationCount();
  householder.mix(householderChannels.data());
  const auto householderAllocations = endAllocationCount();
  if (householderAllocations != 0) {
    std::cerr << "Householder MixMatrix allocated while mixing\n";
    return 1;
  }
  const std::array<rvrbotron::dsp::Sample, 4> expectedHouseholder{
      -4.0F, -3.0F, -2.0F, -1.0F};
  if (householderChannels != expectedHouseholder) {
    std::cerr << "Householder MixMatrix did not subtract twice the mean\n";
    return 1;
  }
  householder.mix(householderChannels.data());
  if (householderChannels !=
      std::array<rvrbotron::dsp::Sample, 4>{1.0F, 2.0F, 3.0F, 4.0F}) {
    std::cerr << "Householder MixMatrix was not involutory\n";
    return 1;
  }

  auto invalidHouseholderMatrix = householderMatrix4;
  invalidHouseholderMatrix[1] = -0.25;
  try {
    rvrbotron::dsp::HouseholderMixMatrix invalidHouseholder(
        4, invalidHouseholderMatrix);
    std::cerr
        << "Householder MixMatrix accepted a non-canonical coefficient\n";
    return 1;
  } catch (const std::invalid_argument&) {
  }

  // A known orthogonal 2x2 matrix supplied directly, independent of the
  // Householder QR construction: RandomOrthogonalMixMatrix trusts the
  // resolved coefficients' MM^T = I property, not their provenance.
  const std::vector<double> rotationMatrix2{0.0, 1.0, -1.0, 0.0};
  rvrbotron::dsp::RandomOrthogonalMixMatrix rotation(2, rotationMatrix2);
  std::array<rvrbotron::dsp::Sample, 2> rotationChannels{1.0F, 0.0F};
  beginAllocationCount();
  rotation.mix(rotationChannels.data());
  const auto rotationAllocations = endAllocationCount();
  if (rotationAllocations != 0) {
    std::cerr << "RandomOrthogonal MixMatrix allocated while mixing\n";
    return 1;
  }
  if (rotationChannels != std::array<rvrbotron::dsp::Sample, 2>{0.0F, -1.0F}) {
    std::cerr << "RandomOrthogonal MixMatrix did not apply its resolved "
                 "coefficients\n";
    return 1;
  }

  const std::vector<double> nonOrthogonalMatrix2{1.0, 1.0, 0.0, 1.0};
  try {
    rvrbotron::dsp::RandomOrthogonalMixMatrix invalidRotation(
        2, nonOrthogonalMatrix2);
    std::cerr
        << "RandomOrthogonal MixMatrix accepted non-orthogonal coefficients\n";
    return 1;
  } catch (const std::invalid_argument&) {
  }

  auto invalidMatrixConfig = fwhtConfig;
  invalidMatrixConfig.matrix[1] = -0.5;
  try {
    rvrbotron::dsp::DiffusionStep invalidMatrix(invalidMatrixConfig);
    std::cerr << "Diffusion Step accepted invalid resolved matrix evidence\n";
    return 1;
  } catch (const std::invalid_argument&) {
  }

  auto insufficientBufferConfig = fwhtConfig;
  insufficientBufferConfig.delaysSamples[0] = 1;
  try {
    rvrbotron::dsp::DiffusionStep insufficientBuffer(
        insufficientBufferConfig);
    std::cerr << "Diffusion Step accepted an undersized resolved buffer\n";
    return 1;
  } catch (const std::invalid_argument&) {
  }

  rvrbotron::dsp::ResolvedDiffusionStep firstOrderedStep;
  firstOrderedStep.index = 0;
  firstOrderedStep.delaysSamples = {0};
  firstOrderedStep.bufferSizes = {0};
  firstOrderedStep.permutation = {0};
  firstOrderedStep.polaritySigns = {-1};
  firstOrderedStep.matrix = {1.0};
  auto secondOrderedStep = firstOrderedStep;
  secondOrderedStep.index = 1;
  rvrbotron::dsp::ResolvedDiffuser orderedConfig;
  orderedConfig.steps.push_back(std::move(firstOrderedStep));
  orderedConfig.steps.push_back(std::move(secondOrderedStep));
  rvrbotron::dsp::Diffuser orderedDiffuser(orderedConfig);
  const std::array<rvrbotron::dsp::Sample, 1> orderedInput{0.25F};
  std::array<rvrbotron::dsp::Sample, 1> orderedOutput{};
  OrderedDiffuserCapture orderedCapture;
  beginAllocationCount();
  orderedDiffuser.processFrame(
      orderedInput.data(), orderedOutput.data(), &orderedCapture);
  const auto orderedAllocations = endAllocationCount();
  if (orderedAllocations != 0) {
    std::cerr << "Diffuser allocated while processing ordered steps\n";
    return 1;
  }
  if (orderedDiffuser.channelCount() != 1 ||
      orderedDiffuser.stepCount() != 2 ||
      !orderedCapture.valid ||
      orderedCapture.frames != 2 ||
      !close(orderedCapture.values[0], -0.25) ||
      !close(orderedCapture.values[1], 0.25) ||
      !close(orderedOutput[0], 0.25)) {
    std::cerr << "Diffuser did not process its resolved ordered steps\n";
    return 1;
  }

  // The Early Reflections tap seam (issue #111, docs/design/reverb/
  // stages/07-early-reflections.md's "What this forces on the
  // architecture"): a caller-owned accumulator, not a registered
  // observer. Diffuser adds (+=) the configured step's completed
  // post-step frame into it, leaves the Diffuser's own Main output
  // bit-identical, and allocates nothing.
  {
    rvrbotron::dsp::Diffuser tapStep0Diffuser(orderedConfig);
    std::array<rvrbotron::dsp::Sample, 1> tapAccumulator{
        rvrbotron::dsp::Sample{0}};
    const rvrbotron::dsp::DiffuserEarlyTap tapStep0{
        0, rvrbotron::dsp::Sample{1}, tapAccumulator.data()};
    std::array<rvrbotron::dsp::Sample, 1> tapOutput{};
    beginAllocationCount();
    tapStep0Diffuser.processFrame(
        orderedInput.data(), tapOutput.data(), nullptr, &tapStep0, 1);
    const auto tapAllocations = endAllocationCount();
    if (tapAllocations != 0) {
      std::cerr << "Diffuser allocated while accumulating an Early tap\n";
      return 1;
    }
    if (!close(tapAccumulator[0], -0.25)) {
      std::cerr << "Diffuser's Early tap did not accumulate the configured "
                   "step's completed post-step frame\n";
      return 1;
    }
    if (tapOutput[0] != orderedOutput[0]) {
      std::cerr << "configuring an Early tap changed the Diffuser's own "
                   "Main output\n";
      return 1;
    }

    // Accumulation is additive, not overwriting: a second call without
    // resetting the accumulator sums both calls' contributions -- the
    // caller (EarlyReflections) owns clearing it once per frame.
    tapStep0Diffuser.processFrame(
        orderedInput.data(), tapOutput.data(), nullptr, &tapStep0, 1);
    if (!close(tapAccumulator[0], -0.5)) {
      std::cerr << "Diffuser's Early tap did not accumulate additively "
                   "across repeated calls\n";
      return 1;
    }

    // Tapping a different step accumulates that step's own completed
    // frame instead.
    rvrbotron::dsp::Diffuser tapStep1Diffuser(orderedConfig);
    std::array<rvrbotron::dsp::Sample, 1> tapStep1Accumulator{
        rvrbotron::dsp::Sample{0}};
    const rvrbotron::dsp::DiffuserEarlyTap tapStep1{
        1, rvrbotron::dsp::Sample{1}, tapStep1Accumulator.data()};
    std::array<rvrbotron::dsp::Sample, 1> tapStep1Output{};
    tapStep1Diffuser.processFrame(
        orderedInput.data(), tapStep1Output.data(), nullptr, &tapStep1, 1);
    if (!close(tapStep1Accumulator[0], 0.25)) {
      std::cerr << "Diffuser's Early tap did not accumulate the configured "
                   "step's own completed post-step frame\n";
      return 1;
    }

    // Multiple taps share one accumulator (issue #112): each tap's own
    // gain scales its own step's completed post-step frame before
    // summing, and the two-pointer match against the per-step loop
    // correctly skips an untapped step in between.
    auto stepC = orderedConfig.steps[1];
    stepC.index = 2;
    rvrbotron::dsp::ResolvedDiffuser multiTapConfig;
    multiTapConfig.steps = {
        orderedConfig.steps[0], orderedConfig.steps[1], stepC};
    rvrbotron::dsp::Diffuser multiTapDiffuser(multiTapConfig);
    std::array<rvrbotron::dsp::Sample, 1> multiTapAccumulator{
        rvrbotron::dsp::Sample{0}};
    const std::array<rvrbotron::dsp::DiffuserEarlyTap, 2> multiTaps{
        {{0, rvrbotron::dsp::Sample{2}, multiTapAccumulator.data()},
         {2, rvrbotron::dsp::Sample{0.5}, multiTapAccumulator.data()}}};
    std::array<rvrbotron::dsp::Sample, 1> multiTapOutput{};
    multiTapDiffuser.processFrame(
        orderedInput.data(),
        multiTapOutput.data(),
        nullptr,
        multiTaps.data(),
        multiTaps.size());
    // Per-step completed frames are -0.25, 0.25, -0.25 (each step negates
    // in turn). Tap@0 contributes -0.25*2 = -0.5; tap@2 contributes
    // -0.25*0.5 = -0.125; step 1 is untapped.
    if (!close(multiTapAccumulator[0], -0.625)) {
      std::cerr << "Diffuser did not accumulate multiple gained taps into "
                   "one shared accumulator\n";
      return 1;
    }
  }

  constexpr double splitScale = 0.70710678118654752440;
  const std::array<rvrbotron::dsp::Sample, 1> stereoLeft{0.75F};
  const std::array<rvrbotron::dsp::Sample, 1> stereoRight{-0.25F};
  const rvrbotron::dsp::Sample* stereoInputs[]{
      stereoLeft.data(), stereoRight.data()};
  std::array<rvrbotron::dsp::Sample, 2> splitChannels{};
  rvrbotron::dsp::Split stereoEnergy(
      {
          2,
          2,
          rvrbotron::dsp::SplitStrategyType::duplicate,
          rvrbotron::dsp::EnergyNormalisation::energy,
          splitScale,
          splitScale,
      });
  beginAllocationCount();
  stereoEnergy.processFrame(stereoInputs, 0, splitChannels.data());
  const auto splitAllocations = endAllocationCount();
  if (splitAllocations != 0) {
    std::cerr << "SplitStrategy allocated while processing\n";
    return 1;
  }
  if (!close(splitChannels[0], 0.25) ||
      !close(splitChannels[1], 0.25)) {
    std::cerr << "energy-normalized stereo duplicate Split is incorrect\n";
    return 1;
  }

  rvrbotron::dsp::Split stereoNone(
      {
          2,
          2,
          rvrbotron::dsp::SplitStrategyType::duplicate,
          rvrbotron::dsp::EnergyNormalisation::none,
          splitScale,
          1.0,
      });
  stereoNone.processFrame(stereoInputs, 0, splitChannels.data());
  if (!close(splitChannels[0], 0.5 * splitScale) ||
      !close(splitChannels[1], 0.5 * splitScale)) {
    std::cerr << "unnormalized stereo duplicate Split is incorrect\n";
    return 1;
  }

  constexpr double halvesScale = 0.70710678118654752440; // sqrt(2/4)
  std::array<rvrbotron::dsp::Sample, 4> fourChannels{};
  rvrbotron::dsp::Split stereoHalves(
      {
          2,
          4,
          rvrbotron::dsp::SplitStrategyType::stereoHalves,
          rvrbotron::dsp::EnergyNormalisation::energy,
          1.0,
          halvesScale,
      });
  beginAllocationCount();
  stereoHalves.processFrame(stereoInputs, 0, fourChannels.data());
  const auto halvesAllocations = endAllocationCount();
  if (halvesAllocations != 0) {
    std::cerr << "stereo-halves SplitStrategy allocated while processing\n";
    return 1;
  }
  if (!close(fourChannels[0], 0.75 * halvesScale) ||
      !close(fourChannels[1], 0.75 * halvesScale) ||
      !close(fourChannels[2], -0.25 * halvesScale) ||
      !close(fourChannels[3], -0.25 * halvesScale)) {
    std::cerr << "stereo-halves Split mapping is incorrect\n";
    return 1;
  }

  rvrbotron::dsp::Split stereoInterleave(
      {
          2,
          4,
          rvrbotron::dsp::SplitStrategyType::stereoInterleave,
          rvrbotron::dsp::EnergyNormalisation::energy,
          1.0,
          halvesScale,
      });
  beginAllocationCount();
  stereoInterleave.processFrame(stereoInputs, 0, fourChannels.data());
  const auto interleaveAllocations = endAllocationCount();
  if (interleaveAllocations != 0) {
    std::cerr
        << "stereo-interleave SplitStrategy allocated while processing\n";
    return 1;
  }
  if (!close(fourChannels[0], 0.75 * halvesScale) ||
      !close(fourChannels[1], -0.25 * halvesScale) ||
      !close(fourChannels[2], 0.75 * halvesScale) ||
      !close(fourChannels[3], -0.25 * halvesScale)) {
    std::cerr << "stereo-interleave Split mapping is incorrect\n";
    return 1;
  }

  const std::array<rvrbotron::dsp::Sample, 1> monoInput{0.5F};
  const rvrbotron::dsp::Sample* monoInputs[]{monoInput.data()};
  std::array<rvrbotron::dsp::Sample, 4> monoFallbackChannels{};
  constexpr double monoFallbackScale = 0.5; // 1/sqrt(4)
  rvrbotron::dsp::Split monoFallback(
      {
          1,
          4,
          rvrbotron::dsp::SplitStrategyType::stereoHalves,
          rvrbotron::dsp::EnergyNormalisation::energy,
          1.0,
          monoFallbackScale,
      });
  monoFallback.processFrame(monoInputs, 0, monoFallbackChannels.data());
  if (!close(monoFallbackChannels[0], 0.25) ||
      !close(monoFallbackChannels[1], 0.25) ||
      !close(monoFallbackChannels[2], 0.25) ||
      !close(monoFallbackChannels[3], 0.25)) {
    std::cerr
        << "mono input did not fall back to the duplicate mapping for "
           "stereo-halves\n";
    return 1;
  }

  bool oddChannelsThrew = false;
  try {
    rvrbotron::dsp::Split oddHalves(
        {
            2,
            3,
            rvrbotron::dsp::SplitStrategyType::stereoHalves,
            rvrbotron::dsp::EnergyNormalisation::energy,
            1.0,
            1.0,
        });
  } catch (const std::invalid_argument&) {
    oddChannelsThrew = true;
  }
  if (!oddChannelsThrew) {
    std::cerr
        << "stereo-halves Split accepted an odd Channel count with stereo "
           "input\n";
    return 1;
  }

  bool oddInterleaveThrew = false;
  try {
    rvrbotron::dsp::Split oddInterleave(
        {
            2,
            5,
            rvrbotron::dsp::SplitStrategyType::stereoInterleave,
            rvrbotron::dsp::EnergyNormalisation::energy,
            1.0,
            1.0,
        });
  } catch (const std::invalid_argument&) {
    oddInterleaveThrew = true;
  }
  if (!oddInterleaveThrew) {
    std::cerr
        << "stereo-interleave Split accepted an odd Channel count with "
           "stereo input\n";
    return 1;
  }

  // N=1 is odd, so it is rejected the same way as any other odd Channel
  // count when the source is stereo.
  bool oneChannelHalvesThrew = false;
  try {
    rvrbotron::dsp::Split oneChannelHalves(
        {
            2,
            1,
            rvrbotron::dsp::SplitStrategyType::stereoHalves,
            rvrbotron::dsp::EnergyNormalisation::energy,
            1.0,
            1.0,
        });
  } catch (const std::invalid_argument&) {
    oneChannelHalvesThrew = true;
  }
  if (!oneChannelHalvesThrew) {
    std::cerr << "stereo-halves Split accepted N=1 with stereo input\n";
    return 1;
  }

  // Diagnostic "none" normalisation leaves the resolved Channel gain at 1.0
  // regardless of N, unlike energy normalisation's sqrt(2/N) scale.
  rvrbotron::dsp::Split stereoHalvesNone(
      {
          2,
          4,
          rvrbotron::dsp::SplitStrategyType::stereoHalves,
          rvrbotron::dsp::EnergyNormalisation::none,
          1.0,
          1.0,
      });
  stereoHalvesNone.processFrame(stereoInputs, 0, fourChannels.data());
  if (!close(fourChannels[0], 0.75) || !close(fourChannels[1], 0.75) ||
      !close(fourChannels[2], -0.25) || !close(fourChannels[3], -0.25)) {
    std::cerr << "unnormalized stereo-halves Split is incorrect\n";
    return 1;
  }

  const rvrbotron::dsp::ResolvedConfig identityConfig{
      2,
      0,
      48000,
      {},
  };
  rvrbotron::dsp::Reverb identity(identityConfig);

  const std::array<rvrbotron::dsp::Sample, 7> left{
      0.5F,
      -0.25F,
      0.0F,
      1.0F,
      -0.75F,
      0.125F,
      -1.0F,
  };
  const std::array<rvrbotron::dsp::Sample, 7> right{
      -0.5F,
      0.25F,
      1.0F,
      0.0F,
      0.75F,
      -0.125F,
      -1.0F,
  };
  std::array<rvrbotron::dsp::Sample, 7> outputLeft{};
  std::array<rvrbotron::dsp::Sample, 7> outputRight{};
  const rvrbotron::dsp::Sample* identityInputs[]{
      left.data(), right.data()};
  rvrbotron::dsp::Sample* identityOutputs[]{
      outputLeft.data(), outputRight.data()};

  beginAllocationCount();
  identity.process(identityInputs, 2, identityOutputs, 2, left.size());
  const auto identityAllocations = endAllocationCount();
  if (identityAllocations != 0) {
    std::cerr << "empty Composition allocated while processing\n";
    return 1;
  }
  if (left != outputLeft || right != outputRight) {
    std::cerr << "empty Composition changed caller-owned samples\n";
    return 1;
  }

  const auto diffusionConfig = twoChannelDiffusionConfig();
  FixedCapture capture;
  rvrbotron::dsp::Reverb diffusion(diffusionConfig, &capture);
  const std::array<rvrbotron::dsp::Sample, 2> impulse{1.0F, 0.0F};
  std::array<rvrbotron::dsp::Sample, 2> wetLeft{};
  std::array<rvrbotron::dsp::Sample, 2> wetRight{};
  const rvrbotron::dsp::Sample* diffusionInputs[]{impulse.data()};
  rvrbotron::dsp::Sample* diffusionOutputs[]{
      wetLeft.data(), wetRight.data()};

  beginAllocationCount();
  diffusion.process(diffusionInputs, 1, diffusionOutputs, 2, 2);
  const auto diffusionAllocations = endAllocationCount();
  if (diffusionAllocations != 0) {
    std::cerr << "Diffusion Step allocated while processing\n";
    return 1;
  }
  if (!capture.valid || capture.splitFrames != 2 ||
      capture.diffusionFrames != 2) {
    std::cerr << "Stage capture sink did not receive the complete response\n";
    return 1;
  }
  if (!close(capture.split[0], 0.7071067811865476) ||
      !close(capture.split[1], 0.7071067811865476) ||
      capture.split[2] != 0 || capture.split[3] != 0) {
    std::cerr << "duplicate Split mapping is incorrect\n";
    return 1;
  }
  if (!close(wetLeft[0], -0.5) || !close(wetRight[0], 0.5) ||
      !close(wetLeft[1], 0.5) || !close(wetRight[1], 0.5)) {
    std::cerr << "select Downmix did not emit expected wet-only response\n";
    return 1;
  }
  double diffusionEnergy = 0.0;
  for (const auto sample : capture.diffusion) {
    diffusionEnergy += static_cast<double>(sample) * sample;
  }
  if (std::abs(diffusionEnergy - 1.0) > 1.0e-5) {
    std::cerr << "normalized Hadamard Diffusion Step changed energy\n";
    return 1;
  }

  rvrbotron::dsp::Reverb blockwise(diffusionConfig);
  std::array<rvrbotron::dsp::Sample, 2> blockLeft{};
  std::array<rvrbotron::dsp::Sample, 2> blockRight{};
  for (std::size_t frame = 0; frame < impulse.size(); ++frame) {
    const rvrbotron::dsp::Sample* input[]{impulse.data() + frame};
    rvrbotron::dsp::Sample* output[]{
        blockLeft.data() + frame, blockRight.data() + frame};
    blockwise.process(input, 1, output, 2, 1);
  }
  if (blockLeft != wetLeft || blockRight != wetRight) {
    std::cerr << "block size changed finite Diffuser output\n";
    return 1;
  }

  const auto ablationConfig = twoChannelAblationConfig();
  FixedCapture ablationCapture;
  rvrbotron::dsp::Reverb ablation(
      ablationConfig, &ablationCapture);
  std::array<rvrbotron::dsp::Sample, 2> ablationLeft{};
  std::array<rvrbotron::dsp::Sample, 2> ablationRight{};
  rvrbotron::dsp::Sample* ablationOutputs[]{
      ablationLeft.data(), ablationRight.data()};
  beginAllocationCount();
  ablation.process(
      diffusionInputs, 1, ablationOutputs, 2, impulse.size());
  const auto ablationAllocations = endAllocationCount();
  if (ablationAllocations != 0) {
    std::cerr << "ablated stages allocated while processing\n";
    return 1;
  }
  if (!close(ablationCapture.split[0], 1.0) ||
      !close(ablationCapture.split[1], 1.0) ||
      ablationCapture.split[2] != 0 ||
      ablationCapture.split[3] != 0) {
    std::cerr << "Split none normalisation changed level\n";
    return 1;
  }
  if (!close(ablationLeft[0], 0.7071067811865476) ||
      !close(ablationRight[0], 0.7071067811865476) ||
      !close(ablationLeft[1], 0.7071067811865476) ||
      !close(ablationRight[1], -0.7071067811865476)) {
    std::cerr << "Diffusion ablations or Downmix none are incorrect\n";
    return 1;
  }

  // resolveConfig is a public non-JSON entry point too, so a select
  // Downmix with no leftChannel must be rejected there directly (#107)
  // rather than silently resolving Channel 0 -- the JSON parser's own
  // requireField is not the only place this contract has to hold.
  {
    rvrbotron::config::SplitConfig split;
    split.channels = 4;
    split.strategy = rvrbotron::dsp::SplitStrategyType::duplicate;
    split.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;
    rvrbotron::config::DiffuserConfig diffuser;
    diffuser.steps = 1;
    diffuser.totalMs = 2.0;
    rvrbotron::config::DownmixConfig downmixWithoutLeftChannel;
    downmixWithoutLeftChannel.strategy =
        rvrbotron::dsp::DownmixStrategy::select;
    rvrbotron::config::CompositionConfig composition;
    composition.stagesSpecified = true;
    composition.stages.emplace_back(split);
    composition.stages.emplace_back(diffuser);
    composition.stages.emplace_back(downmixWithoutLeftChannel);
    rvrbotron::config::ReverbConfig requested;
    requested.formatVersion = 2;
    requested.composition = std::move(composition);
    bool rejected = false;
    try {
      static_cast<void>(
          rvrbotron::config::resolveConfig(requested, 48000, 1));
    } catch (const rvrbotron::HarnessError&) {
      rejected = true;
    }
    if (!rejected) {
      std::cerr << "resolveConfig accepted a select Downmix with no "
                   "leftChannel\n";
      return 1;
    }
  }

  // orthogonal-rows (#108): rows 0 and 1 of the MAINDNMX-tagged N-by-N
  // RandomOrthogonal matrix at (channels=4, seed=7), independently
  // derived the same way as test_mix_matrix_resolution.cpp's own MAINDNMX
  // fixed vectors (hand-verified Householder QR, same sign convention).
  {
    rvrbotron::config::SplitConfig split;
    split.channels = 4;
    split.strategy = rvrbotron::dsp::SplitStrategyType::duplicate;
    split.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;
    rvrbotron::config::DiffuserConfig diffuser;
    diffuser.steps = 1;
    diffuser.totalMs = 1.0;
    rvrbotron::config::DownmixConfig orthogonalRowsDownmix;
    orthogonalRowsDownmix.strategy =
        rvrbotron::dsp::DownmixStrategy::orthogonalRows;
    rvrbotron::config::CompositionConfig composition;
    composition.stagesSpecified = true;
    composition.stages.emplace_back(split);
    composition.stages.emplace_back(diffuser);
    composition.stages.emplace_back(orthogonalRowsDownmix);
    rvrbotron::config::ReverbConfig requested;
    requested.formatVersion = 2;
    requested.seed = 7;
    requested.composition = composition;
    const auto resolved =
        rvrbotron::config::resolveConfig(requested, 48000, 1);
    const auto& downmix = std::get<rvrbotron::dsp::ResolvedDownmix>(
        resolved.composition.stages[2]);
    if (downmix.leftChannel.has_value() ||
        downmix.rightChannel.has_value()) {
      std::cerr << "orthogonal-rows resolved a leftChannel/rightChannel "
                   "it has no use for\n";
      return 1;
    }
    const std::array<double, 4> expectedRow0{
        -0.06277167196100764, 0.4333697441536047, 0.20815530399484583,
        -0.8745980513757576};
    const std::array<double, 4> expectedRow1{
        0.6192030189721484, -0.6680724091585977, -0.10215450256737603,
        -0.39978911318596055};
    const auto expectedCompensation = std::sqrt(2.0);
    if (downmix.compensation != expectedCompensation) {
      std::cerr << "orthogonal-rows compensation did not match sqrt(N/2)\n";
      return 1;
    }
    for (std::size_t index = 0; index < 4; ++index) {
      if (std::abs(downmix.leftRow[index] - expectedRow0[index]) > 1e-9 ||
          std::abs(downmix.rightRow[index] - expectedRow1[index]) > 1e-9) {
        std::cerr << "orthogonal-rows leftRow/rightRow did not match the "
                     "independently derived MAINDNMX fixed vector\n";
        return 1;
      }
      if (std::abs(
              downmix.effectiveLeftRow[index] -
              expectedRow0[index] * expectedCompensation) > 1e-9 ||
          std::abs(
              downmix.effectiveRightRow[index] -
              expectedRow1[index] * expectedCompensation) > 1e-9) {
        std::cerr << "orthogonal-rows effective rows did not match rows "
                     "scaled by compensation\n";
        return 1;
      }
    }
    if (downmix.alignment != rvrbotron::dsp::DownmixAlignment::aligned) {
      std::cerr << "orthogonal-rows Diffuser-only Downmix did not resolve "
                   "aligned\n";
      return 1;
    }

    rvrbotron::dsp::Reverb orthogonalReverb(resolved);
    std::array<rvrbotron::dsp::Sample, 1> orthogonalInput{
        rvrbotron::dsp::Sample{1}};
    std::array<rvrbotron::dsp::Sample, 1> orthogonalLeft{};
    std::array<rvrbotron::dsp::Sample, 1> orthogonalRight{};
    const rvrbotron::dsp::Sample* orthogonalInputs[]{
        orthogonalInput.data()};
    rvrbotron::dsp::Sample* orthogonalOutputs[]{
        orthogonalLeft.data(), orthogonalRight.data()};
    beginAllocationCount();
    orthogonalReverb.process(orthogonalInputs, 1, orthogonalOutputs, 2, 1);
    if (endAllocationCount() != 0) {
      std::cerr << "orthogonal-rows Downmix allocated while processing\n";
      return 1;
    }

    // N=1 has no row 1 to select: orthogonal-rows requires N >= 2.
    auto singleChannelComposition = composition;
    std::get<rvrbotron::config::SplitConfig>(
        singleChannelComposition.stages[0])
        .channels = 1;
    rvrbotron::config::ReverbConfig singleChannelRequested;
    singleChannelRequested.formatVersion = 2;
    singleChannelRequested.seed = 7;
    singleChannelRequested.composition = std::move(singleChannelComposition);
    bool singleChannelRejected = false;
    try {
      static_cast<void>(rvrbotron::config::resolveConfig(
          singleChannelRequested, 48000, 1));
    } catch (const rvrbotron::HarnessError&) {
      singleChannelRejected = true;
    }
    if (!singleChannelRejected) {
      std::cerr << "resolveConfig accepted orthogonal-rows at N=1\n";
      return 1;
    }
  }

  // `halves` (#110): the first ceil(N/2) Channels map left, the remainder
  // maps right, each group using equal 1/sqrt(groupSize) coefficients.
  {
    rvrbotron::config::SplitConfig split;
    split.channels = 4;
    split.strategy = rvrbotron::dsp::SplitStrategyType::duplicate;
    split.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;
    rvrbotron::config::DiffuserConfig diffuser;
    diffuser.steps = 1;
    diffuser.totalMs = 1.0;
    // Householder (unlike the default Hadamard) is valid at the N=5 odd
    // Channel count exercised below, and at N=4/N=1.
    rvrbotron::config::DiffusionStepConfig step;
    step.mix = rvrbotron::dsp::MixMatrixType::householder;
    diffuser.step = step;
    rvrbotron::config::DownmixConfig halvesDownmix;
    halvesDownmix.strategy = rvrbotron::dsp::DownmixStrategy::halves;
    rvrbotron::config::CompositionConfig composition;
    composition.stagesSpecified = true;
    composition.stages.emplace_back(split);
    composition.stages.emplace_back(diffuser);
    composition.stages.emplace_back(halvesDownmix);
    rvrbotron::config::ReverbConfig requested;
    requested.formatVersion = 2;
    requested.seed = 7;
    requested.composition = composition;
    const auto resolved =
        rvrbotron::config::resolveConfig(requested, 48000, 1);
    const auto& downmix = std::get<rvrbotron::dsp::ResolvedDownmix>(
        resolved.composition.stages[2]);
    if (downmix.leftChannel.has_value() ||
        downmix.rightChannel.has_value()) {
      std::cerr << "halves resolved a leftChannel/rightChannel it has no "
                   "use for\n";
      return 1;
    }
    const auto half = 1.0 / std::sqrt(2.0);
    const std::array<double, 4> expectedLeft{half, half, 0.0, 0.0};
    const std::array<double, 4> expectedRight{0.0, 0.0, half, half};
    const auto expectedCompensation = std::sqrt(2.0);
    if (downmix.compensation != expectedCompensation) {
      std::cerr << "halves compensation did not match sqrt(N/2)\n";
      return 1;
    }
    for (std::size_t index = 0; index < 4; ++index) {
      if (downmix.leftRow[index] != expectedLeft[index] ||
          downmix.rightRow[index] != expectedRight[index]) {
        std::cerr << "halves leftRow/rightRow did not match the expected "
                     "equal-coefficient groups\n";
        return 1;
      }
      if (downmix.effectiveLeftRow[index] !=
              expectedLeft[index] * expectedCompensation ||
          downmix.effectiveRightRow[index] !=
              expectedRight[index] * expectedCompensation) {
        std::cerr << "halves effective rows did not match rows scaled by "
                     "compensation\n";
        return 1;
      }
    }
    if (downmix.alignment != rvrbotron::dsp::DownmixAlignment::aligned) {
      std::cerr << "halves Diffuser-only Downmix did not resolve aligned\n";
      return 1;
    }

    rvrbotron::dsp::Reverb halvesReverb(resolved);
    std::array<rvrbotron::dsp::Sample, 1> halvesInput{
        rvrbotron::dsp::Sample{1}};
    std::array<rvrbotron::dsp::Sample, 1> halvesLeft{};
    std::array<rvrbotron::dsp::Sample, 1> halvesRight{};
    const rvrbotron::dsp::Sample* halvesInputs[]{halvesInput.data()};
    rvrbotron::dsp::Sample* halvesOutputs[]{
        halvesLeft.data(), halvesRight.data()};
    beginAllocationCount();
    halvesReverb.process(halvesInputs, 1, halvesOutputs, 2, 1);
    if (endAllocationCount() != 0) {
      std::cerr << "halves Downmix allocated while processing\n";
      return 1;
    }

    // N=5 is odd: left group ceil(5/2)=3 at 1/sqrt(3), right group
    // floor(5/2)=2 at 1/sqrt(2) -- unequal odd-N groups (#110).
    auto oddComposition = composition;
    std::get<rvrbotron::config::SplitConfig>(oddComposition.stages[0])
        .channels = 5;
    rvrbotron::config::ReverbConfig oddRequested;
    oddRequested.formatVersion = 2;
    oddRequested.seed = 7;
    oddRequested.composition = std::move(oddComposition);
    const auto oddResolved =
        rvrbotron::config::resolveConfig(oddRequested, 48000, 1);
    const auto& oddDownmix = std::get<rvrbotron::dsp::ResolvedDownmix>(
        oddResolved.composition.stages[2]);
    const auto oddLeftCoefficient = 1.0 / std::sqrt(3.0);
    const auto oddRightCoefficient = 1.0 / std::sqrt(2.0);
    const std::array<double, 5> oddExpectedLeft{
        oddLeftCoefficient, oddLeftCoefficient, oddLeftCoefficient, 0.0,
        0.0};
    const std::array<double, 5> oddExpectedRight{
        0.0, 0.0, 0.0, oddRightCoefficient, oddRightCoefficient};
    for (std::size_t index = 0; index < 5; ++index) {
      if (oddDownmix.leftRow[index] != oddExpectedLeft[index] ||
          oddDownmix.rightRow[index] != oddExpectedRight[index]) {
        std::cerr << "halves at odd N=5 did not resolve the expected "
                     "unequal groups\n";
        return 1;
      }
    }

    // N=1 has no second Channel to place in the remainder group: halves
    // requires N >= 2.
    auto singleChannelComposition = composition;
    std::get<rvrbotron::config::SplitConfig>(
        singleChannelComposition.stages[0])
        .channels = 1;
    rvrbotron::config::ReverbConfig singleChannelRequested;
    singleChannelRequested.formatVersion = 2;
    singleChannelRequested.seed = 7;
    singleChannelRequested.composition =
        std::move(singleChannelComposition);
    bool singleChannelRejected = false;
    try {
      static_cast<void>(rvrbotron::config::resolveConfig(
          singleChannelRequested, 48000, 1));
    } catch (const rvrbotron::HarnessError&) {
      singleChannelRejected = true;
    }
    if (!singleChannelRejected) {
      std::cerr << "resolveConfig accepted halves at N=1\n";
      return 1;
    }
  }

  // `alternating` (#110): even Channel indices map left, odd indices map
  // right, each group using equal 1/sqrt(groupSize) coefficients.
  {
    rvrbotron::config::SplitConfig split;
    split.channels = 4;
    split.strategy = rvrbotron::dsp::SplitStrategyType::duplicate;
    split.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;
    rvrbotron::config::DiffuserConfig diffuser;
    diffuser.steps = 1;
    diffuser.totalMs = 1.0;
    // Householder (unlike the default Hadamard) is valid at the N=5 odd
    // Channel count exercised below, and at N=4/N=1.
    rvrbotron::config::DiffusionStepConfig step;
    step.mix = rvrbotron::dsp::MixMatrixType::householder;
    diffuser.step = step;
    rvrbotron::config::DownmixConfig alternatingDownmix;
    alternatingDownmix.strategy =
        rvrbotron::dsp::DownmixStrategy::alternating;
    rvrbotron::config::CompositionConfig composition;
    composition.stagesSpecified = true;
    composition.stages.emplace_back(split);
    composition.stages.emplace_back(diffuser);
    composition.stages.emplace_back(alternatingDownmix);
    rvrbotron::config::ReverbConfig requested;
    requested.formatVersion = 2;
    requested.seed = 7;
    requested.composition = composition;
    const auto resolved =
        rvrbotron::config::resolveConfig(requested, 48000, 1);
    const auto& downmix = std::get<rvrbotron::dsp::ResolvedDownmix>(
        resolved.composition.stages[2]);
    if (downmix.leftChannel.has_value() ||
        downmix.rightChannel.has_value()) {
      std::cerr << "alternating resolved a leftChannel/rightChannel it "
                   "has no use for\n";
      return 1;
    }
    const auto half = 1.0 / std::sqrt(2.0);
    const std::array<double, 4> expectedLeft{half, 0.0, half, 0.0};
    const std::array<double, 4> expectedRight{0.0, half, 0.0, half};
    const auto expectedCompensation = std::sqrt(2.0);
    if (downmix.compensation != expectedCompensation) {
      std::cerr << "alternating compensation did not match sqrt(N/2)\n";
      return 1;
    }
    for (std::size_t index = 0; index < 4; ++index) {
      if (downmix.leftRow[index] != expectedLeft[index] ||
          downmix.rightRow[index] != expectedRight[index]) {
        std::cerr << "alternating leftRow/rightRow did not match the "
                     "expected even/odd groups\n";
        return 1;
      }
      if (downmix.effectiveLeftRow[index] !=
              expectedLeft[index] * expectedCompensation ||
          downmix.effectiveRightRow[index] !=
              expectedRight[index] * expectedCompensation) {
        std::cerr << "alternating effective rows did not match rows "
                     "scaled by compensation\n";
        return 1;
      }
    }
    if (downmix.alignment != rvrbotron::dsp::DownmixAlignment::aligned) {
      std::cerr << "alternating Diffuser-only Downmix did not resolve "
                   "aligned\n";
      return 1;
    }

    rvrbotron::dsp::Reverb alternatingReverb(resolved);
    std::array<rvrbotron::dsp::Sample, 1> alternatingInput{
        rvrbotron::dsp::Sample{1}};
    std::array<rvrbotron::dsp::Sample, 1> alternatingLeft{};
    std::array<rvrbotron::dsp::Sample, 1> alternatingRight{};
    const rvrbotron::dsp::Sample* alternatingInputs[]{
        alternatingInput.data()};
    rvrbotron::dsp::Sample* alternatingOutputs[]{
        alternatingLeft.data(), alternatingRight.data()};
    beginAllocationCount();
    alternatingReverb.process(
        alternatingInputs, 1, alternatingOutputs, 2, 1);
    if (endAllocationCount() != 0) {
      std::cerr << "alternating Downmix allocated while processing\n";
      return 1;
    }

    // N=5 is odd: even indices {0,2,4} (size 3, 1/sqrt(3)); odd indices
    // {1,3} (size 2, 1/sqrt(2)) -- unequal odd-N groups (#110).
    auto oddComposition = composition;
    std::get<rvrbotron::config::SplitConfig>(oddComposition.stages[0])
        .channels = 5;
    rvrbotron::config::ReverbConfig oddRequested;
    oddRequested.formatVersion = 2;
    oddRequested.seed = 7;
    oddRequested.composition = std::move(oddComposition);
    const auto oddResolved =
        rvrbotron::config::resolveConfig(oddRequested, 48000, 1);
    const auto& oddDownmix = std::get<rvrbotron::dsp::ResolvedDownmix>(
        oddResolved.composition.stages[2]);
    const auto oddLeftCoefficient = 1.0 / std::sqrt(3.0);
    const auto oddRightCoefficient = 1.0 / std::sqrt(2.0);
    const std::array<double, 5> oddExpectedLeft{
        oddLeftCoefficient, 0.0, oddLeftCoefficient, 0.0,
        oddLeftCoefficient};
    const std::array<double, 5> oddExpectedRight{
        0.0, oddRightCoefficient, 0.0, oddRightCoefficient, 0.0};
    for (std::size_t index = 0; index < 5; ++index) {
      if (oddDownmix.leftRow[index] != oddExpectedLeft[index] ||
          oddDownmix.rightRow[index] != oddExpectedRight[index]) {
        std::cerr << "alternating at odd N=5 did not resolve the expected "
                     "unequal groups\n";
        return 1;
      }
    }

    // N=1 has no odd-indexed Channel: alternating requires N >= 2.
    auto singleChannelComposition = composition;
    std::get<rvrbotron::config::SplitConfig>(
        singleChannelComposition.stages[0])
        .channels = 1;
    rvrbotron::config::ReverbConfig singleChannelRequested;
    singleChannelRequested.formatVersion = 2;
    singleChannelRequested.seed = 7;
    singleChannelRequested.composition =
        std::move(singleChannelComposition);
    bool singleChannelRejected = false;
    try {
      static_cast<void>(rvrbotron::config::resolveConfig(
          singleChannelRequested, 48000, 1));
    } catch (const rvrbotron::HarnessError&) {
      singleChannelRejected = true;
    }
    if (!singleChannelRejected) {
      std::cerr << "resolveConfig accepted alternating at N=1\n";
      return 1;
    }
  }

  // `sum-all` (#114): the diagnostic Coherent Downmix ablation -- the
  // same `1/sqrt(N)` row duplicated to both L and R. Unlike halves/
  // alternating/orthogonal-rows, it supports N>=1 like `select`
  // (docs/design/reverb/stages/08-downmix.md), so N=1 resolves rather
  // than being rejected.
  {
    rvrbotron::config::SplitConfig split;
    split.channels = 4;
    split.strategy = rvrbotron::dsp::SplitStrategyType::duplicate;
    split.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;
    rvrbotron::config::DiffuserConfig diffuser;
    diffuser.steps = 1;
    diffuser.totalMs = 1.0;
    // Householder (unlike the default Hadamard) is valid at the N=1
    // Channel count exercised below, and at N=4.
    rvrbotron::config::DiffusionStepConfig step;
    step.mix = rvrbotron::dsp::MixMatrixType::householder;
    diffuser.step = step;
    rvrbotron::config::DownmixConfig sumAllDownmix;
    sumAllDownmix.strategy = rvrbotron::dsp::DownmixStrategy::sumAll;
    rvrbotron::config::CompositionConfig composition;
    composition.stagesSpecified = true;
    composition.stages.emplace_back(split);
    composition.stages.emplace_back(diffuser);
    composition.stages.emplace_back(sumAllDownmix);
    rvrbotron::config::ReverbConfig requested;
    requested.formatVersion = 2;
    requested.seed = 7;
    requested.composition = composition;
    const auto resolved =
        rvrbotron::config::resolveConfig(requested, 48000, 1);
    const auto& downmix = std::get<rvrbotron::dsp::ResolvedDownmix>(
        resolved.composition.stages[2]);
    if (downmix.leftChannel.has_value() ||
        downmix.rightChannel.has_value()) {
      std::cerr << "sum-all resolved a leftChannel/rightChannel it has no "
                   "use for\n";
      return 1;
    }
    const auto coefficient = 0.5; // 1/sqrt(4)
    const std::array<double, 4> expectedRow{
        coefficient, coefficient, coefficient, coefficient};
    const auto expectedCompensation = std::sqrt(2.0);
    if (downmix.compensation != expectedCompensation) {
      std::cerr << "sum-all compensation did not match sqrt(N/2)\n";
      return 1;
    }
    for (std::size_t index = 0; index < 4; ++index) {
      if (downmix.leftRow[index] != expectedRow[index] ||
          downmix.rightRow[index] != expectedRow[index]) {
        std::cerr << "sum-all leftRow/rightRow did not match the expected "
                     "duplicated 1/sqrt(N) row\n";
        return 1;
      }
      if (downmix.effectiveLeftRow[index] !=
              expectedRow[index] * expectedCompensation ||
          downmix.effectiveRightRow[index] !=
              expectedRow[index] * expectedCompensation) {
        std::cerr << "sum-all effective rows did not match rows scaled by "
                     "compensation\n";
        return 1;
      }
    }
    if (downmix.leftRow != downmix.rightRow) {
      std::cerr << "sum-all leftRow/rightRow were not the same duplicated "
                   "row\n";
      return 1;
    }
    if (downmix.alignment != rvrbotron::dsp::DownmixAlignment::aligned) {
      std::cerr << "sum-all Diffuser-only Downmix did not resolve "
                   "aligned\n";
      return 1;
    }

    rvrbotron::dsp::Reverb sumAllReverb(resolved);
    std::array<rvrbotron::dsp::Sample, 1> sumAllInput{
        rvrbotron::dsp::Sample{1}};
    std::array<rvrbotron::dsp::Sample, 1> sumAllLeft{};
    std::array<rvrbotron::dsp::Sample, 1> sumAllRight{};
    const rvrbotron::dsp::Sample* sumAllInputs[]{sumAllInput.data()};
    rvrbotron::dsp::Sample* sumAllOutputs[]{
        sumAllLeft.data(), sumAllRight.data()};
    beginAllocationCount();
    sumAllReverb.process(sumAllInputs, 1, sumAllOutputs, 2, 1);
    if (endAllocationCount() != 0) {
      std::cerr << "sum-all Downmix allocated while processing\n";
      return 1;
    }
    // N=1 has no second Channel, unlike halves/alternating, but sum-all
    // supports N>=1 like select (#114): resolution must succeed, not
    // reject, and duplicates the sole Channel with the same N=1 energy
    // compensation select's own mono duplication uses.
    auto singleChannelComposition = composition;
    std::get<rvrbotron::config::SplitConfig>(
        singleChannelComposition.stages[0])
        .channels = 1;
    rvrbotron::config::ReverbConfig singleChannelRequested;
    singleChannelRequested.formatVersion = 2;
    singleChannelRequested.seed = 7;
    singleChannelRequested.composition =
        std::move(singleChannelComposition);
    const auto singleChannelResolved = rvrbotron::config::resolveConfig(
        singleChannelRequested, 48000, 1);
    const auto& singleChannelDownmix =
        std::get<rvrbotron::dsp::ResolvedDownmix>(
            singleChannelResolved.composition.stages[2]);
    const auto singleChannelCompensation = 1.0 / std::sqrt(2.0);
    if (singleChannelDownmix.leftRow.size() != 1 ||
        singleChannelDownmix.leftRow[0] != 1.0 ||
        singleChannelDownmix.rightRow != singleChannelDownmix.leftRow ||
        singleChannelDownmix.compensation != singleChannelCompensation) {
      std::cerr << "sum-all at N=1 did not resolve the expected "
                   "single-Channel duplicated row\n";
      return 1;
    }
  }

  // halves/alternating/sum-all through a Feedback Loop (#110/#114):
  // Alignment expectation is derived from Composition wiring, so a
  // source that includes a Feedback Loop must resolve unaligned for
  // every strategy, exactly as it already does for select/
  // orthogonal-rows.
  for (const auto strategy :
       {rvrbotron::dsp::DownmixStrategy::halves,
        rvrbotron::dsp::DownmixStrategy::alternating,
        rvrbotron::dsp::DownmixStrategy::sumAll}) {
    rvrbotron::config::SplitConfig split;
    split.channels = 4;
    split.strategy = rvrbotron::dsp::SplitStrategyType::duplicate;
    split.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;
    rvrbotron::config::FeedbackLoopConfig loop;
    rvrbotron::config::DownmixConfig downmixConfig;
    downmixConfig.strategy = strategy;
    rvrbotron::config::CompositionConfig composition;
    composition.stagesSpecified = true;
    composition.stages.emplace_back(split);
    composition.stages.emplace_back(loop);
    composition.stages.emplace_back(downmixConfig);
    rvrbotron::config::ReverbConfig requested;
    requested.formatVersion = 2;
    requested.seed = 7;
    requested.composition = std::move(composition);
    const auto resolved =
        rvrbotron::config::resolveConfig(requested, 48000, 1);
    const auto& downmix = std::get<rvrbotron::dsp::ResolvedDownmix>(
        resolved.composition.stages[2]);
    if (downmix.alignment != rvrbotron::dsp::DownmixAlignment::unaligned) {
      std::cerr << "halves/alternating/sum-all Feedback Loop Downmix did "
                   "not resolve unaligned\n";
      return 1;
    }
  }

  // Width (#109): the resolved 2x2 mid/side matrix, exact at the 0/90/180
  // endpoints and via the documented formula at an intermediate angle
  // (docs/design/reverb/stages/08-downmix.md's "Width as a constant-power
  // mid/side law").
  for (const auto widthDeg : {0.0, 45.0, 90.0, 135.0, 180.0}) {
    rvrbotron::config::SplitConfig split;
    split.channels = 2;
    split.strategy = rvrbotron::dsp::SplitStrategyType::duplicate;
    split.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;
    rvrbotron::config::DiffuserConfig diffuser;
    diffuser.steps = 1;
    diffuser.totalMs = 1.0;
    auto downmix = referenceSelectDownmixConfig(2);
    downmix.widthDeg = widthDeg;
    rvrbotron::config::CompositionConfig composition;
    composition.stagesSpecified = true;
    composition.stages.emplace_back(split);
    composition.stages.emplace_back(diffuser);
    composition.stages.emplace_back(downmix);
    rvrbotron::config::ReverbConfig requested;
    requested.formatVersion = 2;
    requested.seed = 7;
    requested.composition = composition;
    const auto resolved =
        rvrbotron::config::resolveConfig(requested, 48000, 1);
    const auto& resolvedDownmix = std::get<rvrbotron::dsp::ResolvedDownmix>(
        resolved.composition.stages[2]);
    if (resolvedDownmix.widthDeg != widthDeg) {
      std::cerr << "Width did not resolve the requested widthDeg\n";
      return 1;
    }
    const auto expectedMatrix = expectedWidthMatrix(widthDeg);
    if (resolvedDownmix.widthMatrix.size() != 4) {
      std::cerr << "Width matrix did not resolve to 4 elements\n";
      return 1;
    }
    for (std::size_t index = 0; index < 4; ++index) {
      if (resolvedDownmix.widthMatrix[index] != expectedMatrix[index]) {
        std::cerr << "Width matrix at " << widthDeg
                   << " degrees did not match the documented formula\n";
        return 1;
      }
    }
  }

  // Width's DSP application (#109), tested directly at the Downmix seam
  // with a hand-chosen pre-Width [left, right] rather than through the
  // full Reverb pipeline, so the pre-Width value is exactly known.
  {
    auto widthTestDownmix = [](const double widthDeg) {
      rvrbotron::dsp::ResolvedDownmix config;
      config.inputChannels = 2;
      config.outputChannels = 2;
      config.strategy = rvrbotron::dsp::DownmixStrategy::select;
      config.leftChannel = 0;
      config.rightChannel = 1;
      config.normalisation = rvrbotron::dsp::EnergyNormalisation::none;
      config.compensation = 1.0;
      config.widthDeg = widthDeg;
      const auto matrix = expectedWidthMatrix(widthDeg);
      config.widthMatrix.assign(matrix.begin(), matrix.end());
      return rvrbotron::dsp::Downmix(config);
    };
    const auto preWidthLeft = static_cast<rvrbotron::dsp::Sample>(0.6);
    const auto preWidthRight = static_cast<rvrbotron::dsp::Sample>(-0.4);
    const std::array<rvrbotron::dsp::Sample, 2> preWidth{
        preWidthLeft, preWidthRight};
    const rvrbotron::dsp::Sample* const channelsData = preWidth.data();

    // 90 degrees is an exact identity bypass.
    {
      auto downmix = widthTestDownmix(90.0);
      std::array<rvrbotron::dsp::Sample, 1> left{};
      std::array<rvrbotron::dsp::Sample, 1> right{};
      rvrbotron::dsp::Sample* outputs[]{left.data(), right.data()};
      downmix.processFrame(channelsData, outputs, 0);
      if (left[0] != preWidthLeft || right[0] != preWidthRight) {
        std::cerr << "Width at 90 degrees was not an exact bypass\n";
        return 1;
      }
    }

    // 90 degrees is bit-identical even for signed zero, which the
    // matrix form (1*L + 0*R) alone would not preserve: IEEE 754 rounds
    // (-0.0) + (+0.0) to +0.0, not -0.0.
    {
      auto downmix = widthTestDownmix(90.0);
      const auto negativeZero = -static_cast<rvrbotron::dsp::Sample>(0.0);
      const auto positiveZero = static_cast<rvrbotron::dsp::Sample>(0.0);
      const std::array<rvrbotron::dsp::Sample, 2> signedZeroPreWidth{
          negativeZero, positiveZero};
      std::array<rvrbotron::dsp::Sample, 1> left{};
      std::array<rvrbotron::dsp::Sample, 1> right{};
      rvrbotron::dsp::Sample* outputs[]{left.data(), right.data()};
      downmix.processFrame(signedZeroPreWidth.data(), outputs, 0);
      if (!std::signbit(left[0]) || std::signbit(right[0])) {
        std::cerr << "Width at 90 degrees did not preserve signed zero "
                     "on bypass\n";
        return 1;
      }
    }

    // 0 degrees mono-izes any input to (L+R)/sqrt(2) on both outputs.
    {
      auto downmix = widthTestDownmix(0.0);
      std::array<rvrbotron::dsp::Sample, 1> left{};
      std::array<rvrbotron::dsp::Sample, 1> right{};
      rvrbotron::dsp::Sample* outputs[]{left.data(), right.data()};
      downmix.processFrame(channelsData, outputs, 0);
      const auto expectedMono =
          (static_cast<double>(preWidthLeft) +
           static_cast<double>(preWidthRight)) /
          std::sqrt(2.0);
      if (!close(left[0], expectedMono) || !close(right[0], expectedMono)) {
        std::cerr << "Width at 0 degrees did not mono-ize the input\n";
        return 1;
      }
    }

    // 180 degrees is side-only and out of phase: a mono pre-Width input
    // (left == right) cancels to exact zero.
    {
      auto downmix = widthTestDownmix(180.0);
      const auto monoValue = static_cast<rvrbotron::dsp::Sample>(0.5);
      const std::array<rvrbotron::dsp::Sample, 2> monoPreWidth{
          monoValue, monoValue};
      std::array<rvrbotron::dsp::Sample, 1> left{};
      std::array<rvrbotron::dsp::Sample, 1> right{};
      rvrbotron::dsp::Sample* outputs[]{left.data(), right.data()};
      downmix.processFrame(monoPreWidth.data(), outputs, 0);
      if (left[0] != rvrbotron::dsp::Sample{0} ||
          right[0] != rvrbotron::dsp::Sample{0}) {
        std::cerr << "Width at 180 degrees did not cancel a mono "
                     "pre-Width input\n";
        return 1;
      }
    }

    // 180 degrees on a non-mono input is side-only and out of phase:
    // equal magnitude, opposite sign.
    {
      auto downmix = widthTestDownmix(180.0);
      std::array<rvrbotron::dsp::Sample, 1> left{};
      std::array<rvrbotron::dsp::Sample, 1> right{};
      rvrbotron::dsp::Sample* outputs[]{left.data(), right.data()};
      downmix.processFrame(channelsData, outputs, 0);
      const auto expectedSide =
          (static_cast<double>(preWidthLeft) -
           static_cast<double>(preWidthRight)) /
          std::sqrt(2.0);
      if (!close(left[0], expectedSide) || !close(right[0], -expectedSide)) {
        std::cerr << "Width at 180 degrees was not side-only and out of "
                     "phase\n";
        return 1;
      }
    }

    // An intermediate width (45 degrees) applies the same formula the
    // resolved matrix above was checked against.
    {
      auto downmix = widthTestDownmix(45.0);
      std::array<rvrbotron::dsp::Sample, 1> left{};
      std::array<rvrbotron::dsp::Sample, 1> right{};
      rvrbotron::dsp::Sample* outputs[]{left.data(), right.data()};
      downmix.processFrame(channelsData, outputs, 0);
      const auto matrix45 = expectedWidthMatrix(45.0);
      const auto expectedLeft =
          matrix45[0] * static_cast<double>(preWidthLeft) +
          matrix45[1] * static_cast<double>(preWidthRight);
      const auto expectedRight =
          matrix45[2] * static_cast<double>(preWidthLeft) +
          matrix45[3] * static_cast<double>(preWidthRight);
      if (!close(left[0], expectedLeft) || !close(right[0], expectedRight)) {
        std::cerr << "Width at 45 degrees did not match the documented "
                     "formula\n";
        return 1;
      }
    }
  }

  // Main wet path enablement and level (#109). resolveConfig is a public
  // non-JSON entry point too (mirroring #107/#108/#110's own
  // direct-construction checks): mainEnabled/mainLevelDb set on the
  // empty identity Composition must be rejected there directly.
  {
    rvrbotron::config::CompositionConfig emptyWithMainEnabled;
    emptyWithMainEnabled.mainEnabled = false;
    rvrbotron::config::ReverbConfig requested;
    requested.formatVersion = 2;
    requested.composition = emptyWithMainEnabled;
    bool rejected = false;
    try {
      static_cast<void>(
          rvrbotron::config::resolveConfig(requested, 48000, 1));
    } catch (const rvrbotron::HarnessError&) {
      rejected = true;
    }
    if (!rejected) {
      std::cerr << "resolveConfig accepted mainEnabled on the empty "
                   "identity Composition\n";
      return 1;
    }
  }
  {
    rvrbotron::config::CompositionConfig emptyWithMainLevelDb;
    emptyWithMainLevelDb.mainLevelDb = -6.0;
    rvrbotron::config::ReverbConfig requested;
    requested.formatVersion = 2;
    requested.composition = emptyWithMainLevelDb;
    bool rejected = false;
    try {
      static_cast<void>(
          rvrbotron::config::resolveConfig(requested, 48000, 1));
    } catch (const rvrbotron::HarnessError&) {
      rejected = true;
    }
    if (!rejected) {
      std::cerr << "resolveConfig accepted mainLevelDb on the empty "
                   "identity Composition\n";
      return 1;
    }
  }

  // A non-empty Composition exposes documented mainEnabled/mainLevelDb
  // defaults (#109): enabled, 0 dB, and the linear gain that implies.
  {
    rvrbotron::config::SplitConfig split;
    split.channels = 2;
    split.strategy = rvrbotron::dsp::SplitStrategyType::duplicate;
    split.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;
    rvrbotron::config::DiffuserConfig diffuser;
    diffuser.steps = 1;
    diffuser.totalMs = 1.0;
    auto downmix = referenceSelectDownmixConfig(2);
    rvrbotron::config::CompositionConfig composition;
    composition.stagesSpecified = true;
    composition.stages.emplace_back(split);
    composition.stages.emplace_back(diffuser);
    composition.stages.emplace_back(downmix);
    rvrbotron::config::ReverbConfig requested;
    requested.formatVersion = 2;
    requested.seed = 7;
    requested.composition = composition;
    const auto resolved =
        rvrbotron::config::resolveConfig(requested, 48000, 1);
    if (!resolved.composition.mainEnabled ||
        resolved.composition.mainLevelDb != 0.0 ||
        resolved.composition.mainGain != 1.0) {
      std::cerr << "a non-empty Composition did not default to "
                   "mainEnabled/0 dB mainLevelDb/1.0 mainGain\n";
      return 1;
    }

    // A disabled Main wet path contributes exact stereo zero (#109),
    // even though its interior Split/Diffuser processing still runs.
    auto disabledComposition = composition;
    disabledComposition.mainEnabled = false;
    rvrbotron::config::ReverbConfig disabledRequested;
    disabledRequested.formatVersion = 2;
    disabledRequested.seed = 7;
    disabledRequested.composition = std::move(disabledComposition);
    const auto disabledResolved =
        rvrbotron::config::resolveConfig(disabledRequested, 48000, 1);
    if (disabledResolved.composition.mainEnabled) {
      std::cerr << "mainEnabled: false did not resolve disabled\n";
      return 1;
    }
    rvrbotron::dsp::Reverb disabledReverb(disabledResolved);
    std::array<rvrbotron::dsp::Sample, 4> disabledInput{
        rvrbotron::dsp::Sample{1},
        rvrbotron::dsp::Sample{1},
        rvrbotron::dsp::Sample{1},
        rvrbotron::dsp::Sample{1}};
    std::array<rvrbotron::dsp::Sample, 4> disabledLeft{};
    std::array<rvrbotron::dsp::Sample, 4> disabledRight{};
    const rvrbotron::dsp::Sample* disabledInputs[]{disabledInput.data()};
    rvrbotron::dsp::Sample* disabledOutputs[]{
        disabledLeft.data(), disabledRight.data()};
    disabledReverb.process(disabledInputs, 1, disabledOutputs, 2, 4);
    for (std::size_t frame = 0; frame < 4; ++frame) {
      if (disabledLeft[frame] != rvrbotron::dsp::Sample{0} ||
          disabledRight[frame] != rvrbotron::dsp::Sample{0}) {
        std::cerr << "a disabled Main wet path did not contribute exact "
                     "stereo zero\n";
        return 1;
      }
    }

    // mainLevelDb scales the post-Width stereo output by its linear
    // gain, applied once after Downmix (#109).
    auto leveledComposition = composition;
    leveledComposition.mainLevelDb = -6.0;
    rvrbotron::config::ReverbConfig leveledRequested;
    leveledRequested.formatVersion = 2;
    leveledRequested.seed = 7;
    leveledRequested.composition = std::move(leveledComposition);
    const auto leveledResolved =
        rvrbotron::config::resolveConfig(leveledRequested, 48000, 1);
    const auto expectedGain = std::pow(10.0, -6.0 / 20.0);
    if (std::abs(leveledResolved.composition.mainGain - expectedGain) >
        1e-9) {
      std::cerr << "mainLevelDb: -6 did not resolve the expected linear "
                   "gain\n";
      return 1;
    }
    rvrbotron::dsp::Reverb unleveledReverb(resolved);
    rvrbotron::dsp::Reverb leveledReverb(leveledResolved);
    std::array<rvrbotron::dsp::Sample, 4> unleveledInput{
        rvrbotron::dsp::Sample{1},
        rvrbotron::dsp::Sample{1},
        rvrbotron::dsp::Sample{1},
        rvrbotron::dsp::Sample{1}};
    std::array<rvrbotron::dsp::Sample, 4> unleveledLeft{};
    std::array<rvrbotron::dsp::Sample, 4> unleveledRight{};
    std::array<rvrbotron::dsp::Sample, 4> leveledLeft{};
    std::array<rvrbotron::dsp::Sample, 4> leveledRight{};
    const rvrbotron::dsp::Sample* unleveledInputs[]{
        unleveledInput.data()};
    rvrbotron::dsp::Sample* unleveledOutputs[]{
        unleveledLeft.data(), unleveledRight.data()};
    rvrbotron::dsp::Sample* leveledOutputs[]{
        leveledLeft.data(), leveledRight.data()};
    unleveledReverb.process(unleveledInputs, 1, unleveledOutputs, 2, 4);
    leveledReverb.process(unleveledInputs, 1, leveledOutputs, 2, 4);
    for (std::size_t frame = 0; frame < 4; ++frame) {
      const auto expectedLeft =
          static_cast<double>(unleveledLeft[frame]) * expectedGain;
      const auto expectedRight =
          static_cast<double>(unleveledRight[frame]) * expectedGain;
      if (!close(leveledLeft[frame], expectedLeft) ||
          !close(leveledRight[frame], expectedRight)) {
        std::cerr << "mainLevelDb: -6 did not scale the Main wet path "
                     "output by its resolved gain\n";
        return 1;
      }
    }

    // An extreme mainLevelDb resolves a mainGain that is a valid finite
    // positive double but is not representable at float precision (see
    // the analogous CLI-boundary check in test_configuration_cli.py).
    // resolveConfig is a public non-JSON entry point too, so this must
    // be rejected there directly as well.
    for (const auto extremeMainLevelDb : {1000.0, -1000.0}) {
      auto extremeComposition = composition;
      extremeComposition.mainLevelDb = extremeMainLevelDb;
      rvrbotron::config::ReverbConfig extremeRequested;
      extremeRequested.formatVersion = 2;
      extremeRequested.seed = 7;
      extremeRequested.composition = std::move(extremeComposition);
      bool extremeRejected = false;
      try {
        static_cast<void>(
            rvrbotron::config::resolveConfig(extremeRequested, 48000, 1));
      } catch (const rvrbotron::HarnessError&) {
        extremeRejected = true;
      }
      if (!extremeRejected) {
        std::cerr << "resolveConfig accepted a mainLevelDb of "
                   << extremeMainLevelDb
                   << " whose gain is not representable at float "
                      "precision\n";
        return 1;
      }
    }
  }

  // The Composition's own dry/wet envelope (issue #114, docs/design/
  // reverb/stages/09-composition.md's "Pre-delay and dry/wet").
  // resolveConfig is a public non-JSON entry point too, mirroring the
  // Main wet path's own direct-construction checks above: dryDb/wetDb/
  // wetOnly set on the empty identity Composition must be rejected
  // there directly.
  for (const auto& emptyField :
       {std::string("dryDb"),
        std::string("wetDb"),
        std::string("wetOnly"),
        std::string("preDelayMs")}) {
    rvrbotron::config::CompositionConfig emptyWithEnvelopeField;
    if (emptyField == "dryDb") {
      emptyWithEnvelopeField.dryDb = -6.0;
    } else if (emptyField == "wetDb") {
      emptyWithEnvelopeField.wetDb = -6.0;
    } else if (emptyField == "wetOnly") {
      emptyWithEnvelopeField.wetOnly = false;
    } else {
      emptyWithEnvelopeField.preDelayMs = 20.0;
    }
    rvrbotron::config::ReverbConfig requested;
    requested.formatVersion = 2;
    requested.composition = emptyWithEnvelopeField;
    bool rejected = false;
    try {
      static_cast<void>(
          rvrbotron::config::resolveConfig(requested, 48000, 1));
    } catch (const rvrbotron::HarnessError&) {
      rejected = true;
    }
    if (!rejected) {
      std::cerr << "resolveConfig accepted " << emptyField
                << " on the empty identity Composition\n";
      return 1;
    }
  }

  // The same rejection holds for validateResolvedConfig called directly
  // on a hand-built ResolvedConfig, not only through resolveConfig: it
  // is a public, non-JSON entry point too (PR review on #131, echoing
  // the same finding raised on #111 for `early`, tested further below).
  // Unlike `early`, the envelope fields are plain scalars rather than
  // std::optional, so a hand-built empty Composition cannot omit them --
  // only set them to something other than ADR-0007's legacy neutral
  // reading, which is exactly what must be rejected.
  for (const auto& nonNeutralField :
       {std::string("dryDb"),
        std::string("dryGain"),
        std::string("wetDb"),
        std::string("wetGain"),
        std::string("wetOnly"),
        std::string("preDelayMs"),
        std::string("preDelaySamples")}) {
    rvrbotron::dsp::ResolvedConfig handBuiltEmptyWithEnvelope;
    handBuiltEmptyWithEnvelope.formatVersion = 2;
    handBuiltEmptyWithEnvelope.sampleRate = 48000;
    if (nonNeutralField == "dryDb") {
      handBuiltEmptyWithEnvelope.composition.dryDb = -6.0;
    } else if (nonNeutralField == "dryGain") {
      handBuiltEmptyWithEnvelope.composition.dryGain = 0.5;
    } else if (nonNeutralField == "wetDb") {
      handBuiltEmptyWithEnvelope.composition.wetDb = -6.0;
    } else if (nonNeutralField == "wetGain") {
      handBuiltEmptyWithEnvelope.composition.wetGain = 0.5;
    } else if (nonNeutralField == "wetOnly") {
      handBuiltEmptyWithEnvelope.composition.wetOnly = false;
    } else if (nonNeutralField == "preDelayMs") {
      handBuiltEmptyWithEnvelope.composition.preDelayMs = 20.0;
    } else {
      handBuiltEmptyWithEnvelope.composition.preDelaySamples = 960;
    }
    bool rejected = false;
    try {
      rvrbotron::config::validateResolvedConfig(handBuiltEmptyWithEnvelope);
    } catch (const rvrbotron::HarnessError&) {
      rejected = true;
    }
    if (!rejected) {
      std::cerr << "validateResolvedConfig accepted a non-neutral "
                << nonNeutralField
                << " on a hand-built empty identity Composition\n";
      return 1;
    }
  }

  // Pre-delay (issue #133, docs/design/reverb/stages/09-composition.md's
  // "Pre-delay and dry/wet"): a single delay before Split, completing
  // the Composition envelope.
  {
    rvrbotron::config::SplitConfig split;
    split.channels = 2;
    split.strategy = rvrbotron::dsp::SplitStrategyType::duplicate;
    split.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;
    rvrbotron::config::DiffuserConfig diffuser;
    diffuser.steps = 1;
    diffuser.totalMs = 1.0;
    auto downmix = referenceSelectDownmixConfig(2);
    rvrbotron::config::CompositionConfig composition;
    composition.stagesSpecified = true;
    composition.stages.emplace_back(split);
    composition.stages.emplace_back(diffuser);
    composition.stages.emplace_back(downmix);

    constexpr std::uint32_t kSampleRate = 48000;
    constexpr double kPreDelayMs = 10.0;
    // 10ms @ 48kHz is exact (480.0), so the nearest-frame rule's rounding
    // is not itself under test here -- a fixture chosen to isolate
    // Pre-delay's own behavior from the rounding formula.
    constexpr std::uint64_t kPreDelaySamples = 480;

    // Nearest-frame resolution.
    auto preDelayComposition = composition;
    preDelayComposition.wetOnly = false;
    preDelayComposition.preDelayMs = kPreDelayMs;
    rvrbotron::config::ReverbConfig preDelayRequested;
    preDelayRequested.formatVersion = 2;
    preDelayRequested.seed = 7;
    preDelayRequested.composition = preDelayComposition;
    const auto preDelayResolved =
        rvrbotron::config::resolveConfig(preDelayRequested, kSampleRate, 2);
    if (preDelayResolved.composition.preDelayMs != kPreDelayMs ||
        preDelayResolved.composition.preDelaySamples != kPreDelaySamples) {
      std::cerr << "preDelayMs: 10 did not resolve the expected "
                   "nearest-frame sample count\n";
      return 1;
    }

    // A non-empty Composition defaults to 0 ms / 0 sample Pre-delay, and
    // zero Pre-delay bypasses delay processing: complete omission and an
    // explicit 0 resolve and render bit-identically.
    rvrbotron::config::ReverbConfig defaultRequested;
    defaultRequested.formatVersion = 2;
    defaultRequested.seed = 7;
    defaultRequested.composition = composition;
    const auto defaultResolved =
        rvrbotron::config::resolveConfig(defaultRequested, kSampleRate, 2);
    if (defaultResolved.composition.preDelayMs != 0.0 ||
        defaultResolved.composition.preDelaySamples != 0) {
      std::cerr << "a non-empty Composition did not default to 0 ms / "
                   "0 sample Pre-delay\n";
      return 1;
    }
    auto explicitZeroComposition = composition;
    explicitZeroComposition.preDelayMs = 0.0;
    rvrbotron::config::ReverbConfig explicitZeroRequested;
    explicitZeroRequested.formatVersion = 2;
    explicitZeroRequested.seed = 7;
    explicitZeroRequested.composition = std::move(explicitZeroComposition);
    const auto explicitZeroResolved = rvrbotron::config::resolveConfig(
        explicitZeroRequested, kSampleRate, 2);

    constexpr std::size_t kFrames = 600; // > kPreDelaySamples (480)
    std::vector<rvrbotron::dsp::Sample> steadyLeft(
        kFrames, rvrbotron::dsp::Sample{1});
    std::vector<rvrbotron::dsp::Sample> steadyRight(
        kFrames, rvrbotron::dsp::Sample{1});
    const rvrbotron::dsp::Sample* steadyInputs[]{
        steadyLeft.data(), steadyRight.data()};

    rvrbotron::dsp::Reverb defaultReverb(defaultResolved);
    rvrbotron::dsp::Reverb explicitZeroReverb(explicitZeroResolved);
    if (defaultReverb.preDelayFrames() != 0 ||
        explicitZeroReverb.preDelayFrames() != 0) {
      std::cerr << "omitted/explicit-zero Pre-delay did not report zero "
                   "preDelayFrames\n";
      return 1;
    }
    std::vector<rvrbotron::dsp::Sample> defaultLeft(kFrames);
    std::vector<rvrbotron::dsp::Sample> defaultRight(kFrames);
    std::vector<rvrbotron::dsp::Sample> explicitZeroLeft(kFrames);
    std::vector<rvrbotron::dsp::Sample> explicitZeroRight(kFrames);
    rvrbotron::dsp::Sample* defaultOutputs[]{
        defaultLeft.data(), defaultRight.data()};
    rvrbotron::dsp::Sample* explicitZeroOutputs[]{
        explicitZeroLeft.data(), explicitZeroRight.data()};
    beginAllocationCount();
    defaultReverb.process(steadyInputs, 2, defaultOutputs, 2, kFrames);
    explicitZeroReverb.process(
        steadyInputs, 2, explicitZeroOutputs, 2, kFrames);
    if (endAllocationCount() != 0) {
      std::cerr << "zero Pre-delay allocated while processing\n";
      return 1;
    }
    if (defaultLeft != explicitZeroLeft || defaultRight != explicitZeroRight) {
      std::cerr << "omitted and explicit-zero Pre-delay did not render "
                   "bit-identically\n";
      return 1;
    }

    // The wet path receives silence for exactly preDelaySamples frames
    // before the (delayed) source reaches Split; the dry signal stays
    // sample-aligned from frame zero. Verified two ways: (1) the "split"
    // Stage capture, by exact equivalence against a reference fed a
    // manually zero-padded input at zero Pre-delay -- rigorous without
    // assuming anything about Split's own internal scaling; (2) frame
    // zero of the final output, where wet is still exact silence (the
    // Diffuser's own step delay is comfortably longer than one frame),
    // isolating dry directly.
    auto referenceComposition = composition;
    referenceComposition.wetOnly = false;
    rvrbotron::config::ReverbConfig referenceRequested;
    referenceRequested.formatVersion = 2;
    referenceRequested.seed = 7;
    referenceRequested.composition = std::move(referenceComposition);
    const auto referenceResolved =
        rvrbotron::config::resolveConfig(referenceRequested, kSampleRate, 2);

    class SplitCaptureCollector final : public rvrbotron::dsp::StageCaptureSink {
    public:
      std::vector<rvrbotron::dsp::Sample> left;
      std::vector<rvrbotron::dsp::Sample> right;

      void captureFrame(
          const rvrbotron::dsp::StageCaptureBoundary boundary,
          const std::uint32_t,
          const rvrbotron::dsp::Sample* const channels,
          const std::size_t channelCount) noexcept override {
        if (boundary != rvrbotron::dsp::StageCaptureBoundary::split) {
          return;
        }
        left.push_back(channels[0]);
        right.push_back(channelCount > 1 ? channels[1] : channels[0]);
      }
    };

    SplitCaptureCollector preDelayCapture;
    rvrbotron::dsp::Reverb preDelayReverb(preDelayResolved, &preDelayCapture);
    std::vector<rvrbotron::dsp::Sample> preDelayLeft(kFrames);
    std::vector<rvrbotron::dsp::Sample> preDelayRight(kFrames);
    rvrbotron::dsp::Sample* preDelayOutputs[]{
        preDelayLeft.data(), preDelayRight.data()};
    if (preDelayReverb.preDelayFrames() != kPreDelaySamples) {
      std::cerr << "Reverb::preDelayFrames() did not report the resolved "
                   "Pre-delay\n";
      return 1;
    }
    preDelayReverb.process(steadyInputs, 2, preDelayOutputs, 2, kFrames);

    std::vector<rvrbotron::dsp::Sample> paddedLeft(kFrames, rvrbotron::dsp::Sample{0});
    std::vector<rvrbotron::dsp::Sample> paddedRight(kFrames, rvrbotron::dsp::Sample{0});
    std::copy(
        steadyLeft.begin(),
        steadyLeft.end() - static_cast<std::ptrdiff_t>(kPreDelaySamples),
        paddedLeft.begin() + static_cast<std::ptrdiff_t>(kPreDelaySamples));
    std::copy(
        steadyRight.begin(),
        steadyRight.end() - static_cast<std::ptrdiff_t>(kPreDelaySamples),
        paddedRight.begin() + static_cast<std::ptrdiff_t>(kPreDelaySamples));
    const rvrbotron::dsp::Sample* paddedInputs[]{
        paddedLeft.data(), paddedRight.data()};

    SplitCaptureCollector referenceCapture;
    rvrbotron::dsp::Reverb referenceReverb(
        referenceResolved, &referenceCapture);
    std::vector<rvrbotron::dsp::Sample> referenceLeft(kFrames);
    std::vector<rvrbotron::dsp::Sample> referenceRight(kFrames);
    rvrbotron::dsp::Sample* referenceOutputs[]{
        referenceLeft.data(), referenceRight.data()};
    referenceReverb.process(paddedInputs, 2, referenceOutputs, 2, kFrames);

    if (preDelayReverb.tailBudgetFrames() != referenceReverb.tailBudgetFrames()) {
      std::cerr << "Pre-delay changed tailBudgetFrames, which must keep "
                   "its existing decay-only meaning\n";
      return 1;
    }
    for (std::size_t frame = 0; frame < kPreDelaySamples; ++frame) {
      if (preDelayCapture.left[frame] != rvrbotron::dsp::Sample{0} ||
          preDelayCapture.right[frame] != rvrbotron::dsp::Sample{0}) {
        std::cerr << "the wet path was not exact silence during the "
                     "resolved Pre-delay interval at frame " << frame
                  << '\n';
        return 1;
      }
    }
    if (preDelayCapture.left != referenceCapture.left ||
        preDelayCapture.right != referenceCapture.right) {
      std::cerr << "Pre-delay did not reproduce a manually zero-padded, "
                   "zero-Pre-delay reference at the split Stage capture\n";
      return 1;
    }
    // Frame zero: wet is still exact silence (the Diffuser's own step
    // delay exceeds one frame), so output equals dry alone -- proving
    // the dry signal is sample-aligned from frame zero, not delayed
    // alongside wet.
    if (!close(preDelayLeft[0], 1.0) || !close(preDelayRight[0], 1.0)) {
      std::cerr << "dry was not sample-aligned from frame zero under a "
                   "nonzero Pre-delay\n";
      return 1;
    }

    // Representative renders are identical across legal block sizes,
    // including a Pre-delay boundary (480 samples) falling within and
    // across block boundaries -- 7 shares no common factor with 480.
    SplitCaptureCollector blockedCapture;
    rvrbotron::dsp::Reverb blockedReverb(preDelayResolved, &blockedCapture);
    std::vector<rvrbotron::dsp::Sample> blockedLeft(kFrames);
    std::vector<rvrbotron::dsp::Sample> blockedRight(kFrames);
    std::size_t rendered = 0;
    while (rendered < kFrames) {
      const auto blockFrames = std::min<std::size_t>(7, kFrames - rendered);
      const rvrbotron::dsp::Sample* blockInputs[]{
          steadyLeft.data() + rendered, steadyRight.data() + rendered};
      rvrbotron::dsp::Sample* blockOutputs[]{
          blockedLeft.data() + rendered, blockedRight.data() + rendered};
      blockedReverb.process(blockInputs, 2, blockOutputs, 2, blockFrames);
      rendered += blockFrames;
    }
    if (blockedLeft != preDelayLeft || blockedRight != preDelayRight) {
      std::cerr << "Pre-delay output changed when rendered in 7-frame "
                   "blocks instead of one call\n";
      return 1;
    }

    // The reserved timeline (preDelayFrames plus tailBudgetFrames) does
    // not depend on whether the wet branches are actually enabled:
    // both are resolved from the Composition's own structure, not from
    // whether Main/Early happen to be silenced -- so total output
    // length still reserves them even when both wet branches are
    // disabled.
    auto bothDisabledComposition = composition;
    bothDisabledComposition.mainEnabled = false;
    bothDisabledComposition.preDelayMs = kPreDelayMs;
    rvrbotron::config::ReverbConfig bothDisabledRequested;
    bothDisabledRequested.formatVersion = 2;
    bothDisabledRequested.seed = 7;
    bothDisabledRequested.composition = std::move(bothDisabledComposition);
    const auto bothDisabledResolved = rvrbotron::config::resolveConfig(
        bothDisabledRequested, kSampleRate, 2);
    rvrbotron::dsp::Reverb bothDisabledReverb(bothDisabledResolved);
    if (bothDisabledReverb.preDelayFrames() != kPreDelaySamples ||
        bothDisabledReverb.tailBudgetFrames() !=
            preDelayReverb.tailBudgetFrames()) {
      std::cerr << "a disabled Main wet path (and no Early Reflections) "
                   "changed the reserved Pre-delay/Tail-budget timeline\n";
      return 1;
    }

    // DSP-owned memory grows to include Pre-delay storage: exactly the
    // DelayLine object's own footprint plus its reported owned storage
    // -- checked against an independently constructed reference
    // DelayLine (the same public API Reverb itself uses), not merely a
    // "grew by some positive amount" bound, which a much larger audio
    // buffer would satisfy even if the smaller sizeof(DelayLine) term
    // were dropped entirely. Reverb holds Pre-delay's DelayLine behind
    // a unique_ptr, unlike FeedbackLoop's embedded-by-value DelayLine
    // (whose own sizeof(*this) already covers it), so sizeof(DelayLine)
    // is not otherwise counted anywhere in Reverb::ownedBytes().
    const rvrbotron::dsp::DelayLine referencePreDelayLine(
        std::vector<std::uint64_t>(2, kPreDelaySamples),
        std::vector<std::uint64_t>(2, kPreDelaySamples));
    const auto expectedPreDelayBytes =
        sizeof(rvrbotron::dsp::DelayLine) +
        referencePreDelayLine.ownedStorageBytes();
    if (preDelayReverb.ownedBytes() - defaultReverb.ownedBytes() !=
        expectedPreDelayBytes) {
      std::cerr << "DSP-owned memory did not grow by exactly the "
                   "Pre-delay DelayLine's own object footprint plus its "
                   "owned storage\n";
      return 1;
    }

    // A longer configured delay grows DSP-owned memory further, proving
    // the audio buffer itself scales rather than a fixed per-instance
    // overhead.
    auto longerPreDelayComposition = composition;
    longerPreDelayComposition.preDelayMs = kPreDelayMs * 2.0;
    rvrbotron::config::ReverbConfig longerPreDelayRequested;
    longerPreDelayRequested.formatVersion = 2;
    longerPreDelayRequested.seed = 7;
    longerPreDelayRequested.composition = std::move(longerPreDelayComposition);
    const auto longerPreDelayResolved = rvrbotron::config::resolveConfig(
        longerPreDelayRequested, kSampleRate, 2);
    rvrbotron::dsp::Reverb longerPreDelayReverb(longerPreDelayResolved);
    if (longerPreDelayReverb.ownedBytes() <= preDelayReverb.ownedBytes()) {
      std::cerr << "DSP-owned memory did not grow with a longer "
                   "configured Pre-delay\n";
      return 1;
    }

    // Out-of-range and non-finite preDelayMs are rejected.
    for (const auto invalidPreDelayMs :
         {-1.0,
          201.0,
          std::numeric_limits<double>::quiet_NaN(),
          std::numeric_limits<double>::infinity()}) {
      auto invalidComposition = composition;
      invalidComposition.preDelayMs = invalidPreDelayMs;
      rvrbotron::config::ReverbConfig invalidRequested;
      invalidRequested.formatVersion = 2;
      invalidRequested.seed = 7;
      invalidRequested.composition = std::move(invalidComposition);
      bool invalidRejected = false;
      try {
        static_cast<void>(
            rvrbotron::config::resolveConfig(invalidRequested, kSampleRate, 2));
      } catch (const rvrbotron::HarnessError&) {
        invalidRejected = true;
      }
      if (!invalidRejected) {
        std::cerr << "resolveConfig accepted an out-of-range or "
                     "non-finite preDelayMs: " << invalidPreDelayMs << '\n';
        return 1;
      }
    }

    // A preDelaySamples inconsistent with its preDelayMs is rejected by
    // the direct validator, naming the derivation it violates.
    auto inconsistentResolved = preDelayResolved;
    inconsistentResolved.composition.preDelaySamples = kPreDelaySamples + 1;
    bool inconsistentRejected = false;
    try {
      rvrbotron::config::validateResolvedConfig(inconsistentResolved);
    } catch (const rvrbotron::HarnessError& error) {
      inconsistentRejected = true;
      if (!error.location().has_value() ||
          error.location()->find("/composition/preDelaySamples") ==
              std::string::npos) {
        std::cerr << "an inconsistent preDelaySamples was not rejected "
                     "at its own path: "
                  << error.location().value_or("<none>") << '\n';
        return 1;
      }
    }
    if (!inconsistentRejected) {
      std::cerr << "validateResolvedConfig accepted a preDelaySamples "
                   "inconsistent with its preDelayMs\n";
      return 1;
    }
  }

  {
    // A minimal Diffuser-only Main wet path (mirroring the Main wet
    // path block above): its own Diffusion Step delay line is long
    // enough that frame 0 of its output is still exact silence, so
    // frame 0 isolates the dry contribution completely regardless of
    // what the wet path eventually produces.
    rvrbotron::config::SplitConfig split;
    split.channels = 2;
    split.strategy = rvrbotron::dsp::SplitStrategyType::duplicate;
    split.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;
    rvrbotron::config::DiffuserConfig diffuser;
    diffuser.steps = 1;
    diffuser.totalMs = 1.0;
    auto downmix = referenceSelectDownmixConfig(2);
    rvrbotron::config::CompositionConfig composition;
    composition.stagesSpecified = true;
    composition.stages.emplace_back(split);
    composition.stages.emplace_back(diffuser);
    composition.stages.emplace_back(downmix);
    rvrbotron::config::ReverbConfig requested;
    requested.formatVersion = 2;
    requested.seed = 7;
    requested.composition = composition;

    // A non-empty Composition exposes documented dryDb/wetDb/wetOnly
    // defaults: 0 dB, 0 dB, and wet-only -- reproducing every existing
    // non-empty format-v2 request's wet-only rendering exactly.
    const auto defaultResolved =
        rvrbotron::config::resolveConfig(requested, 48000, 2);
    if (defaultResolved.composition.dryDb != 0.0 ||
        defaultResolved.composition.dryGain != 1.0 ||
        defaultResolved.composition.wetDb != 0.0 ||
        defaultResolved.composition.wetGain != 1.0 ||
        !defaultResolved.composition.wetOnly) {
      std::cerr << "a non-empty Composition did not default to 0 dB dry/"
                   "wet, 1.0 dry/wet gain, and wet-only\n";
      return 1;
    }

    // Explicit neutral fields resolve identically to complete omission
    // (docs/design/reverb/stages/09-composition.md's Composition
    // envelope): the same construction/render below is exercised twice.
    auto explicitNeutralComposition = composition;
    explicitNeutralComposition.dryDb = 0.0;
    explicitNeutralComposition.wetDb = 0.0;
    explicitNeutralComposition.wetOnly = true;
    rvrbotron::config::ReverbConfig explicitNeutralRequested;
    explicitNeutralRequested.formatVersion = 2;
    explicitNeutralRequested.seed = 7;
    explicitNeutralRequested.composition =
        std::move(explicitNeutralComposition);
    const auto explicitNeutralResolved = rvrbotron::config::resolveConfig(
        explicitNeutralRequested, 48000, 2);

    std::array<rvrbotron::dsp::Sample, 1> stereoLeftInput{
        rvrbotron::dsp::Sample{1}};
    std::array<rvrbotron::dsp::Sample, 1> stereoRightInput{
        rvrbotron::dsp::Sample{2}};
    const rvrbotron::dsp::Sample* stereoInputs[]{
        stereoLeftInput.data(), stereoRightInput.data()};

    rvrbotron::dsp::Reverb defaultReverb(defaultResolved);
    rvrbotron::dsp::Reverb explicitNeutralReverb(explicitNeutralResolved);
    std::array<rvrbotron::dsp::Sample, 1> defaultLeft{};
    std::array<rvrbotron::dsp::Sample, 1> defaultRight{};
    std::array<rvrbotron::dsp::Sample, 1> explicitNeutralLeft{};
    std::array<rvrbotron::dsp::Sample, 1> explicitNeutralRight{};
    rvrbotron::dsp::Sample* defaultOutputs[]{
        defaultLeft.data(), defaultRight.data()};
    rvrbotron::dsp::Sample* explicitNeutralOutputs[]{
        explicitNeutralLeft.data(), explicitNeutralRight.data()};
    beginAllocationCount();
    defaultReverb.process(stereoInputs, 2, defaultOutputs, 2, 1);
    explicitNeutralReverb.process(
        stereoInputs, 2, explicitNeutralOutputs, 2, 1);
    if (endAllocationCount() != 0) {
      std::cerr << "the dry/wet envelope allocated while processing\n";
      return 1;
    }
    // wetOnly (default and explicit) gates dry to exact zero; frame 0's
    // Diffuser output is exact silence, so exact zero here proves dry
    // never leaked in, not merely that the wet path happened to be
    // silent.
    if (defaultLeft[0] != rvrbotron::dsp::Sample{0} ||
        defaultRight[0] != rvrbotron::dsp::Sample{0} ||
        defaultLeft[0] != explicitNeutralLeft[0] ||
        defaultRight[0] != explicitNeutralRight[0]) {
      std::cerr << "omitted and explicit neutral dry/wet fields did not "
                   "both render exact wet-only silence at frame 0\n";
      return 1;
    }

    // wetOnly: false maps stereo dry input channel-for-channel and
    // applies dryGain, with no hidden crossfade, normalization, or
    // energy compensation -- frame 0 isolates this completely.
    auto insertComposition = composition;
    insertComposition.wetOnly = false;
    insertComposition.dryDb = -6.0;
    rvrbotron::config::ReverbConfig insertRequested;
    insertRequested.formatVersion = 2;
    insertRequested.seed = 7;
    insertRequested.composition = std::move(insertComposition);
    const auto insertResolved =
        rvrbotron::config::resolveConfig(insertRequested, 48000, 2);
    const auto expectedDryGain = std::pow(10.0, -6.0 / 20.0);
    if (std::abs(insertResolved.composition.dryGain - expectedDryGain) >
        1e-9) {
      std::cerr << "dryDb: -6 did not resolve the expected linear gain\n";
      return 1;
    }
    rvrbotron::dsp::Reverb insertReverb(insertResolved);
    std::array<rvrbotron::dsp::Sample, 1> insertLeft{};
    std::array<rvrbotron::dsp::Sample, 1> insertRight{};
    rvrbotron::dsp::Sample* insertOutputs[]{
        insertLeft.data(), insertRight.data()};
    beginAllocationCount();
    insertReverb.process(stereoInputs, 2, insertOutputs, 2, 1);
    if (endAllocationCount() != 0) {
      std::cerr << "wetOnly: false dry mapping allocated while "
                   "processing\n";
      return 1;
    }
    if (!close(insertLeft[0], 1.0 * expectedDryGain) ||
        !close(insertRight[0], 2.0 * expectedDryGain)) {
      std::cerr << "wetOnly: false did not map stereo dry input "
                   "channel-for-channel scaled by dryGain\n";
      return 1;
    }

    // Mono dry input duplicates to both output channels at the same
    // gain, without energy compensation (i.e. not divided by sqrt(2)).
    // Resolved separately against a mono input-channel count: Reverb's
    // own inputChannelCount is a construction-time contract (Split's
    // resolved inputChannels), not something process() renegotiates per
    // call.
    auto monoInsertRequested = insertRequested;
    const auto monoInsertResolved =
        rvrbotron::config::resolveConfig(monoInsertRequested, 48000, 1);
    std::array<rvrbotron::dsp::Sample, 1> monoInput{
        rvrbotron::dsp::Sample{3}};
    const rvrbotron::dsp::Sample* monoInputs[]{monoInput.data()};
    rvrbotron::dsp::Reverb monoInsertReverb(monoInsertResolved);
    std::array<rvrbotron::dsp::Sample, 1> monoLeft{};
    std::array<rvrbotron::dsp::Sample, 1> monoRight{};
    rvrbotron::dsp::Sample* monoOutputs[]{
        monoLeft.data(), monoRight.data()};
    monoInsertReverb.process(monoInputs, 1, monoOutputs, 2, 1);
    if (!close(monoLeft[0], 3.0 * expectedDryGain) ||
        !close(monoRight[0], 3.0 * expectedDryGain)) {
      std::cerr << "mono dry input was not duplicated to both output "
                   "channels at dryGain without energy compensation\n";
      return 1;
    }

    // The global wetGain multiplies the complete Wet sum exactly once:
    // rendered with dry disabled (wetOnly true) so the comparison
    // isolates the Wet sum, over enough frames that the Diffuser's own
    // delay line has produced genuinely nonzero output.
    constexpr std::size_t kWetGainTestFrames = 256;
    auto wetReferenceComposition = composition;
    wetReferenceComposition.wetOnly = true;
    rvrbotron::config::ReverbConfig wetReferenceRequested;
    wetReferenceRequested.formatVersion = 2;
    wetReferenceRequested.seed = 7;
    wetReferenceRequested.composition = std::move(wetReferenceComposition);
    const auto wetReferenceResolved = rvrbotron::config::resolveConfig(
        wetReferenceRequested, 48000, 2);

    auto wetLeveledComposition = composition;
    wetLeveledComposition.wetOnly = true;
    wetLeveledComposition.wetDb = -6.0;
    rvrbotron::config::ReverbConfig wetLeveledRequested;
    wetLeveledRequested.formatVersion = 2;
    wetLeveledRequested.seed = 7;
    wetLeveledRequested.composition = std::move(wetLeveledComposition);
    const auto wetLeveledResolved = rvrbotron::config::resolveConfig(
        wetLeveledRequested, 48000, 2);
    const auto expectedWetGain = std::pow(10.0, -6.0 / 20.0);
    if (std::abs(wetLeveledResolved.composition.wetGain - expectedWetGain) >
        1e-9) {
      std::cerr << "wetDb: -6 did not resolve the expected linear gain\n";
      return 1;
    }

    std::vector<rvrbotron::dsp::Sample> steadyLeftIn(
        kWetGainTestFrames, rvrbotron::dsp::Sample{1});
    std::vector<rvrbotron::dsp::Sample> steadyRightIn(
        kWetGainTestFrames, rvrbotron::dsp::Sample{1});
    const rvrbotron::dsp::Sample* steadyInputs[]{
        steadyLeftIn.data(), steadyRightIn.data()};
    std::vector<rvrbotron::dsp::Sample> wetReferenceLeft(kWetGainTestFrames);
    std::vector<rvrbotron::dsp::Sample> wetReferenceRight(kWetGainTestFrames);
    std::vector<rvrbotron::dsp::Sample> wetLeveledLeft(kWetGainTestFrames);
    std::vector<rvrbotron::dsp::Sample> wetLeveledRight(kWetGainTestFrames);
    rvrbotron::dsp::Reverb wetReferenceReverb(wetReferenceResolved);
    rvrbotron::dsp::Reverb wetLeveledReverb(wetLeveledResolved);
    rvrbotron::dsp::Sample* wetReferenceOutputs[]{
        wetReferenceLeft.data(), wetReferenceRight.data()};
    rvrbotron::dsp::Sample* wetLeveledOutputs[]{
        wetLeveledLeft.data(), wetLeveledRight.data()};
    wetReferenceReverb.process(
        steadyInputs, 2, wetReferenceOutputs, 2, kWetGainTestFrames);
    wetLeveledReverb.process(
        steadyInputs, 2, wetLeveledOutputs, 2, kWetGainTestFrames);

    bool sawNonzeroWet = false;
    for (std::size_t frame = 0; frame < kWetGainTestFrames; ++frame) {
      if (wetReferenceLeft[frame] != rvrbotron::dsp::Sample{0} ||
          wetReferenceRight[frame] != rvrbotron::dsp::Sample{0}) {
        sawNonzeroWet = true;
      }
      const auto expectedLeft =
          static_cast<double>(wetReferenceLeft[frame]) * expectedWetGain;
      const auto expectedRight =
          static_cast<double>(wetReferenceRight[frame]) * expectedWetGain;
      if (!close(wetLeveledLeft[frame], expectedLeft) ||
          !close(wetLeveledRight[frame], expectedRight)) {
        std::cerr << "wetDb: -6 did not scale the Wet sum by its "
                     "resolved gain at frame "
                  << frame << '\n';
        return 1;
      }
    }
    if (!sawNonzeroWet) {
      std::cerr << "the Diffuser produced no nonzero output within "
                << kWetGainTestFrames
                << " frames -- the wetGain scaling comparison above did "
                   "not exercise genuinely nonzero Wet sum values\n";
      return 1;
    }

    // Final output is reconstructable from the dry input, the Wet sum
    // (here, the wet-only reference render above), and the Resolved
    // envelope values: dry contribution plus the scaled Wet sum, fixed
    // order (docs/design/reverb/stages/09-composition.md).
    auto combinedComposition = composition;
    combinedComposition.wetOnly = false;
    combinedComposition.dryDb = -6.0;
    combinedComposition.wetDb = -3.0;
    rvrbotron::config::ReverbConfig combinedRequested;
    combinedRequested.formatVersion = 2;
    combinedRequested.seed = 7;
    combinedRequested.composition = std::move(combinedComposition);
    const auto combinedResolved =
        rvrbotron::config::resolveConfig(combinedRequested, 48000, 2);

    auto combinedWetReferenceComposition = composition;
    combinedWetReferenceComposition.wetOnly = true;
    combinedWetReferenceComposition.wetDb = -3.0;
    rvrbotron::config::ReverbConfig combinedWetReferenceRequested;
    combinedWetReferenceRequested.formatVersion = 2;
    combinedWetReferenceRequested.seed = 7;
    combinedWetReferenceRequested.composition =
        std::move(combinedWetReferenceComposition);
    const auto combinedWetReferenceResolved =
        rvrbotron::config::resolveConfig(
            combinedWetReferenceRequested, 48000, 2);

    rvrbotron::dsp::Reverb combinedReverb(combinedResolved);
    rvrbotron::dsp::Reverb combinedWetReferenceReverb(
        combinedWetReferenceResolved);
    std::vector<rvrbotron::dsp::Sample> combinedLeft(kWetGainTestFrames);
    std::vector<rvrbotron::dsp::Sample> combinedRight(kWetGainTestFrames);
    std::vector<rvrbotron::dsp::Sample> combinedWetReferenceLeft(
        kWetGainTestFrames);
    std::vector<rvrbotron::dsp::Sample> combinedWetReferenceRight(
        kWetGainTestFrames);
    rvrbotron::dsp::Sample* combinedOutputs[]{
        combinedLeft.data(), combinedRight.data()};
    rvrbotron::dsp::Sample* combinedWetReferenceOutputs[]{
        combinedWetReferenceLeft.data(), combinedWetReferenceRight.data()};
    combinedReverb.process(
        steadyInputs, 2, combinedOutputs, 2, kWetGainTestFrames);
    combinedWetReferenceReverb.process(
        steadyInputs,
        2,
        combinedWetReferenceOutputs,
        2,
        kWetGainTestFrames);
    const auto expectedCombinedDryGain = std::pow(10.0, -6.0 / 20.0);
    for (std::size_t frame = 0; frame < kWetGainTestFrames; ++frame) {
      const auto expectedLeft =
          1.0 * expectedCombinedDryGain +
          static_cast<double>(combinedWetReferenceLeft[frame]);
      const auto expectedRight =
          1.0 * expectedCombinedDryGain +
          static_cast<double>(combinedWetReferenceRight[frame]);
      if (!close(combinedLeft[frame], expectedLeft) ||
          !close(combinedRight[frame], expectedRight)) {
        std::cerr << "final output was not the fixed-order sum of the "
                     "dry contribution and the scaled Wet sum at frame "
                  << frame << '\n';
        return 1;
      }
    }

    // Dry contribution is exact zero for every frame after source EOF:
    // the CLI feeds zeroed input during Tail-budget drain (see
    // main.cpp), so a Reverb fed zero input must contribute exact dry
    // zero regardless of wetOnly, leaving only the continuing Wet sum.
    std::vector<rvrbotron::dsp::Sample> silentInput(
        kWetGainTestFrames, rvrbotron::dsp::Sample{0});
    const rvrbotron::dsp::Sample* silentInputs[]{
        silentInput.data(), silentInput.data()};
    auto postEofComposition = composition;
    postEofComposition.wetOnly = false;
    postEofComposition.dryDb = -6.0;
    rvrbotron::config::ReverbConfig postEofRequested;
    postEofRequested.formatVersion = 2;
    postEofRequested.seed = 7;
    postEofRequested.composition = std::move(postEofComposition);
    const auto postEofResolved =
        rvrbotron::config::resolveConfig(postEofRequested, 48000, 2);
    rvrbotron::dsp::Reverb postEofReverb(postEofResolved);
    rvrbotron::dsp::Reverb postEofWetReferenceReverb(wetReferenceResolved);
    std::vector<rvrbotron::dsp::Sample> postEofLeft(kWetGainTestFrames);
    std::vector<rvrbotron::dsp::Sample> postEofRight(kWetGainTestFrames);
    std::vector<rvrbotron::dsp::Sample> postEofWetReferenceLeft(
        kWetGainTestFrames);
    std::vector<rvrbotron::dsp::Sample> postEofWetReferenceRight(
        kWetGainTestFrames);
    rvrbotron::dsp::Sample* postEofOutputs[]{
        postEofLeft.data(), postEofRight.data()};
    rvrbotron::dsp::Sample* postEofWetReferenceOutputs[]{
        postEofWetReferenceLeft.data(), postEofWetReferenceRight.data()};
    postEofReverb.process(
        silentInputs, 2, postEofOutputs, 2, kWetGainTestFrames);
    postEofWetReferenceReverb.process(
        silentInputs,
        2,
        postEofWetReferenceOutputs,
        2,
        kWetGainTestFrames);
    for (std::size_t frame = 0; frame < kWetGainTestFrames; ++frame) {
      if (postEofLeft[frame] != postEofWetReferenceLeft[frame] ||
          postEofRight[frame] != postEofWetReferenceRight[frame]) {
        std::cerr << "an enabled dry path with zero (post-EOF) input did "
                     "not contribute exact dry zero at frame " << frame
                  << '\n';
        return 1;
      }
    }

    // An extreme dryDb/wetDb resolves a gain that is a valid finite
    // positive double but is not representable at float precision,
    // mirroring mainLevelDb's own extreme-value check above.
    for (const auto extremeField : {"dryDb", "wetDb"}) {
      for (const auto extremeLevel : {1000.0, -1000.0}) {
        auto extremeComposition = composition;
        if (std::string(extremeField) == "dryDb") {
          extremeComposition.dryDb = extremeLevel;
        } else {
          extremeComposition.wetDb = extremeLevel;
        }
        rvrbotron::config::ReverbConfig extremeRequested;
        extremeRequested.formatVersion = 2;
        extremeRequested.seed = 7;
        extremeRequested.composition = std::move(extremeComposition);
        bool extremeRejected = false;
        try {
          static_cast<void>(
              rvrbotron::config::resolveConfig(extremeRequested, 48000, 2));
        } catch (const rvrbotron::HarnessError&) {
          extremeRejected = true;
        }
        if (!extremeRejected) {
          std::cerr << "resolveConfig accepted a " << extremeField << " of "
                     << extremeLevel
                     << " whose gain is not representable at float "
                        "precision\n";
          return 1;
        }
      }
    }
  }

  // The parallel Early Reflections branch (issue #111, docs/design/
  // reverb/stages/07-early-reflections.md and docs/design/reverb/stages/
  // 09-composition.md). resolveConfig is a public non-JSON entry point
  // too, mirroring the Main wet path's own direct-construction checks
  // above.
  {
    rvrbotron::config::SplitConfig split;
    split.channels = 2;
    split.strategy = rvrbotron::dsp::SplitStrategyType::duplicate;
    split.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;
    rvrbotron::config::DiffuserConfig diffuser;
    diffuser.steps = 1;
    diffuser.totalMs = 1.0;

    rvrbotron::config::CompositionConfig diffuserOnlyComposition;
    diffuserOnlyComposition.stagesSpecified = true;
    diffuserOnlyComposition.stages.emplace_back(split);
    diffuserOnlyComposition.stages.emplace_back(diffuser);
    diffuserOnlyComposition.stages.emplace_back(referenceSelectDownmixConfig(2));

    // Early Reflections on the empty identity Composition is rejected
    // directly, mirroring mainEnabled/mainLevelDb (#109).
    {
      rvrbotron::config::EarlyConfig earlyOnEmpty;
      earlyOnEmpty.taps =
          std::vector<rvrbotron::config::EarlyTapConfig>{{0}};
      rvrbotron::config::CompositionConfig emptyWithEarly;
      emptyWithEarly.early = earlyOnEmpty;
      rvrbotron::config::ReverbConfig requested;
      requested.formatVersion = 2;
      requested.composition = emptyWithEarly;
      bool rejected = false;
      try {
        static_cast<void>(
            rvrbotron::config::resolveConfig(requested, 48000, 1));
      } catch (const rvrbotron::HarnessError&) {
        rejected = true;
      }
      if (!rejected) {
        std::cerr << "resolveConfig accepted composition.early on the "
                     "empty identity Composition\n";
        return 1;
      }
    }

    // The same rejection holds for validateResolvedConfig called
    // directly on a hand-built ResolvedConfig, not only through
    // resolveConfig: it is a public, non-JSON entry point too (PR review
    // on #111), and its own empty-stages early return must not let a
    // populated `early` slip past unrejected.
    {
      rvrbotron::dsp::ResolvedConfig handBuiltEmptyWithEarly;
      handBuiltEmptyWithEarly.formatVersion = 2;
      handBuiltEmptyWithEarly.sampleRate = 48000;
      rvrbotron::dsp::ResolvedEarlyReflections handBuiltEarly;
      handBuiltEarly.taps.push_back({0});
      handBuiltEmptyWithEarly.composition.early = handBuiltEarly;
      bool rejected = false;
      try {
        rvrbotron::config::validateResolvedConfig(handBuiltEmptyWithEarly);
      } catch (const rvrbotron::HarnessError&) {
        rejected = true;
      }
      if (!rejected) {
        std::cerr << "validateResolvedConfig accepted composition.early on "
                     "a hand-built empty identity Composition\n";
        return 1;
      }
    }

    // Early Reflections without a Diffuser are rejected: a Feedback-
    // Loop-only Main wet path has no source for a tap.
    {
      rvrbotron::config::FeedbackLoopConfig loop;
      loop.delayMinMs = 5.0;
      loop.delayMaxMs = 10.0;
      loop.rt60Sec = 0.1;
      rvrbotron::config::CompositionConfig loopOnlyComposition;
      loopOnlyComposition.stagesSpecified = true;
      loopOnlyComposition.stages.emplace_back(split);
      loopOnlyComposition.stages.emplace_back(loop);
      loopOnlyComposition.stages.emplace_back(
          referenceSelectDownmixConfig(2));
      rvrbotron::config::EarlyConfig earlyWithoutDiffuser;
      earlyWithoutDiffuser.taps =
          std::vector<rvrbotron::config::EarlyTapConfig>{{0}};
      loopOnlyComposition.early = earlyWithoutDiffuser;
      rvrbotron::config::ReverbConfig requested;
      requested.formatVersion = 2;
      requested.seed = 3;
      requested.composition = loopOnlyComposition;
      bool rejected = false;
      try {
        static_cast<void>(
            rvrbotron::config::resolveConfig(requested, 48000, 1));
      } catch (const rvrbotron::HarnessError&) {
        rejected = true;
      }
      if (!rejected) {
        std::cerr << "resolveConfig accepted Early Reflections without a "
                     "Diffuser\n";
        return 1;
      }
    }

    // Branch controls without any tap are rejected: they could not
    // affect sound.
    {
      rvrbotron::config::EarlyConfig earlyWithoutTaps;
      earlyWithoutTaps.enabled = false;
      auto composition = diffuserOnlyComposition;
      composition.early = earlyWithoutTaps;
      rvrbotron::config::ReverbConfig requested;
      requested.formatVersion = 2;
      requested.seed = 3;
      requested.composition = composition;
      bool rejected = false;
      try {
        static_cast<void>(
            rvrbotron::config::resolveConfig(requested, 48000, 1));
      } catch (const rvrbotron::HarnessError&) {
        rejected = true;
      }
      if (!rejected) {
        std::cerr << "resolveConfig accepted Early Reflections controls "
                     "without any tap\n";
        return 1;
      }
    }

    // Duplicate tap indices are rejected (issue #112).
    {
      rvrbotron::config::EarlyConfig earlyDuplicateTaps;
      earlyDuplicateTaps.taps =
          std::vector<rvrbotron::config::EarlyTapConfig>{{0}, {0}};
      auto composition = diffuserOnlyComposition;
      composition.early = earlyDuplicateTaps;
      rvrbotron::config::ReverbConfig requested;
      requested.formatVersion = 2;
      requested.seed = 3;
      requested.composition = composition;
      bool rejected = false;
      try {
        static_cast<void>(
            rvrbotron::config::resolveConfig(requested, 48000, 1));
      } catch (const rvrbotron::HarnessError&) {
        rejected = true;
      }
      if (!rejected) {
        std::cerr << "resolveConfig accepted duplicate Early tap indices\n";
        return 1;
      }
    }

    // An out-of-range tap stepIndex is rejected: this Diffuser has one
    // step (index 0), so index 1 does not exist.
    {
      rvrbotron::config::EarlyConfig earlyOutOfRange;
      earlyOutOfRange.taps =
          std::vector<rvrbotron::config::EarlyTapConfig>{{1}};
      auto composition = diffuserOnlyComposition;
      composition.early = earlyOutOfRange;
      rvrbotron::config::ReverbConfig requested;
      requested.formatVersion = 2;
      requested.seed = 3;
      requested.composition = composition;
      bool rejected = false;
      try {
        static_cast<void>(
            rvrbotron::config::resolveConfig(requested, 48000, 1));
      } catch (const rvrbotron::HarnessError&) {
        rejected = true;
      }
      if (!rejected) {
        std::cerr << "resolveConfig accepted an out-of-range Early tap "
                     "stepIndex\n";
        return 1;
      }
    }

    // A non-empty Early branch defaults to enabled, 0 dB level, and a
    // `select` Downmix of Channels 0/1 -- unlike the Main Downmix's own
    // `select`, which has no implicit Channel choice (issue #107).
    rvrbotron::config::EarlyConfig earlyDefault;
    earlyDefault.taps = std::vector<rvrbotron::config::EarlyTapConfig>{{0}};
    auto diffuserOnlyWithEarly = diffuserOnlyComposition;
    diffuserOnlyWithEarly.early = earlyDefault;
    rvrbotron::config::ReverbConfig diffuserOnlyRequested;
    diffuserOnlyRequested.formatVersion = 2;
    diffuserOnlyRequested.seed = 11;
    diffuserOnlyRequested.composition = diffuserOnlyWithEarly;
    const auto diffuserOnlyResolved =
        rvrbotron::config::resolveConfig(diffuserOnlyRequested, 48000, 1);
    if (!diffuserOnlyResolved.composition.early.has_value()) {
      std::cerr << "a non-empty Early branch did not resolve\n";
      return 1;
    }
    const auto& resolvedEarlyDefault =
        *diffuserOnlyResolved.composition.early;
    if (!resolvedEarlyDefault.enabled ||
        resolvedEarlyDefault.levelDb != 0.0 ||
        resolvedEarlyDefault.gain != 1.0 ||
        resolvedEarlyDefault.taps.size() != 1 ||
        resolvedEarlyDefault.taps[0].stepIndex != 0 ||
        resolvedEarlyDefault.downmix.strategy !=
            rvrbotron::dsp::DownmixStrategy::select ||
        resolvedEarlyDefault.downmix.leftChannel != 0U ||
        resolvedEarlyDefault.downmix.rightChannel != 1U ||
        resolvedEarlyDefault.downmix.alignment !=
            rvrbotron::dsp::DownmixAlignment::aligned) {
      std::cerr << "a non-empty Early branch did not default to enabled/"
                   "0 dB/select Channels 0-1/aligned\n";
      return 1;
    }

    // At N=1, the default Early Downmix duplicates Channel 0 to mono,
    // exactly like every other `select` Downmix with an omitted
    // rightChannel.
    {
      rvrbotron::config::SplitConfig monoSplit;
      monoSplit.channels = 1;
      monoSplit.strategy = rvrbotron::dsp::SplitStrategyType::duplicate;
      monoSplit.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;
      rvrbotron::config::DiffuserConfig monoDiffuser;
      monoDiffuser.steps = 1;
      monoDiffuser.totalMs = 1.0;
      rvrbotron::config::CompositionConfig monoComposition;
      monoComposition.stagesSpecified = true;
      monoComposition.stages.emplace_back(monoSplit);
      monoComposition.stages.emplace_back(monoDiffuser);
      monoComposition.stages.emplace_back(referenceSelectDownmixConfig(1));
      rvrbotron::config::EarlyConfig monoEarly;
      monoEarly.taps = std::vector<rvrbotron::config::EarlyTapConfig>{{0}};
      monoComposition.early = monoEarly;
      rvrbotron::config::ReverbConfig monoRequested;
      monoRequested.formatVersion = 2;
      monoRequested.seed = 11;
      monoRequested.composition = monoComposition;
      const auto monoResolved =
          rvrbotron::config::resolveConfig(monoRequested, 48000, 1);
      const auto& monoEarlyResolved = *monoResolved.composition.early;
      if (monoEarlyResolved.downmix.leftChannel != 0U ||
          monoEarlyResolved.downmix.rightChannel.has_value()) {
        std::cerr << "the default Early Downmix at N=1 did not duplicate "
                     "Channel 0 to mono\n";
        return 1;
      }
    }

    // Diffuser-then-Feedback-Loop routes the tap in parallel too, and
    // Early's own Downmix resolves an aligned Alignment expectation
    // independently of the Main Downmix, which is unaligned when its
    // source includes a Feedback Loop (issue #107).
    rvrbotron::config::FeedbackLoopConfig loop;
    loop.delayMinMs = 2.0;
    loop.delayMaxMs = 3.0;
    loop.rt60Sec = 0.05;
    rvrbotron::config::CompositionConfig diffuserLoopComposition;
    diffuserLoopComposition.stagesSpecified = true;
    diffuserLoopComposition.stages.emplace_back(split);
    diffuserLoopComposition.stages.emplace_back(diffuser);
    diffuserLoopComposition.stages.emplace_back(loop);
    diffuserLoopComposition.stages.emplace_back(
        referenceSelectDownmixConfig(2));
    diffuserLoopComposition.early = earlyDefault;
    rvrbotron::config::ReverbConfig diffuserLoopRequested;
    diffuserLoopRequested.formatVersion = 2;
    diffuserLoopRequested.seed = 11;
    diffuserLoopRequested.composition = diffuserLoopComposition;
    const auto diffuserLoopResolved =
        rvrbotron::config::resolveConfig(diffuserLoopRequested, 48000, 1);
    const auto& diffuserLoopMainDownmix =
        std::get<rvrbotron::dsp::ResolvedDownmix>(
            diffuserLoopResolved.composition.stages.back());
    if (diffuserLoopMainDownmix.alignment !=
            rvrbotron::dsp::DownmixAlignment::unaligned ||
        diffuserLoopResolved.composition.early->downmix.alignment !=
            rvrbotron::dsp::DownmixAlignment::aligned) {
      std::cerr << "Early's Downmix did not resolve an aligned Alignment "
                   "expectation independently of an unaligned Main "
                   "Downmix\n";
      return 1;
    }

    // Early's Downmix resolves from its own domain-separated
    // RandomOrthogonal usage tag (EARLDNMX), distinct from the Main
    // Downmix's own (MAINDNMX, issue #108): the same seed and Channel
    // count resolve different rows for the two branches.
    {
      auto orthogonalMainDownmix = referenceSelectDownmixConfig(2);
      orthogonalMainDownmix.strategy =
          rvrbotron::dsp::DownmixStrategy::orthogonalRows;
      orthogonalMainDownmix.leftChannel.reset();
      orthogonalMainDownmix.rightChannel.reset();
      rvrbotron::config::CompositionConfig orthogonalComposition;
      orthogonalComposition.stagesSpecified = true;
      orthogonalComposition.stages.emplace_back(split);
      orthogonalComposition.stages.emplace_back(diffuser);
      orthogonalComposition.stages.emplace_back(orthogonalMainDownmix);
      rvrbotron::config::EarlyConfig orthogonalEarly;
      orthogonalEarly.taps =
          std::vector<rvrbotron::config::EarlyTapConfig>{{0}};
      rvrbotron::config::DownmixConfig orthogonalEarlyDownmix;
      orthogonalEarlyDownmix.strategy =
          rvrbotron::dsp::DownmixStrategy::orthogonalRows;
      orthogonalEarly.downmix = orthogonalEarlyDownmix;
      orthogonalComposition.early = orthogonalEarly;
      rvrbotron::config::ReverbConfig orthogonalRequested;
      orthogonalRequested.formatVersion = 2;
      orthogonalRequested.seed = 17;
      orthogonalRequested.composition = orthogonalComposition;
      const auto orthogonalResolved = rvrbotron::config::resolveConfig(
          orthogonalRequested, 48000, 1);
      const auto& orthogonalMain = std::get<rvrbotron::dsp::ResolvedDownmix>(
          orthogonalResolved.composition.stages.back());
      const auto& orthogonalEarlyResolved =
          orthogonalResolved.composition.early->downmix;
      if (orthogonalMain.leftRow == orthogonalEarlyResolved.leftRow &&
          orthogonalMain.rightRow == orthogonalEarlyResolved.rightRow) {
        std::cerr << "Early's orthogonal-rows Downmix did not resolve "
                     "independently of the Main Downmix's own rows\n";
        return 1;
      }
    }

    // EarlyReflections::ownedBytes() must not double-count its embedded
    // Downmix's own sizeof (PR review on #111): Downmix is held by
    // value, so its in-place storage is already part of sizeof(*this),
    // and only its own backing-vector allocations
    // (Downmix::ownedStorageBytes()) should be added on top. A `select`
    // Downmix's dense rows stay empty (issue #108's fast path), so its
    // only owned storage here is the accumulator and the one-tap taps_
    // array -- exactly sized to expose an extra, wrongly-added
    // sizeof(Downmix) if the bug regresses.
    {
      const rvrbotron::dsp::EarlyReflections earlyDsp(
          *diffuserOnlyResolved.composition.early);
      const auto expectedOwnedBytes =
          sizeof(rvrbotron::dsp::EarlyReflections) +
          2 * sizeof(rvrbotron::dsp::Sample) +
          diffuserOnlyResolved.composition.early->taps.size() *
              sizeof(rvrbotron::dsp::DiffuserEarlyTap);
      if (earlyDsp.ownedBytes() != expectedOwnedBytes) {
        std::cerr << "EarlyReflections::ownedBytes() double-counted its "
                     "embedded Downmix's own sizeof (expected "
                  << expectedOwnedBytes << ", got " << earlyDsp.ownedBytes()
                  << ")\n";
        return 1;
      }
    }

    // Reverb-level behavior: superposition, non-interference, branch
    // level, enablement, and never entering the Feedback Loop. Renders
    // enough frames (this Diffuser's sample budget is 48 samples at
    // 48 kHz) that a tapped Diffusion Step's delayed output has actually
    // reached the observation window, with sustained input so energy
    // checks are not sensitive to exactly where that delay lands.
    constexpr std::size_t kEarlyTestFrames = 64;
    const auto renderFrames =
        [](const rvrbotron::dsp::ResolvedConfig& config) {
          rvrbotron::dsp::Reverb reverb(config);
          std::array<rvrbotron::dsp::Sample, kEarlyTestFrames> input{};
          input.fill(rvrbotron::dsp::Sample{1});
          std::array<rvrbotron::dsp::Sample, kEarlyTestFrames> left{};
          std::array<rvrbotron::dsp::Sample, kEarlyTestFrames> right{};
          const rvrbotron::dsp::Sample* inputs[]{input.data()};
          rvrbotron::dsp::Sample* outputs[]{left.data(), right.data()};
          reverb.process(inputs, 1, outputs, 2, kEarlyTestFrames);
          return std::make_pair(left, right);
        };

    auto withoutEarlyRequested = diffuserOnlyRequested;
    withoutEarlyRequested.composition = diffuserOnlyComposition;
    const auto withoutEarlyResolved =
        rvrbotron::config::resolveConfig(withoutEarlyRequested, 48000, 1);
    const auto withoutEarly = renderFrames(withoutEarlyResolved);

    // A disabled-but-configured Early branch contributes exact zero, and
    // its presence does not perturb the Main wet path's own output --
    // both proven together by exact equality with the no-branch render.
    auto earlyDisabledComposition = diffuserOnlyWithEarly;
    earlyDisabledComposition.early->enabled = false;
    auto earlyDisabledRequested = diffuserOnlyRequested;
    earlyDisabledRequested.composition = earlyDisabledComposition;
    const auto earlyDisabledResolved =
        rvrbotron::config::resolveConfig(earlyDisabledRequested, 48000, 1);
    const auto earlyDisabled = renderFrames(earlyDisabledResolved);
    if (earlyDisabled != withoutEarly) {
      std::cerr << "a disabled Early branch did not contribute exact zero, "
                   "or perturbed the Main wet path's own output\n";
      return 1;
    }

    // Isolate Early's own stereo contribution by disabling the Main wet
    // path, then check it is not trivially silent.
    auto earlyOnlyComposition = diffuserOnlyWithEarly;
    earlyOnlyComposition.mainEnabled = false;
    auto earlyOnlyRequested = diffuserOnlyRequested;
    earlyOnlyRequested.composition = earlyOnlyComposition;
    const auto earlyOnlyResolved =
        rvrbotron::config::resolveConfig(earlyOnlyRequested, 48000, 1);
    const auto earlyOnly = renderFrames(earlyOnlyResolved);
    double earlyOnlyEnergy = 0.0;
    for (std::size_t frame = 0; frame < kEarlyTestFrames; ++frame) {
      earlyOnlyEnergy += static_cast<double>(earlyOnly.first[frame]) *
              static_cast<double>(earlyOnly.first[frame]) +
          static_cast<double>(earlyOnly.second[frame]) *
              static_cast<double>(earlyOnly.second[frame]);
    }
    if (!(earlyOnlyEnergy > 0.0)) {
      std::cerr << "an enabled Early branch produced silent output\n";
      return 1;
    }

    // Superposition: combined output equals the sample-wise sum of the
    // separately rendered Main-only and Early-only branches.
    const auto combined = renderFrames(diffuserOnlyResolved);
    for (std::size_t frame = 0; frame < kEarlyTestFrames; ++frame) {
      const auto expectedLeft = static_cast<double>(withoutEarly.first[frame]) +
          static_cast<double>(earlyOnly.first[frame]);
      const auto expectedRight =
          static_cast<double>(withoutEarly.second[frame]) +
          static_cast<double>(earlyOnly.second[frame]);
      if (!close(combined.first[frame], expectedLeft) ||
          !close(combined.second[frame], expectedRight)) {
        std::cerr << "combined output was not the sample-wise sum of Main "
                     "and Early's own separately rendered branches\n";
        return 1;
      }
    }

    // levelDb scales Early's own branch output by its resolved linear
    // gain, applied once after Downmix.
    auto earlyLeveledComposition = earlyOnlyComposition;
    earlyLeveledComposition.early->levelDb = -6.0;
    auto earlyLeveledRequested = diffuserOnlyRequested;
    earlyLeveledRequested.composition = earlyLeveledComposition;
    const auto earlyLeveledResolved =
        rvrbotron::config::resolveConfig(earlyLeveledRequested, 48000, 1);
    const auto earlyLeveled = renderFrames(earlyLeveledResolved);
    const auto expectedEarlyGain = std::pow(10.0, -6.0 / 20.0);
    for (std::size_t frame = 0; frame < kEarlyTestFrames; ++frame) {
      const auto expectedLeft =
          static_cast<double>(earlyOnly.first[frame]) * expectedEarlyGain;
      const auto expectedRight =
          static_cast<double>(earlyOnly.second[frame]) * expectedEarlyGain;
      if (!close(earlyLeveled.first[frame], expectedLeft) ||
          !close(earlyLeveled.second[frame], expectedRight)) {
        std::cerr << "levelDb: -6 did not scale Early's own branch output "
                     "by its resolved gain\n";
        return 1;
      }
    }

    // Early Reflections never enter the Feedback Loop: tapping the same
    // Diffuser (same seed, same Channels, same steps) produces a
    // bit-identical Early-only contribution whether or not a Feedback
    // Loop follows it in the Main wet path.
    auto diffuserLoopEarlyOnlyComposition = diffuserLoopComposition;
    diffuserLoopEarlyOnlyComposition.mainEnabled = false;
    rvrbotron::config::ReverbConfig diffuserLoopEarlyOnlyRequested;
    diffuserLoopEarlyOnlyRequested.formatVersion = 2;
    diffuserLoopEarlyOnlyRequested.seed = 11;
    diffuserLoopEarlyOnlyRequested.composition =
        diffuserLoopEarlyOnlyComposition;
    const auto diffuserLoopEarlyOnlyResolved = rvrbotron::config::resolveConfig(
        diffuserLoopEarlyOnlyRequested, 48000, 1);
    const auto diffuserLoopEarlyOnly =
        renderFrames(diffuserLoopEarlyOnlyResolved);
    if (diffuserLoopEarlyOnly != earlyOnly) {
      std::cerr << "Early Reflections' own contribution changed when a "
                   "Feedback Loop followed the same Diffuser -- Early "
                   "Reflections must never enter the Feedback Loop\n";
      return 1;
    }

    // Early Reflections do not extend the resolved Tail budget (issue
    // #112, docs/design/reverb/stages/09-composition.md's "Early
    // Reflections do not extend the existing drain"): every tap is
    // already bounded by the Diffuser's own finite response.
    rvrbotron::dsp::Reverb withoutEarlyReverb(withoutEarlyResolved);
    rvrbotron::dsp::Reverb combinedReverb(diffuserOnlyResolved);
    if (combinedReverb.tailBudgetFrames() !=
        withoutEarlyReverb.tailBudgetFrames()) {
      std::cerr << "configuring an Early Reflections branch changed the "
                   "resolved Tail budget\n";
      return 1;
    }

    // Optional early-stereo/main-stereo captures occur immediately
    // before summation (issue #113): each branch's own captured stereo
    // pair sums to the final output sample-for-sample, combined energy
    // reconciles with branch energies plus their cross term, a disabled
    // branch's own capture is correctly sized exact zero (and both
    // branches disabled together produce exact silence throughout), and
    // no early-stereo capture occurs at all when no Early branch is
    // configured.
    const auto renderWithCapture =
        [](const rvrbotron::dsp::ResolvedConfig& config,
           rvrbotron::dsp::StageCaptureSink* const sink) {
          rvrbotron::dsp::Reverb reverb(config, sink);
          std::array<rvrbotron::dsp::Sample, kEarlyTestFrames> input{};
          input.fill(rvrbotron::dsp::Sample{1});
          std::array<rvrbotron::dsp::Sample, kEarlyTestFrames> left{};
          std::array<rvrbotron::dsp::Sample, kEarlyTestFrames> right{};
          const rvrbotron::dsp::Sample* inputs[]{input.data()};
          rvrbotron::dsp::Sample* outputs[]{left.data(), right.data()};
          reverb.process(inputs, 1, outputs, 2, kEarlyTestFrames);
          return std::make_pair(left, right);
        };

    BranchCapture combinedCapture;
    const auto combinedWithCapture =
        renderWithCapture(diffuserOnlyResolved, &combinedCapture);
    if (!combinedCapture.valid ||
        combinedCapture.mainLeft.size() != kEarlyTestFrames ||
        combinedCapture.earlyLeft.size() != kEarlyTestFrames) {
      std::cerr << "did not capture one Main-stereo and one Early-stereo "
                   "frame per rendered frame\n";
      return 1;
    }
    double mainEnergy = 0.0;
    double earlyEnergy = 0.0;
    double crossTerm = 0.0;
    double combinedEnergy = 0.0;
    for (std::size_t frame = 0; frame < kEarlyTestFrames; ++frame) {
      const auto mL = static_cast<double>(combinedCapture.mainLeft[frame]);
      const auto mR = static_cast<double>(combinedCapture.mainRight[frame]);
      const auto eL = static_cast<double>(combinedCapture.earlyLeft[frame]);
      const auto eR = static_cast<double>(combinedCapture.earlyRight[frame]);
      if (!close(combinedWithCapture.first[frame], mL + eL) ||
          !close(combinedWithCapture.second[frame], mR + eR)) {
        std::cerr << "combined output did not equal the sample-wise sum "
                     "of its captured Main-stereo and Early-stereo "
                     "branches at frame "
                  << frame << "\n";
        return 1;
      }
      mainEnergy += mL * mL + mR * mR;
      earlyEnergy += eL * eL + eR * eR;
      crossTerm += mL * eL + mR * eR;
      const auto cL = static_cast<double>(combinedWithCapture.first[frame]);
      const auto cR = static_cast<double>(combinedWithCapture.second[frame]);
      combinedEnergy += cL * cL + cR * cR;
    }
    const auto expectedCombinedEnergy =
        mainEnergy + earlyEnergy + 2.0 * crossTerm;
    if (std::abs(combinedEnergy - expectedCombinedEnergy) >
        1e-6 * std::max(1.0, combinedEnergy)) {
      std::cerr << "combined energy did not reconcile with branch "
                   "energies plus their cross term\n";
      return 1;
    }

    auto bothDisabledComposition = diffuserOnlyWithEarly;
    bothDisabledComposition.mainEnabled = false;
    bothDisabledComposition.early->enabled = false;
    rvrbotron::config::ReverbConfig bothDisabledRequested;
    bothDisabledRequested.formatVersion = 2;
    bothDisabledRequested.seed = 11;
    bothDisabledRequested.composition = bothDisabledComposition;
    const auto bothDisabledResolved =
        rvrbotron::config::resolveConfig(bothDisabledRequested, 48000, 1);
    BranchCapture bothDisabledCapture;
    const auto bothDisabledWithCapture =
        renderWithCapture(bothDisabledResolved, &bothDisabledCapture);
    if (!bothDisabledCapture.valid ||
        bothDisabledCapture.mainLeft.size() != kEarlyTestFrames ||
        bothDisabledCapture.mainRight.size() != kEarlyTestFrames ||
        bothDisabledCapture.earlyLeft.size() != kEarlyTestFrames ||
        bothDisabledCapture.earlyRight.size() != kEarlyTestFrames) {
      std::cerr << "disabled branch captures did not share the render timeline\n";
      return 1;
    }
    for (std::size_t frame = 0; frame < kEarlyTestFrames; ++frame) {
      if (bothDisabledWithCapture.first[frame] != rvrbotron::dsp::Sample{0} ||
          bothDisabledWithCapture.second[frame] !=
              rvrbotron::dsp::Sample{0} ||
          bothDisabledCapture.mainLeft[frame] != rvrbotron::dsp::Sample{0} ||
          bothDisabledCapture.mainRight[frame] != rvrbotron::dsp::Sample{0} ||
          bothDisabledCapture.earlyLeft[frame] != rvrbotron::dsp::Sample{0} ||
          bothDisabledCapture.earlyRight[frame] !=
              rvrbotron::dsp::Sample{0}) {
        std::cerr << "both branches disabled did not produce exact "
                     "silence in the output and both captures at frame "
                  << frame << "\n";
        return 1;
      }
    }

    BranchCapture noEarlyCapture;
    renderWithCapture(withoutEarlyResolved, &noEarlyCapture);
    if (!noEarlyCapture.earlyLeft.empty() ||
        !noEarlyCapture.earlyRight.empty()) {
      std::cerr << "an early-stereo capture occurred with no Early "
                   "Reflections branch configured\n";
      return 1;
    }
    if (noEarlyCapture.mainLeft.size() != kEarlyTestFrames) {
      std::cerr << "Main-stereo was not captured once per rendered "
                   "frame\n";
      return 1;
    }
  }

  // Multiple distinct taps (issue #112): unique, sorted canonically
  // during resolution regardless of Requested order, each independently
  // shaped by its own gainDb and the shared decayDbPerSec envelope
  // slope, and a Requested tap-list permutation resolves and renders
  // identically.
  {
    rvrbotron::config::SplitConfig multiTapSplit;
    multiTapSplit.channels = 2;
    multiTapSplit.strategy = rvrbotron::dsp::SplitStrategyType::duplicate;
    multiTapSplit.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;
    rvrbotron::config::DiffuserConfig multiStepDiffuser;
    multiStepDiffuser.steps = 4;
    multiStepDiffuser.totalMs = 4.0;
    multiStepDiffuser.distribution =
        rvrbotron::config::DiffusionDistribution::even;

    rvrbotron::config::CompositionConfig multiStepComposition;
    multiStepComposition.stagesSpecified = true;
    multiStepComposition.stages.emplace_back(multiTapSplit);
    multiStepComposition.stages.emplace_back(multiStepDiffuser);
    multiStepComposition.stages.emplace_back(referenceSelectDownmixConfig(2));

    rvrbotron::config::EarlyTapConfig tap0;
    tap0.stepIndex = 0;
    tap0.gainDb = -3.0;
    rvrbotron::config::EarlyTapConfig tap2;
    tap2.stepIndex = 2;
    tap2.gainDb = 4.0;
    rvrbotron::config::EarlyTapConfig tap3;
    tap3.stepIndex = 3;

    rvrbotron::config::EarlyConfig forwardOrderEarly;
    forwardOrderEarly.taps =
        std::vector<rvrbotron::config::EarlyTapConfig>{tap0, tap2, tap3};
    forwardOrderEarly.decayDbPerSec = 50.0;

    auto forwardComposition = multiStepComposition;
    forwardComposition.early = forwardOrderEarly;
    rvrbotron::config::ReverbConfig forwardRequested;
    forwardRequested.formatVersion = 2;
    forwardRequested.seed = 23;
    forwardRequested.composition = forwardComposition;
    const auto forwardResolved =
        rvrbotron::config::resolveConfig(forwardRequested, 48000, 1);

    const auto& resolvedTaps = forwardResolved.composition.early->taps;
    if (resolvedTaps.size() != 3 || resolvedTaps[0].stepIndex != 0 ||
        resolvedTaps[1].stepIndex != 2 || resolvedTaps[2].stepIndex != 3) {
      std::cerr << "multiple Early taps did not resolve in canonical "
                   "ascending order\n";
      return 1;
    }

    // A later tap has a longer nominal support end than an earlier one,
    // so at a positive decayDbPerSec it is shaped more negatively.
    if (resolvedTaps[1].nominalSupportMaxMs <=
            resolvedTaps[0].nominalSupportMaxMs ||
        resolvedTaps[2].nominalSupportMaxMs <=
            resolvedTaps[1].nominalSupportMaxMs) {
      std::cerr << "later Early taps did not resolve a longer nominal "
                   "support end than earlier ones\n";
      return 1;
    }
    for (const auto& tap : resolvedTaps) {
      const auto expectedShapingGainDb = tap.gainDb -
          *forwardOrderEarly.decayDbPerSec * (tap.nominalSupportMaxMs / 1000.0);
      if (std::abs(tap.shapingGainDb - expectedShapingGainDb) > 1e-9) {
        std::cerr << "a tap's shapingGainDb did not match gainDb minus "
                     "decayDbPerSec times its own nominal support end\n";
        return 1;
      }
      const auto expectedGain = std::pow(10.0, expectedShapingGainDb / 20.0);
      if (std::abs(tap.gain - expectedGain) > 1e-9) {
        std::cerr << "a tap's resolved linear gain did not match its own "
                     "shapingGainDb\n";
        return 1;
      }
    }

    // A Requested tap-list permutation resolves and renders identically.
    rvrbotron::config::EarlyConfig reverseOrderEarly = forwardOrderEarly;
    reverseOrderEarly.taps =
        std::vector<rvrbotron::config::EarlyTapConfig>{tap3, tap2, tap0};
    auto reverseComposition = multiStepComposition;
    reverseComposition.early = reverseOrderEarly;
    rvrbotron::config::ReverbConfig reverseRequested;
    reverseRequested.formatVersion = 2;
    reverseRequested.seed = 23;
    reverseRequested.composition = reverseComposition;
    const auto reverseResolved =
        rvrbotron::config::resolveConfig(reverseRequested, 48000, 1);

    const auto& reversedTaps = reverseResolved.composition.early->taps;
    if (reversedTaps.size() != resolvedTaps.size()) {
      std::cerr << "a permuted tap-list request resolved a different tap "
                   "count\n";
      return 1;
    }
    for (std::size_t index = 0; index < resolvedTaps.size(); ++index) {
      const auto& forwardTap = resolvedTaps[index];
      const auto& reverseTap = reversedTaps[index];
      if (forwardTap.stepIndex != reverseTap.stepIndex ||
          forwardTap.gainDb != reverseTap.gainDb ||
          forwardTap.nominalSupportMinSamples !=
              reverseTap.nominalSupportMinSamples ||
          forwardTap.nominalSupportMaxSamples !=
              reverseTap.nominalSupportMaxSamples ||
          forwardTap.conservativeSupportMinSamples !=
              reverseTap.conservativeSupportMinSamples ||
          forwardTap.conservativeSupportMaxSamples !=
              reverseTap.conservativeSupportMaxSamples ||
          forwardTap.shapingGainDb != reverseTap.shapingGainDb ||
          forwardTap.gain != reverseTap.gain) {
        std::cerr << "a Requested tap-list permutation did not resolve "
                     "identically\n";
        return 1;
      }
    }

    rvrbotron::dsp::Reverb forwardReverb(forwardResolved);
    rvrbotron::dsp::Reverb reverseReverb(reverseResolved);
    constexpr std::size_t permutationFrames = 64;
    std::array<rvrbotron::dsp::Sample, permutationFrames> permutationInput{};
    permutationInput.fill(rvrbotron::dsp::Sample{1});
    std::array<rvrbotron::dsp::Sample, permutationFrames> forwardLeft{};
    std::array<rvrbotron::dsp::Sample, permutationFrames> forwardRight{};
    std::array<rvrbotron::dsp::Sample, permutationFrames> reverseLeft{};
    std::array<rvrbotron::dsp::Sample, permutationFrames> reverseRight{};
    const rvrbotron::dsp::Sample* permutationInputs[]{
        permutationInput.data()};
    rvrbotron::dsp::Sample* forwardOutputs[]{
        forwardLeft.data(), forwardRight.data()};
    rvrbotron::dsp::Sample* reverseOutputs[]{
        reverseLeft.data(), reverseRight.data()};
    forwardReverb.process(
        permutationInputs, 1, forwardOutputs, 2, permutationFrames);
    reverseReverb.process(
        permutationInputs, 1, reverseOutputs, 2, permutationFrames);
    if (forwardLeft != reverseLeft || forwardRight != reverseRight) {
      std::cerr << "a Requested tap-list permutation did not render "
                   "identically\n";
      return 1;
    }
  }

  // No tap energy occurs outside its exact nominal (and, absent
  // Modulation, identically conservative) support (issue #112): a
  // hand-built two-step Diffuser with known per-Channel delays gives an
  // exact expected interval, computed independently by simple arithmetic
  // on the same delaysSamples this test constructs -- not by calling
  // production resolution code.
  {
    constexpr double scale = 0.70710678118654752440;
    rvrbotron::dsp::ResolvedDiffusionStep supportStep0;
    supportStep0.index = 0;
    supportStep0.delaysSamples = {2, 5};
    supportStep0.bufferSizes = {2, 5};
    supportStep0.permutation = {0, 1};
    supportStep0.polaritySigns = {1, 1};
    supportStep0.matrix = {scale, scale, scale, -scale};
    rvrbotron::dsp::ResolvedDiffusionStep supportStep1;
    supportStep1.index = 1;
    supportStep1.delaysSamples = {1, 3};
    supportStep1.bufferSizes = {1, 3};
    supportStep1.permutation = {0, 1};
    supportStep1.polaritySigns = {1, 1};
    supportStep1.matrix = {scale, scale, scale, -scale};

    rvrbotron::dsp::ResolvedDiffuser supportDiffuser;
    supportDiffuser.totalSamples = 8;
    supportDiffuser.steps = {supportStep0, supportStep1};

    rvrbotron::dsp::ResolvedConfig supportConfig;
    supportConfig.formatVersion = 2;
    supportConfig.sampleRate = 1000;
    supportConfig.composition.stages.emplace_back(
        rvrbotron::dsp::ResolvedSplit{
            1,
            2,
            rvrbotron::dsp::SplitStrategyType::duplicate,
            rvrbotron::dsp::EnergyNormalisation::energy,
            1.0,
            scale,
        });
    supportConfig.composition.stages.emplace_back(supportDiffuser);
    const rvrbotron::dsp::ResolvedDownmix supportDownmix{
        2,
        2,
        rvrbotron::dsp::DownmixStrategy::select,
        0,
        1,
        rvrbotron::dsp::EnergyNormalisation::energy,
        1.0,
        {1.0, 0.0},
        {0.0, 1.0},
        {1.0, 0.0},
        {0.0, 1.0},
        rvrbotron::dsp::DownmixAlignment::aligned,
        90.0,
        {1.0, 0.0, 0.0, 1.0},
    };
    supportConfig.composition.stages.emplace_back(supportDownmix);
    supportConfig.composition.mainEnabled = false;

    rvrbotron::dsp::ResolvedEarlyReflections supportEarly;
    supportEarly.enabled = true;
    supportEarly.gain = 1.0;
    rvrbotron::dsp::ResolvedEarlyTap supportTap;
    supportTap.stepIndex = 1;
    supportTap.nominalSupportMinSamples = 3;
    supportTap.nominalSupportMaxSamples = 8;
    supportTap.conservativeSupportMinSamples = 3;
    supportTap.conservativeSupportMaxSamples = 8;
    supportTap.gain = 1.0;
    supportEarly.taps.push_back(supportTap);
    supportEarly.downmix = supportDownmix;
    supportConfig.composition.early = supportEarly;

    rvrbotron::dsp::Reverb supportReverb(supportConfig);
    constexpr std::size_t supportFrames = 16;
    std::array<rvrbotron::dsp::Sample, supportFrames> supportInput{};
    supportInput[0] = rvrbotron::dsp::Sample{1};
    std::array<rvrbotron::dsp::Sample, supportFrames> supportLeft{};
    std::array<rvrbotron::dsp::Sample, supportFrames> supportRight{};
    const rvrbotron::dsp::Sample* supportInputs[]{supportInput.data()};
    rvrbotron::dsp::Sample* supportOutputs[]{
        supportLeft.data(), supportRight.data()};
    supportReverb.process(
        supportInputs, 1, supportOutputs, 2, supportFrames);

    for (std::size_t frame = 0; frame < supportFrames; ++frame) {
      const auto withinBounds =
          frame >= supportTap.nominalSupportMinSamples &&
          frame <= supportTap.nominalSupportMaxSamples;
      if (!withinBounds &&
          (supportLeft[frame] != rvrbotron::dsp::Sample{0} ||
           supportRight[frame] != rvrbotron::dsp::Sample{0})) {
        std::cerr << "tap energy occurred at frame " << frame
                   << ", outside its exact nominal/conservative support ["
                   << supportTap.nominalSupportMinSamples << ", "
                   << supportTap.nominalSupportMaxSamples << "]\n";
        return 1;
      }
    }
  }

  // Conservative Tap support widens beyond nominal support when the
  // tapped step's own Modulation is active (issue #112), while nominal
  // support itself never changes. Probes this seed's own resolved delays
  // with Modulation omitted first (mirroring the Diffusion Step
  // Modulation tests' own probe-then-build pattern below), so the depth
  // chosen here stays safely within the Excursion rejection rule
  // regardless of which values segmented-random draws.
  {
    constexpr std::uint32_t channels = 8;
    constexpr double totalMs = 200.0;
    constexpr std::uint32_t stepCount = 4;
    constexpr std::uint32_t tappedStep = stepCount - 1;

    // `modulatedStepIndex` names which step carries the Modulation
    // override -- the tapped step itself for the widening test below, or
    // an earlier contributing step for the summation test further down
    // (PR review on #112: a test fixture that only ever modulates the
    // tapped step cannot catch a regression that widens conservative
    // support from that step alone rather than summing every
    // contributing step's own reach).
    const auto buildModulatedComposition =
        [&](const std::uint32_t modulatedStepIndex,
            const std::optional<rvrbotron::config::ModulationConfig>&
                stepModulation) {
          rvrbotron::config::SplitConfig probeSplit;
          probeSplit.channels = channels;
          probeSplit.strategy = rvrbotron::dsp::SplitStrategyType::duplicate;
          probeSplit.normalisation =
              rvrbotron::dsp::EnergyNormalisation::energy;

          rvrbotron::config::DiffusionStepConfig probeStep;
          probeStep.delayStrategy =
              rvrbotron::dsp::DelayStrategy::segmentedRandom;
          probeStep.mix = rvrbotron::dsp::MixMatrixType::hadamard;
          probeStep.shuffle = true;
          probeStep.polarity = rvrbotron::dsp::PolarityStrategy::seededRandom;

          rvrbotron::config::DiffuserConfig probeDiffuser;
          probeDiffuser.steps = stepCount;
          probeDiffuser.totalMs = totalMs;
          probeDiffuser.distribution =
              rvrbotron::config::DiffusionDistribution::even;
          probeDiffuser.step = probeStep;
          if (stepModulation.has_value()) {
            rvrbotron::config::DiffusionStepOverride override;
            override.index = modulatedStepIndex;
            override.step.modulation = stepModulation;
            probeDiffuser.stepOverrides =
                std::vector<rvrbotron::config::DiffusionStepOverride>{
                    override};
          }

          rvrbotron::config::CompositionConfig composition;
          composition.stagesSpecified = true;
          composition.stages.emplace_back(probeSplit);
          composition.stages.emplace_back(probeDiffuser);
          composition.stages.emplace_back(
              referenceSelectDownmixConfig(channels));
          rvrbotron::config::EarlyConfig early;
          early.taps =
              std::vector<rvrbotron::config::EarlyTapConfig>{{tappedStep}};
          composition.early = early;

          rvrbotron::config::ReverbConfig requested;
          requested.formatVersion = 2;
          requested.seed = 0x9e3779b97f4a7c15ULL;
          requested.composition = std::move(composition);
          return rvrbotron::config::resolveConfig(requested, 48000, 1);
        };

    const auto probeResolved =
        buildModulatedComposition(tappedStep, std::nullopt);
    const auto& probeDiffuserStage = std::get<rvrbotron::dsp::ResolvedDiffuser>(
        probeResolved.composition.stages[1]);
    auto minDelay = std::numeric_limits<std::uint64_t>::max();
    for (const auto& step : probeDiffuserStage.steps) {
      for (const auto delay : step.delaysSamples) {
        minDelay = std::min(minDelay, delay);
      }
    }
    if (minDelay == std::numeric_limits<std::uint64_t>::max() ||
        minDelay < 10) {
      std::cerr << "Tap support Modulation-widening probe resolved an "
                   "unexpectedly short delay -- adjust the test fixture\n";
      return 1;
    }
    const auto safeExcursionSamples =
        static_cast<double>(
            minDelay -
            rvrbotron::config::kModulationInterpolationMarginSamples) /
        3.0;
    const auto safeDepthMs = safeExcursionSamples * 1000.0 / 48000.0;

    rvrbotron::config::ModulationConfig tappedModulation;
    tappedModulation.depthMs = safeDepthMs;
    const auto modulatedResolved =
        buildModulatedComposition(tappedStep, tappedModulation);
    const auto& modulatedTap =
        modulatedResolved.composition.early->taps.front();
    const auto& probeTap = probeResolved.composition.early->taps.front();

    // The Diffuser's own resolved totalSamples grows by the tapped step's
    // own Modulation reach (PR review on #112): a general fix to the
    // Diffuser's drain, independent of Early Reflections, since a
    // modulated Channel's read can still reference live content past a
    // step's own nominal length.
    const auto& modulatedDiffuserStage =
        std::get<rvrbotron::dsp::ResolvedDiffuser>(
            modulatedResolved.composition.stages[1]);
    const auto expectedReachSamples =
        rvrbotron::config::resolveModulationHeadroomSamples(
            modulatedDiffuserStage.steps[tappedStep]
                .modulation->excursionSamples);
    if (modulatedDiffuserStage.totalSamples !=
        probeDiffuserStage.totalSamples + expectedReachSamples) {
      std::cerr << "the Diffuser's own resolved totalSamples did not grow "
                   "by the tapped step's own Modulation reach ("
                << modulatedDiffuserStage.totalSamples << " != "
                << probeDiffuserStage.totalSamples << " + "
                << expectedReachSamples << ")\n";
      return 1;
    }

    if (modulatedTap.nominalSupportMinSamples !=
            probeTap.nominalSupportMinSamples ||
        modulatedTap.nominalSupportMaxSamples !=
            probeTap.nominalSupportMaxSamples) {
      std::cerr << "activating Modulation on the tapped step changed its "
                   "nominal Tap support, which Modulation must never "
                   "affect\n";
      return 1;
    }
    if (modulatedTap.conservativeSupportMinSamples >
            modulatedTap.nominalSupportMinSamples ||
        modulatedTap.conservativeSupportMaxSamples <
            modulatedTap.nominalSupportMaxSamples) {
      std::cerr << "conservative Tap support did not widen to at least "
                   "cover nominal support\n";
      return 1;
    }
    if (modulatedTap.conservativeSupportMaxSamples <=
        probeTap.conservativeSupportMaxSamples) {
      std::cerr << "activating Modulation on the tapped step did not "
                   "widen its conservative Tap support\n";
      return 1;
    }

    // Render the modulated tap in isolation (Main disabled) and confirm
    // no energy falls outside its own resolved conservative support: the
    // unmodulated hand-built test above cannot exercise the actual
    // Excursion/interpolation widening formula, only the unmodulated
    // (zero-reach) case.
    auto earlyOnlyModulatedResolved = modulatedResolved;
    earlyOnlyModulatedResolved.composition.mainEnabled = false;
    rvrbotron::dsp::Reverb modulatedReverb(earlyOnlyModulatedResolved);

    // The Diffuser's own resolved drain already covers this tap's own
    // conservative support end (PR review on #112): before an actively
    // modulated step's own reach was added to the Diffuser's resolved
    // totalSamples, its reach could push a tap's conservative bound past
    // the drain the renderer actually produces, silently truncating real
    // energy from both the render and any Stage capture sharing the same
    // output timeline (docs/design/reverb/stages/09-composition.md's
    // "Early Reflections do not extend the existing drain" -- true only
    // once the drain itself already accounts for Modulation).
    if (modulatedReverb.tailBudgetFrames() <
        modulatedTap.conservativeSupportMaxSamples) {
      std::cerr << "the Diffuser's own resolved drain did not cover the "
                   "modulated tap's own conservative support end ("
                << modulatedReverb.tailBudgetFrames() << " < "
                << modulatedTap.conservativeSupportMaxSamples << ")\n";
      return 1;
    }

    const auto modulatedFrameCount = static_cast<std::size_t>(
        modulatedTap.conservativeSupportMaxSamples + 16);
    std::vector<rvrbotron::dsp::Sample> modulatedInput(
        modulatedFrameCount, rvrbotron::dsp::Sample{0});
    modulatedInput[0] = rvrbotron::dsp::Sample{1};
    std::vector<rvrbotron::dsp::Sample> modulatedLeft(modulatedFrameCount);
    std::vector<rvrbotron::dsp::Sample> modulatedRight(modulatedFrameCount);
    const rvrbotron::dsp::Sample* modulatedInputs[]{modulatedInput.data()};
    rvrbotron::dsp::Sample* modulatedOutputs[]{
        modulatedLeft.data(), modulatedRight.data()};
    modulatedReverb.process(
        modulatedInputs, 1, modulatedOutputs, 2, modulatedFrameCount);

    for (std::size_t frame = 0; frame < modulatedFrameCount; ++frame) {
      const auto withinBounds =
          frame >= modulatedTap.conservativeSupportMinSamples &&
          frame <= modulatedTap.conservativeSupportMaxSamples;
      if (!withinBounds &&
          (modulatedLeft[frame] != rvrbotron::dsp::Sample{0} ||
           modulatedRight[frame] != rvrbotron::dsp::Sample{0})) {
        std::cerr << "modulated tap energy occurred at frame " << frame
                   << ", outside its resolved conservative support ["
                   << modulatedTap.conservativeSupportMinSamples << ", "
                   << modulatedTap.conservativeSupportMaxSamples << "]\n";
        return 1;
      }
    }

    // Modulating an *earlier* contributing step (not the tapped step
    // itself) still widens the later tap's own conservative support
    // (PR review on #112): the widening test above alone cannot
    // distinguish correctly summing every step's own reach through the
    // tap from a regression that only ever looked at the tapped step.
    // `safeDepthMs` was probed against the shortest delay across every
    // step, so it stays safe here too.
    constexpr std::uint32_t earlierStep = 0;
    rvrbotron::config::ModulationConfig earlierStepModulation;
    earlierStepModulation.depthMs = safeDepthMs;
    const auto earlierModulatedResolved =
        buildModulatedComposition(earlierStep, earlierStepModulation);
    const auto& earlierModulatedTap =
        earlierModulatedResolved.composition.early->taps.front();
    if (earlierModulatedTap.nominalSupportMinSamples !=
            probeTap.nominalSupportMinSamples ||
        earlierModulatedTap.nominalSupportMaxSamples !=
            probeTap.nominalSupportMaxSamples) {
      std::cerr << "activating Modulation on an earlier contributing step "
                   "changed the later tap's own nominal Tap support, "
                   "which Modulation must never affect\n";
      return 1;
    }
    if (earlierModulatedTap.conservativeSupportMaxSamples <=
        probeTap.conservativeSupportMaxSamples) {
      std::cerr << "activating Modulation on an earlier contributing step "
                   "did not widen the later tap's own conservative Tap "
                   "support -- support resolution must sum every "
                   "contributing step's own reach, not only the tapped "
                   "step's\n";
      return 1;
    }
  }

  for (const auto channels : {1U, 2U, 4U, 8U, 16U}) {
    if (!reverbDiffusionStepIsAllPass(channels)) {
      std::cerr << "Diffusion Step changed pseudo-random input energy at "
                << channels << " Channels\n";
      return 1;
    }
  }

  // Householder and RandomOrthogonal are valid for any Channel count,
  // including non-powers-of-two, unlike Hadamard.
  for (const auto mix :
       {rvrbotron::dsp::MixMatrixType::householder,
        rvrbotron::dsp::MixMatrixType::randomOrthogonal}) {
    for (const auto channels : {1U, 3U, 5U, 9U, 20U}) {
      if (!reverbDiffusionStepIsAllPass(channels, mix)) {
        std::cerr << "Diffusion Step changed pseudo-random input energy at "
                  << channels << " Channels with a non-Hadamard matrix\n";
        return 1;
      }
    }
  }

  // Reverb::ownedBytes() accounts owned-container capacities, not size:
  // bumping one Channel's resolved delay (and matching buffer size) by
  // 1000 samples changes only that Channel's delay-storage allocation.
  // Comparing the exact byte delta (rather than an absolute figure, which
  // depends on platform/compiler-specific struct layout) keeps this
  // portable across every supported platform.
  auto baselineConfig = twoChannelDiffusionConfig();
  auto& baselineStep =
      std::get<rvrbotron::dsp::ResolvedDiffuser>(
          baselineConfig.composition.stages[1])
          .steps.front();
  auto enlargedConfig = baselineConfig;
  auto& enlargedStep =
      std::get<rvrbotron::dsp::ResolvedDiffuser>(
          enlargedConfig.composition.stages[1])
          .steps.front();
  constexpr std::uint64_t extraDelaySamples = 1000;
  enlargedStep.delaysSamples[1] += extraDelaySamples;
  enlargedStep.bufferSizes[1] += extraDelaySamples;

  rvrbotron::dsp::Reverb baselineReverb(baselineConfig);
  rvrbotron::dsp::Reverb enlargedReverb(enlargedConfig);
  beginAllocationCount();
  const auto baselineBytes = baselineReverb.ownedBytes();
  const auto enlargedBytes = enlargedReverb.ownedBytes();
  const auto ownedBytesAllocations = endAllocationCount();
  if (ownedBytesAllocations != 0) {
    std::cerr << "Reverb::ownedBytes allocated while measuring\n";
    return 1;
  }
  if (baselineBytes == 0) {
    std::cerr << "Reverb::ownedBytes reported zero for a real Diffuser\n";
    return 1;
  }
  const auto expectedDelta =
      extraDelaySamples * sizeof(rvrbotron::dsp::Sample);
  if (enlargedBytes - baselineBytes != expectedDelta) {
    std::cerr << "Reverb::ownedBytes did not track delay-storage capacity: "
              << (enlargedBytes - baselineBytes) << " != " << expectedDelta
              << '\n';
    return 1;
  }
  // Enlarging the resolved buffer must not disturb the still-allocation-free
  // real-time contract of the (separately constructed) baseline Reverb.
  std::array<rvrbotron::dsp::Sample, 2> ownedBytesImpulse{1.0F, 0.0F};
  std::array<rvrbotron::dsp::Sample, 2> ownedBytesWetLeft{};
  std::array<rvrbotron::dsp::Sample, 2> ownedBytesWetRight{};
  const rvrbotron::dsp::Sample* ownedBytesInputs[]{
      ownedBytesImpulse.data()};
  rvrbotron::dsp::Sample* ownedBytesOutputs[]{
      ownedBytesWetLeft.data(), ownedBytesWetRight.data()};
  beginAllocationCount();
  baselineReverb.process(
      ownedBytesInputs, 1, ownedBytesOutputs, 2, 2);
  if (endAllocationCount() != 0) {
    std::cerr
        << "Reverb::process allocated after ownedBytes was queried\n";
    return 1;
  }

  // A hand-derivable N=2 Feedback Loop fixture (distinct integer delays 2
  // and 3, N=2 Householder mixing -- a pure swap-and-negate, computed by
  // hand from resolveHouseholderMatrix's closed form) verified against an
  // independently worked oracle rather than trusting this DSP class's own
  // output. Unlike the Diffuser's aligned arrivals (twoChannelDiffusionConfig
  // above), each Channel here carries its own distinct echo time -- the
  // Feedback Loop's output is unaligned.
  rvrbotron::dsp::ResolvedFeedbackLoop unalignedLoopConfig;
  unalignedLoopConfig.channels = 2;
  unalignedLoopConfig.delaysSamples = {2, 3};
  unalignedLoopConfig.bufferSizes = {2, 3};
  unalignedLoopConfig.gains = {0.5, 0.5};
  unalignedLoopConfig.mix = rvrbotron::dsp::MixMatrixType::householder;
  unalignedLoopConfig.matrix = {0.0, -1.0, -1.0, 0.0};
  rvrbotron::dsp::FeedbackLoop unalignedLoop(unalignedLoopConfig);

  std::array<std::array<rvrbotron::dsp::Sample, 2>, 8> loopFrames{};
  const std::array<rvrbotron::dsp::Sample, 2> firstLoopInput{1.0F, 0.0F};
  const std::array<rvrbotron::dsp::Sample, 2> silentLoopInput{0.0F, 0.0F};
  beginAllocationCount();
  for (std::size_t frame = 0; frame < loopFrames.size(); ++frame) {
    const auto& in = frame == 0 ? firstLoopInput : silentLoopInput;
    unalignedLoop.processFrame(in.data(), loopFrames[frame].data());
  }
  const auto loopAllocations = endAllocationCount();
  if (loopAllocations != 0) {
    std::cerr << "Feedback Loop allocated while processing\n";
    return 1;
  }

  const auto firstNonzero = [&](const std::size_t channel) -> std::size_t {
    for (std::size_t frame = 0; frame < loopFrames.size(); ++frame) {
      if (loopFrames[frame][channel] != rvrbotron::dsp::Sample{0}) {
        return frame;
      }
    }
    return loopFrames.size();
  };
  const auto firstArrival0 = firstNonzero(0);
  const auto firstArrival1 = firstNonzero(1);
  if (firstArrival0 != 2 || firstArrival1 != 5) {
    std::cerr << "Feedback Loop did not produce the expected per-Channel "
                 "arrival times: "
              << firstArrival0 << ", " << firstArrival1 << '\n';
    return 1;
  }
  if (firstArrival0 == firstArrival1) {
    std::cerr
        << "Feedback Loop output was unexpectedly aligned across Channels\n";
    return 1;
  }
  if (!close(loopFrames[2][0], 1.0) || !close(loopFrames[5][1], -0.5)) {
    std::cerr << "Feedback Loop delayed/gained arrival values are incorrect\n";
    return 1;
  }

  // Head-to-head aligned-vs-unaligned comparison, independently computed
  // rather than inferred from a comment: active-sample-index sets
  // (CONTEXT.md's "Aligned" -- Channels carrying the same echo times) for
  // the existing two-Channel Diffuser fixture above versus this Feedback
  // Loop fixture. Feeding twoChannelDiffusionConfig's Diffuser the same
  // impulse on both Channels, its delays {0, 1} and Hadamard mixing put
  // both Channels active at exactly frames {0, 1} -- identical sets,
  // differing only in sign, which is exactly the aligned definition.
  {
    const auto diffusionConfig = twoChannelDiffusionConfig();
    const auto& diffuserStage = std::get<rvrbotron::dsp::ResolvedDiffuser>(
        diffusionConfig.composition.stages[1]);
    rvrbotron::dsp::Diffuser alignedDiffuser(diffuserStage);
    std::array<std::array<rvrbotron::dsp::Sample, 2>, 4> diffuserFrames{};
    const std::array<rvrbotron::dsp::Sample, 2> firstDiffuserInput{
        1.0F, 1.0F};
    const std::array<rvrbotron::dsp::Sample, 2> silentDiffuserInput{
        0.0F, 0.0F};
    for (std::size_t frame = 0; frame < diffuserFrames.size(); ++frame) {
      const auto& in = frame == 0 ? firstDiffuserInput : silentDiffuserInput;
      alignedDiffuser.processFrame(in.data(), diffuserFrames[frame].data());
    }
    const auto activeSet = [](const auto& frames, const std::size_t channel) {
      std::vector<std::size_t> active;
      for (std::size_t frame = 0; frame < frames.size(); ++frame) {
        if (frames[frame][channel] != rvrbotron::dsp::Sample{0}) {
          active.push_back(frame);
        }
      }
      return active;
    };
    const auto diffuserActive0 = activeSet(diffuserFrames, 0);
    const auto diffuserActive1 = activeSet(diffuserFrames, 1);
    const std::vector<std::size_t> expectedDiffuserActive{0, 1};
    if (diffuserActive0 != expectedDiffuserActive ||
        diffuserActive1 != expectedDiffuserActive) {
      std::cerr << "Diffuser fixture was not aligned as expected\n";
      return 1;
    }

    const auto loopActive0 = activeSet(loopFrames, 0);
    const auto loopActive1 = activeSet(loopFrames, 1);
    bool loopSetsOverlap = false;
    for (const auto frame : loopActive0) {
      if (std::find(loopActive1.begin(), loopActive1.end(), frame) !=
          loopActive1.end()) {
        loopSetsOverlap = true;
        break;
      }
    }
    if (loopActive0.empty() || loopActive1.empty() || loopSetsOverlap) {
      std::cerr << "Feedback Loop active-sample sets were not genuinely "
                   "unaligned against the Diffuser's aligned fixture\n";
      return 1;
    }
  }

  // Block-size bound (#53): derived as the shortest resolved per-Channel
  // delay, independently re-derived here rather than trusting the field --
  // using segmented-random so the minimum is not trivially delayMinSamples,
  // unlike the even-strategy fixtures used elsewhere in this file.
  {
    const auto blockSizeBoundConfig = resolvedFeedbackLoopConfig(
        4,
        0.5,
        5.0,
        10.0,
        rvrbotron::dsp::MixMatrixType::householder,
        rvrbotron::dsp::DelayStrategy::segmentedRandom,
        48000);
    const auto& blockSizeBoundLoop =
        std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
            blockSizeBoundConfig.composition.stages[1]);
    const auto expectedBlockSizeBound = *std::min_element(
        blockSizeBoundLoop.delaysSamples.begin(),
        blockSizeBoundLoop.delaysSamples.end());
    if (blockSizeBoundLoop.blockSizeBoundSamples != expectedBlockSizeBound) {
      std::cerr << "Feedback Loop blockSizeBoundSamples was not the "
                   "minimum resolved per-Channel delay: "
                << blockSizeBoundLoop.blockSizeBoundSamples
                << " != " << expectedBlockSizeBound << '\n';
      return 1;
    }
  }

  // Decay accuracy and stability across deliberately unequal delays,
  // through the full Reverb (Split -> Feedback Loop -> select Downmix).
  // Every Channel's own gain is solved to decay at the same dB/second rate
  // regardless of its own loop time, so the aggregate stereo output should
  // decay at that same rate even though the four Channels here circulate
  // at different periods.
  constexpr std::uint32_t decayChannels = 4;
  constexpr double requestedRt60Sec = 0.3;
  constexpr std::uint32_t decaySampleRate = 8000;
  const auto decayConfig = resolvedFeedbackLoopConfig(
      decayChannels,
      requestedRt60Sec,
      5.0,
      10.0,
      rvrbotron::dsp::MixMatrixType::householder,
      rvrbotron::dsp::DelayStrategy::even,
      decaySampleRate);
  if (decayConfig.sampleRate != decaySampleRate) {
    std::cerr << "Feedback Loop decay fixture sample rate is wrong\n";
    return 1;
  }
  const auto& decayLoopStage = std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
      decayConfig.composition.stages[1]);
  rvrbotron::dsp::Reverb decayReverb(decayConfig);
  const auto decayTailFrames =
      static_cast<std::size_t>(decayReverb.tailBudgetFrames());
  constexpr std::size_t decayGuard = 200;
  constexpr std::size_t decayWindow = 400;
  constexpr std::size_t decaySeparation = 2000;
  if (decayTailFrames < decayGuard + decaySeparation + decayWindow) {
    std::cerr << "Feedback Loop decay fixture Tail budget is too short for "
                 "this test\n";
    return 1;
  }
  const auto decayFrameCount = decayTailFrames + 1;
  std::vector<rvrbotron::dsp::Sample> decayInput(
      decayFrameCount, rvrbotron::dsp::Sample{0});
  decayInput[0] = 1.0F;
  std::vector<rvrbotron::dsp::Sample> decayLeft(decayFrameCount);
  std::vector<rvrbotron::dsp::Sample> decayRight(decayFrameCount);
  // Processed in chunks no larger than the resolved block-size bound (#53).
  const auto decayBlockSize =
      static_cast<std::size_t>(decayLoopStage.blockSizeBoundSamples);
  beginAllocationCount();
  processMonoToStereoInChunks(
      decayReverb, decayInput, decayLeft, decayRight, decayBlockSize);
  const auto decayAllocations = endAllocationCount();
  if (decayAllocations != 0) {
    std::cerr << "Feedback Loop Reverb allocated while processing\n";
    return 1;
  }

  const auto windowEnergy = [&](const std::size_t start,
                                const std::size_t length) {
    double energy = 0.0;
    for (std::size_t frame = start; frame < start + length; ++frame) {
      const auto l = static_cast<double>(decayLeft[frame]);
      const auto r = static_cast<double>(decayRight[frame]);
      energy += l * l + r * r;
    }
    return energy;
  };
  // No-growth / stability sweep: peak absolute sample anywhere in the tail
  // must never exceed a small multiple of the peak in the first window --
  // constructive beating between Channels is expected, runaway growth is
  // not (`gain < 1` and orthogonal mixing guarantee contraction).
  double earlyPeak = 0.0;
  for (std::size_t frame = decayGuard; frame < decayGuard + decayWindow;
       ++frame) {
    earlyPeak = std::max(
        {earlyPeak,
         std::abs(static_cast<double>(decayLeft[frame])),
         std::abs(static_cast<double>(decayRight[frame]))});
  }
  for (std::size_t frame = decayGuard; frame < decayFrameCount; ++frame) {
    const auto peak = std::max(
        std::abs(static_cast<double>(decayLeft[frame])),
        std::abs(static_cast<double>(decayRight[frame])));
    if (peak > 1.5 * earlyPeak) {
      std::cerr << "Feedback Loop tail grew unexpectedly at frame " << frame
                << '\n';
      return 1;
    }
  }

  // Repeat the no-growth sweep per internal Channel directly against the
  // Feedback Loop DSP class, bypassing select Downmix -- which only reads
  // two of the four Channels and could otherwise mask growth confined to a
  // Channel it never selects.
  rvrbotron::dsp::FeedbackLoop decayLoop(decayLoopStage);
  std::vector<std::vector<rvrbotron::dsp::Sample>> decayChannelFrames(
      decayChannels,
      std::vector<rvrbotron::dsp::Sample>(decayFrameCount));
  std::vector<rvrbotron::dsp::Sample> decayLoopInputFrame(
      decayChannels, rvrbotron::dsp::Sample{0});
  std::vector<rvrbotron::dsp::Sample> decayLoopOutputFrame(decayChannels);
  // Matches the Split stage's energy-normalized duplicate mapping, so this
  // direct-DSP fixture receives the same per-Channel impulse as the
  // Reverb-level render above.
  const auto decaySplitGain = static_cast<rvrbotron::dsp::Sample>(
      1.0 / std::sqrt(static_cast<double>(decayChannels)));
  beginAllocationCount();
  for (std::size_t frame = 0; frame < decayFrameCount; ++frame) {
    std::fill(
        decayLoopInputFrame.begin(),
        decayLoopInputFrame.end(),
        frame == 0 ? decaySplitGain : rvrbotron::dsp::Sample{0});
    decayLoop.processFrame(
        decayLoopInputFrame.data(), decayLoopOutputFrame.data());
    for (std::uint32_t channel = 0; channel < decayChannels; ++channel) {
      decayChannelFrames[channel][frame] = decayLoopOutputFrame[channel];
    }
  }
  const auto decayLoopAllocations = endAllocationCount();
  if (decayLoopAllocations != 0) {
    std::cerr << "Feedback Loop allocated while processing the per-Channel "
                 "decay fixture\n";
    return 1;
  }
  for (std::uint32_t channel = 0; channel < decayChannels; ++channel) {
    double channelEarlyPeak = 0.0;
    for (std::size_t frame = decayGuard; frame < decayGuard + decayWindow;
         ++frame) {
      channelEarlyPeak = std::max(
          channelEarlyPeak,
          std::abs(static_cast<double>(decayChannelFrames[channel][frame])));
    }
    for (std::size_t frame = decayGuard; frame < decayFrameCount; ++frame) {
      const auto peak =
          std::abs(static_cast<double>(decayChannelFrames[channel][frame]));
      if (peak > 1.5 * channelEarlyPeak) {
        std::cerr << "Feedback Loop Channel " << channel
                  << " grew unexpectedly at frame " << frame << '\n';
        return 1;
      }
    }
  }

  const auto decayEnergyA = windowEnergy(decayGuard, decayWindow);
  const auto decayEnergyB =
      windowEnergy(decayGuard + decaySeparation, decayWindow);
  if (!(decayEnergyA > 0.0) || !(decayEnergyB > 0.0) ||
      decayEnergyB >= decayEnergyA) {
    std::cerr
        << "Feedback Loop tail did not decay monotonically between windows\n";
    return 1;
  }
  const auto decayDbDrop = 10.0 * std::log10(decayEnergyA / decayEnergyB);
  const auto decayDeltaTSeconds =
      static_cast<double>(decaySeparation) / decaySampleRate;
  const auto impliedRt60 = 60.0 * decayDeltaTSeconds / decayDbDrop;
  if (std::abs(impliedRt60 - requestedRt60Sec) > 0.05 * requestedRt60Sec) {
    std::cerr << "Feedback Loop decay accuracy outside +-5%: implied "
              << impliedRt60 << "s vs requested " << requestedRt60Sec
              << "s\n";
    return 1;
  }

  // gainMode (#56): "uniform" solves one shared gain from the mean loop
  // time across Channels rather than each Channel's own. A much wider
  // delay spread than the fixture above -- Householder mixing otherwise
  // blends the Channels' individual decay rates together enough that a
  // 2x spread barely shows up in the aggregate two-window measurement --
  // makes the two modes' accuracy difference unambiguous. Measured the
  // same way as the accuracy check above: full Reverb, select Downmix,
  // two-window Schroeder-style energy.
  {
    constexpr double gainModeDelayMinMs = 0.5;
    constexpr double gainModeDelayMaxMs = 20.0;
    const auto perChannelWideConfig = resolvedFeedbackLoopConfig(
        decayChannels,
        requestedRt60Sec,
        gainModeDelayMinMs,
        gainModeDelayMaxMs,
        rvrbotron::dsp::MixMatrixType::householder,
        rvrbotron::dsp::DelayStrategy::even,
        decaySampleRate,
        rvrbotron::dsp::GainMode::perChannel);
    const auto uniformWideConfig = resolvedFeedbackLoopConfig(
        decayChannels,
        requestedRt60Sec,
        gainModeDelayMinMs,
        gainModeDelayMaxMs,
        rvrbotron::dsp::MixMatrixType::householder,
        rvrbotron::dsp::DelayStrategy::even,
        decaySampleRate,
        rvrbotron::dsp::GainMode::uniform);
    const auto& perChannelWideLoop =
        std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
            perChannelWideConfig.composition.stages[1]);
    const auto& uniformWideLoop =
        std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
            uniformWideConfig.composition.stages[1]);
    if (perChannelWideLoop.gainMode != rvrbotron::dsp::GainMode::perChannel ||
        uniformWideLoop.gainMode != rvrbotron::dsp::GainMode::uniform) {
      std::cerr << "gainMode comparison fixtures did not resolve to the "
                   "expected modes\n";
      return 1;
    }
    if (perChannelWideLoop.delaysSamples != uniformWideLoop.delaysSamples) {
      std::cerr << "gainMode comparison fixtures' delays were not "
                   "comparable to each other\n";
      return 1;
    }
    for (const auto gain : uniformWideLoop.gains) {
      if (gain != uniformWideLoop.gains.front()) {
        std::cerr << "gainMode uniform did not solve one shared gain "
                     "across Channels: " << gain
                  << " != " << uniformWideLoop.gains.front() << '\n';
        return 1;
      }
    }

    const auto measureImpliedRt60 = [&](const rvrbotron::dsp::ResolvedConfig&
                                             config) {
      rvrbotron::dsp::Reverb reverb(config);
      const auto tailFrames =
          static_cast<std::size_t>(reverb.tailBudgetFrames());
      const auto frameCount = tailFrames + 1;
      std::vector<rvrbotron::dsp::Sample> input(
          frameCount, rvrbotron::dsp::Sample{0});
      input[0] = 1.0F;
      std::vector<rvrbotron::dsp::Sample> left(frameCount);
      std::vector<rvrbotron::dsp::Sample> right(frameCount);
      const auto& loop = std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
          config.composition.stages[1]);
      const auto blockSize =
          static_cast<std::size_t>(loop.blockSizeBoundSamples);
      processMonoToStereoInChunks(reverb, input, left, right, blockSize);
      const auto windowEnergyAt = [&](const std::size_t start,
                                      const std::size_t length) {
        double energy = 0.0;
        for (std::size_t frame = start; frame < start + length; ++frame) {
          const auto l = static_cast<double>(left[frame]);
          const auto r = static_cast<double>(right[frame]);
          energy += l * l + r * r;
        }
        return energy;
      };
      const auto energyA = windowEnergyAt(decayGuard, decayWindow);
      const auto energyB =
          windowEnergyAt(decayGuard + decaySeparation, decayWindow);
      if (!(energyA > 0.0) || !(energyB > 0.0) || energyB >= energyA) {
        return std::optional<double>();
      }
      const auto dbDrop = 10.0 * std::log10(energyA / energyB);
      return std::optional<double>(60.0 * decayDeltaTSeconds / dbDrop);
    };

    if (perChannelWideLoop.tailBudgetSamples <
        decayGuard + decaySeparation + decayWindow) {
      std::cerr
          << "gainMode comparison fixture's Tail budget is too short\n";
      return 1;
    }
    const auto impliedPerChannelWideOpt =
        measureImpliedRt60(perChannelWideConfig);
    const auto impliedUniformWideOpt = measureImpliedRt60(uniformWideConfig);
    if (!impliedPerChannelWideOpt.has_value() ||
        !impliedUniformWideOpt.has_value()) {
      std::cerr << "gainMode comparison fixture did not decay "
                   "monotonically between windows\n";
      return 1;
    }
    const auto impliedPerChannelWide = *impliedPerChannelWideOpt;
    const auto impliedUniformWide = *impliedUniformWideOpt;
    const auto perChannelRelativeError =
        std::abs(impliedPerChannelWide - requestedRt60Sec) /
        requestedRt60Sec;
    const auto uniformRelativeError =
        std::abs(impliedUniformWide - requestedRt60Sec) / requestedRt60Sec;
    if (perChannelRelativeError > 0.05) {
      std::cerr << "gainMode per-channel decay accuracy outside +-5% at "
                   "this delay spread: implied "
                << impliedPerChannelWide << "s vs requested "
                << requestedRt60Sec << "s\n";
      return 1;
    }
    // uniform must be measurably, not marginally, worse -- a wide margin
    // over per-channel's own error rather than a hardcoded fraction, so
    // the assertion doesn't depend on recomputing the production gain
    // formula's exact expected value.
    if (!(uniformRelativeError > 4.0 * perChannelRelativeError)) {
      std::cerr << "gainMode uniform was not measurably less accurate than "
                   "per-channel across a wide delay spread: implied "
                << impliedUniformWide << "s (uniform) vs "
                << impliedPerChannelWide << "s (per-channel), both against "
                   "requested "
                << requestedRt60Sec << "s\n";
      return 1;
    }
  }

  // Reverb::ownedBytes() accounts a Feedback Loop's delay-storage capacity
  // the same way it does the Diffuser's, above.
  auto baselineLoopConfig = decayConfig;
  auto enlargedLoopConfig = decayConfig;
  auto& enlargedLoop = std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
      enlargedLoopConfig.composition.stages[1]);
  constexpr std::uint64_t extraLoopDelaySamples = 500;
  enlargedLoop.delaysSamples[0] += extraLoopDelaySamples;
  enlargedLoop.bufferSizes[0] += extraLoopDelaySamples;

  rvrbotron::dsp::Reverb baselineLoopReverb(baselineLoopConfig);
  rvrbotron::dsp::Reverb enlargedLoopReverb(enlargedLoopConfig);
  beginAllocationCount();
  const auto baselineLoopBytes = baselineLoopReverb.ownedBytes();
  const auto enlargedLoopBytes = enlargedLoopReverb.ownedBytes();
  const auto loopOwnedBytesAllocations = endAllocationCount();
  if (loopOwnedBytesAllocations != 0) {
    std::cerr << "Feedback Loop Reverb::ownedBytes allocated while "
                 "measuring\n";
    return 1;
  }
  const auto expectedLoopDelta =
      extraLoopDelaySamples * sizeof(rvrbotron::dsp::Sample);
  if (enlargedLoopBytes - baselineLoopBytes != expectedLoopDelta) {
    std::cerr
        << "Feedback Loop Reverb::ownedBytes did not track delay-storage "
           "capacity: "
        << (enlargedLoopBytes - baselineLoopBytes) << " != "
        << expectedLoopDelta << '\n';
    return 1;
  }

  // Diffuser-into-loop chain (#55): [split, diffuser, feedback-loop,
  // downmix]. Same seed and same Feedback Loop parameters as decayConfig
  // above, so the loop's own resolved delays/gains/matrix can be compared
  // directly against it.
  const auto diffuserLoopConfig = resolvedDiffuserThenLoopConfig(
      decayChannels,
      requestedRt60Sec,
      5.0,
      10.0,
      rvrbotron::dsp::MixMatrixType::householder,
      rvrbotron::dsp::DelayStrategy::even,
      decaySampleRate);
  const auto& diffuserLoopStage =
      std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
          diffuserLoopConfig.composition.stages[2]);

  // Loop time for a Channel remains that Channel's own feedback delay,
  // unaffected by Diffuser settings: the loop's resolved values are
  // identical whether or not a Diffuser precedes it.
  if (diffuserLoopStage.delaysSamples != decayLoopStage.delaysSamples ||
      diffuserLoopStage.gains != decayLoopStage.gains ||
      diffuserLoopStage.matrix != decayLoopStage.matrix) {
    std::cerr << "Feedback Loop's own resolved values changed when a "
                 "Diffuser was placed in front of it\n";
    return 1;
  }

  // Total drain is the Diffuser's own finite response plus the loop's Tail
  // budget.
  const auto& diffuserLoopDiffuserStage =
      std::get<rvrbotron::dsp::ResolvedDiffuser>(
          diffuserLoopConfig.composition.stages[1]);
  rvrbotron::dsp::Reverb diffuserLoopReverb(diffuserLoopConfig);
  const auto expectedCombinedTail = diffuserLoopDiffuserStage.totalSamples +
      diffuserLoopStage.tailBudgetSamples;
  if (diffuserLoopReverb.tailBudgetFrames() != expectedCombinedTail) {
    std::cerr << "Diffuser-into-loop Tail budget was not the Diffuser's "
                 "finite response plus the loop's Tail budget: "
              << diffuserLoopReverb.tailBudgetFrames()
              << " != " << expectedCombinedTail << '\n';
    return 1;
  }
  if (diffuserLoopDiffuserStage.totalSamples >=
      *std::min_element(
          decayLoopStage.delaysSamples.begin(),
          decayLoopStage.delaysSamples.end())) {
    std::cerr << "Diffuser-into-loop fixture's Diffuser response is not "
                 "shorter than the loop's fastest delay, which the Echo "
                 "density comparison below assumes\n";
    return 1;
  }

  const auto diffuserLoopFrameCount =
      static_cast<std::size_t>(diffuserLoopReverb.tailBudgetFrames()) + 1;
  std::vector<rvrbotron::dsp::Sample> diffuserLoopInput(
      diffuserLoopFrameCount, rvrbotron::dsp::Sample{0});
  diffuserLoopInput[0] = 1.0F;
  std::vector<rvrbotron::dsp::Sample> diffuserLoopLeft(diffuserLoopFrameCount);
  std::vector<rvrbotron::dsp::Sample> diffuserLoopRight(
      diffuserLoopFrameCount);
  // Processed in chunks no larger than the resolved block-size bound (#53).
  const auto diffuserLoopBlockSize =
      static_cast<std::size_t>(diffuserLoopStage.blockSizeBoundSamples);
  beginAllocationCount();
  processMonoToStereoInChunks(
      diffuserLoopReverb,
      diffuserLoopInput,
      diffuserLoopLeft,
      diffuserLoopRight,
      diffuserLoopBlockSize);
  const auto diffuserLoopAllocations = endAllocationCount();
  if (diffuserLoopAllocations != 0) {
    std::cerr << "Diffuser-into-loop Reverb allocated while processing\n";
    return 1;
  }

  // Comparable decay time: the same two-window energy measurement used for
  // the loop-only fixture above, at the same guard/window/separation, must
  // land within the same requested-RT60 tolerance -- and close to the
  // loop-only fixture's own implied RT60.
  if (diffuserLoopFrameCount < decayGuard + decaySeparation + decayWindow) {
    std::cerr << "Diffuser-into-loop fixture Tail budget is too short for "
                 "this test\n";
    return 1;
  }
  const auto diffuserLoopWindowEnergy = [&](const std::size_t start,
                                            const std::size_t length) {
    double energy = 0.0;
    for (std::size_t frame = start; frame < start + length; ++frame) {
      const auto l = static_cast<double>(diffuserLoopLeft[frame]);
      const auto r = static_cast<double>(diffuserLoopRight[frame]);
      energy += l * l + r * r;
    }
    return energy;
  };
  const auto diffuserLoopEnergyA =
      diffuserLoopWindowEnergy(decayGuard, decayWindow);
  const auto diffuserLoopEnergyB =
      diffuserLoopWindowEnergy(decayGuard + decaySeparation, decayWindow);
  if (!(diffuserLoopEnergyA > 0.0) || !(diffuserLoopEnergyB > 0.0) ||
      diffuserLoopEnergyB >= diffuserLoopEnergyA) {
    std::cerr << "Diffuser-into-loop tail did not decay monotonically "
                 "between windows\n";
    return 1;
  }
  const auto diffuserLoopDbDrop =
      10.0 * std::log10(diffuserLoopEnergyA / diffuserLoopEnergyB);
  const auto impliedDiffuserLoopRt60 =
      60.0 * decayDeltaTSeconds / diffuserLoopDbDrop;
  if (std::abs(impliedDiffuserLoopRt60 - requestedRt60Sec) >
      0.05 * requestedRt60Sec) {
    std::cerr << "Diffuser-into-loop decay accuracy outside +-5%: implied "
              << impliedDiffuserLoopRt60 << "s vs requested "
              << requestedRt60Sec << "s\n";
    return 1;
  }
  if (std::abs(impliedDiffuserLoopRt60 - impliedRt60) >
      0.05 * requestedRt60Sec) {
    std::cerr << "Diffuser-into-loop decay time was not comparable to the "
                 "loop-only decay time at the same rt60Sec: "
              << impliedDiffuserLoopRt60 << "s vs " << impliedRt60 << "s\n";
    return 1;
  }

  // Materially different Echo density: count distinct nonzero-arrival
  // frames within the same early window used for the stability sweep above
  // (well past both fixtures' first delay-tap return). The loop-only
  // render can only produce one arrival per Channel per period, from each
  // Channel's own single delayed impulse; the Diffuser's dense response
  // recirculating through the same loop produces many more.
  const auto countArrivals = [](const auto& left, const auto& right,
                                const std::size_t start,
                                const std::size_t length) {
    std::size_t arrivals = 0;
    for (std::size_t frame = start; frame < start + length; ++frame) {
      if (left[frame] != rvrbotron::dsp::Sample{0} ||
          right[frame] != rvrbotron::dsp::Sample{0}) {
        ++arrivals;
      }
    }
    return arrivals;
  };
  const auto loopOnlyArrivals =
      countArrivals(decayLeft, decayRight, decayGuard, decayWindow);
  const auto diffuserLoopArrivals = countArrivals(
      diffuserLoopLeft, diffuserLoopRight, decayGuard, decayWindow);
  if (diffuserLoopArrivals < 2 * loopOnlyArrivals) {
    std::cerr << "Diffuser-into-loop Echo density was not materially "
                 "greater than the loop-only render's: "
              << diffuserLoopArrivals << " vs " << loopOnlyArrivals << '\n';
    return 1;
  }

  // Denormal flush (#54): an explicit threshold flush in the feedback write
  // path guarantees a sub-normal magnitude never persists in the delay
  // line, independent of the still-dormant silenceFloorDb seam. A
  // one-Channel, one-sample-delay fixture with a slow decay (gain close to
  // but below one) spends thousands of frames in the target range instead
  // of skipping past it in one step, so the flush's own boundary is
  // genuinely exercised rather than merely reached at the end.
  {
    constexpr std::uint32_t denormalSampleRate = 48000;
    constexpr double denormalDelayMs = 1000.0 / denormalSampleRate;
    // Solves to gain ~= 0.99 at the resolved one-sample loop time.
    constexpr double denormalRt60Sec = 0.01432;
    const auto denormalConfig = resolvedFeedbackLoopConfig(
        1,
        denormalRt60Sec,
        denormalDelayMs,
        denormalDelayMs,
        rvrbotron::dsp::MixMatrixType::householder,
        rvrbotron::dsp::DelayStrategy::even,
        denormalSampleRate);
    const auto& denormalLoopStage =
        std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
            denormalConfig.composition.stages[1]);
    if (denormalLoopStage.delaysSamples != std::vector<std::uint64_t>{1}) {
      std::cerr
          << "Denormal-flush fixture did not resolve to a single-sample "
             "delay\n";
      return 1;
    }

    rvrbotron::dsp::FeedbackLoop denormalLoop(denormalLoopStage);
    // Comfortably past the frame at which both float and double Sample
    // reach the smallest normal magnitude at this gain (~8.7k frames for
    // float, ~70.5k for double).
    constexpr std::size_t denormalFrames = 200000;
    std::vector<rvrbotron::dsp::Sample> denormalInput(
        denormalFrames, rvrbotron::dsp::Sample{0});
    denormalInput[0] = 1.0F;
    rvrbotron::dsp::Sample outputFrame{};
    const auto denormalThreshold = static_cast<double>(
        std::numeric_limits<rvrbotron::dsp::Sample>::min());
    bool sawNearThresholdMagnitude = false;
    beginAllocationCount();
    for (std::size_t frame = 0; frame < denormalFrames; ++frame) {
      denormalLoop.processFrame(&denormalInput[frame], &outputFrame);
      const auto magnitude = std::abs(static_cast<double>(outputFrame));
      if (magnitude != 0.0 && magnitude < denormalThreshold) {
        std::cerr << "Feedback Loop let a sub-normal magnitude " << magnitude
                  << " persist in the feedback path at frame " << frame
                  << '\n';
        return 1;
      }
      if (magnitude > 0.0 && magnitude < 1000.0 * denormalThreshold) {
        sawNearThresholdMagnitude = true;
      }
    }
    const auto denormalAllocations = endAllocationCount();
    if (denormalAllocations != 0) {
      std::cerr
          << "Feedback Loop allocated while processing the denormal-flush "
             "fixture\n";
      return 1;
    }
    if (!sawNearThresholdMagnitude) {
      std::cerr << "Denormal-flush fixture never decayed near the flush "
                   "threshold, so the flush was not exercised\n";
      return 1;
    }
    if (outputFrame != rvrbotron::dsp::Sample{0}) {
      std::cerr << "Denormal-flush fixture did not reach exact zero after "
                << denormalFrames << " frames\n";
      return 1;
    }
  }

  // DelayLine::readFraction (issue #89): third-order Lagrange
  // interpolation of a perfectly linear ramp must reproduce the line
  // exactly, at both an integer lookback (frac=0, degenerating to a
  // direct read) and a fractional one -- a property of Lagrange
  // interpolation itself, independent of this class's implementation, so
  // it is a real oracle rather than a self-consistency check.
  {
    rvrbotron::dsp::DelayLine ramp({5}, {10});
    for (std::uint64_t frame = 0; frame < 10; ++frame) {
      ramp.write(0, static_cast<rvrbotron::dsp::Sample>(frame + 1));
    }
    if (!close(ramp.readFraction(0, 5.0), 6.0)) {
      std::cerr << "DelayLine::readFraction at an integer lookback did not "
                   "match the value written that many frames ago\n";
      return 1;
    }
    if (!close(ramp.readFraction(0, 5.5), 5.5)) {
      std::cerr << "DelayLine::readFraction did not reproduce a linear "
                   "ramp exactly at a fractional lookback\n";
      return 1;
    }
  }

  // DelayLine::readFractionLinear (issue #92): the deliberate ablation
  // alongside readFraction's third-order Lagrange. A ramp cannot
  // distinguish the two methods -- linear interpolation reproduces any
  // linear function exactly too, the same property the oracle above
  // relies on -- so this checks the two-point formula directly against
  // hand-computed values on a single-impulse signal instead.
  {
    rvrbotron::dsp::DelayLine impulse({5}, {10});
    const std::array<rvrbotron::dsp::Sample, 10> impulseSequence{
        0, 0, 0, 0, 1, 0, 0, 0, 0, 0};
    for (const auto value : impulseSequence) {
      impulse.write(0, value);
    }
    if (!close(impulse.readFractionLinear(0, 6.0), 1.0) ||
        !close(impulse.readFractionLinear(0, 5.0), 0.0)) {
      std::cerr << "DelayLine::readFractionLinear at an integer lookback "
                   "did not match the value written that many frames ago\n";
      return 1;
    }
    if (!close(impulse.readFractionLinear(0, 5.5), 0.5)) {
      std::cerr << "DelayLine::readFractionLinear did not average its two "
                   "neighboring samples at a half-integer lookback\n";
      return 1;
    }
  }

  // DelayLine::readFractionAllpass (issue #93): the third deliberate
  // ablation, checked directly against the hand-computed first-order
  // Thiran allpass difference equation -- y = a*x0 + x1 - a*state, with
  // a = (1-frac)/(1+frac) -- rather than relying on convergence
  // behavior, since this filter carries state across calls and its
  // *instantaneous* (not just steady-state) output is what the DSP
  // layer actually reads every frame.
  {
    rvrbotron::dsp::DelayLine allpassLine({5}, {10});
    const std::array<rvrbotron::dsp::Sample, 10> sequence{
        0, 0, 0, 0, 1, 3, 0, 0, 0, 0};
    for (const auto value : sequence) {
      allpassLine.write(0, value);
    }
    // lookback 5 -> sequence[10-5] = sequence[5] = 3 (x0); lookback 6 ->
    // sequence[10-6] = sequence[4] = 1 (x1); see the ramp oracle above
    // for the lookback-k -> sequence[N-k] correspondence.
    rvrbotron::dsp::Sample state = 0.25F;
    const auto expectedCoefficient = (1.0 - 0.5) / (1.0 + 0.5);
    const auto expectedOutput =
        expectedCoefficient * 3.0 + 1.0 - expectedCoefficient * 0.25;
    const auto output = allpassLine.readFractionAllpass(0, 5.5, state);
    if (!close(output, expectedOutput)) {
      std::cerr << "DelayLine::readFractionAllpass did not match the "
                   "hand-computed first-order Thiran allpass output "
                   "(expected "
                << expectedOutput << ", got " << output << ")\n";
      return 1;
    }
    if (!close(state, expectedOutput)) {
      std::cerr << "DelayLine::readFractionAllpass did not overwrite its "
                   "caller-owned state with its own new output\n";
      return 1;
    }
    // At an integer lookback (frac=0), the coefficient is exactly 1: the
    // formula still carries state (y = x0 + x1 - state), never
    // collapsing to a bare read the way readFraction/readFractionLinear
    // do -- the documented reason zero-depth Modulation must bypass by
    // construction rather than rely on allpass arithmetic to reach
    // identity.
    rvrbotron::dsp::Sample integerState = 0.0F;
    const auto integerOutput =
        allpassLine.readFractionAllpass(0, 5.0, integerState);
    if (close(integerOutput, 3.0)) {
      std::cerr << "DelayLine::readFractionAllpass unexpectedly collapsed "
                   "to a bare read at an integer lookback -- the resolved "
                   "bypass this class relies on may no longer be "
                   "necessary\n";
      return 1;
    }
  }

  // Modulation (issue #89): zero depth must render bit-identical to
  // Modulation omitted entirely, guaranteed by the resolved bypass
  // (config::resolveConfig never reserves buffer headroom or derives
  // per-Channel seeds/rates at depthMs == 0) rather than by arithmetic
  // collapsing to identity.
  {
    // Short delays (5-10ms) so the impulse actually circulates within
    // this fixture's frame budget -- 40/60ms (thousands of samples)
    // would leave both loops silent for the whole window, making the
    // comparison pass vacuously regardless of correctness (PR #97
    // follow-up review).
    const auto omittedConfig =
        resolvedModulatedLoopConfig(2, 1.5, 5.0, 10.0, std::nullopt);
    rvrbotron::config::ModulationConfig zeroModulation;
    zeroModulation.depthMs = 0.0;
    const auto zeroDepthConfig =
        resolvedModulatedLoopConfig(2, 1.5, 5.0, 10.0, zeroModulation);

    const auto& omittedLoopStage =
        std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
            omittedConfig.composition.stages[1]);
    const auto& zeroDepthLoopStage =
        std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
            zeroDepthConfig.composition.stages[1]);
    if (zeroDepthLoopStage.bufferSizes != omittedLoopStage.bufferSizes) {
      std::cerr
          << "Zero-depth Modulation grew the resolved buffer sizes\n";
      return 1;
    }

    rvrbotron::dsp::FeedbackLoop omittedLoop(omittedLoopStage);
    rvrbotron::dsp::FeedbackLoop zeroDepthLoop(zeroDepthLoopStage);
    constexpr std::size_t identityFrames = 2000;
    const std::array<rvrbotron::dsp::Sample, 2> impulse{1.0F, -1.0F};
    const std::array<rvrbotron::dsp::Sample, 2> silence{0.0F, 0.0F};
    for (std::size_t frame = 0; frame < identityFrames; ++frame) {
      std::array<rvrbotron::dsp::Sample, 2> omittedOutput{};
      std::array<rvrbotron::dsp::Sample, 2> zeroDepthOutput{};
      const auto& in = frame == 0 ? impulse : silence;
      omittedLoop.processFrame(in.data(), omittedOutput.data());
      zeroDepthLoop.processFrame(in.data(), zeroDepthOutput.data());
      if (omittedOutput != zeroDepthOutput) {
        std::cerr << "Zero-depth Modulation output diverged from "
                     "Modulation omitted at frame " << frame << '\n';
        return 1;
      }
    }
  }

  // Active Modulation (issue #89): resolved buffer headroom matches
  // Excursion plus the fixed Interpolation margin, repeat renders of an
  // identical configuration are exact, and every rendered sample stays
  // finite -- the "no buffer overrun" invariant, exercised over a long
  // render rather than a handful of frames.
  {
    constexpr std::uint32_t sampleRate = 48000;
    rvrbotron::config::ModulationConfig activeModulation;
    activeModulation.depthMs = 5.0;
    activeModulation.rateHz = 3.0;
    const auto activeConfig = resolvedModulatedLoopConfig(
        2,
        1.5,
        40.0,
        60.0,
        activeModulation,
        rvrbotron::dsp::DelayStrategy::even,
        sampleRate);
    const auto& activeLoopStage =
        std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
            activeConfig.composition.stages[1]);
    if (!activeLoopStage.modulation.has_value()) {
      std::cerr << "Active Modulation did not resolve a Modulation "
                   "object\n";
      return 1;
    }
    const auto expectedExcursion = rvrbotron::config::resolveExcursionSamples(
        activeModulation.depthMs.value(), sampleRate);
    for (std::uint32_t channel = 0; channel < activeLoopStage.channels;
         ++channel) {
      const auto expectedHeadroom =
          static_cast<std::uint64_t>(std::ceil(expectedExcursion)) +
          rvrbotron::config::kModulationInterpolationMarginSamples;
      const auto expectedBufferSize =
          activeLoopStage.delaysSamples[channel] + expectedHeadroom;
      if (activeLoopStage.bufferSizes[channel] != expectedBufferSize) {
        std::cerr << "Active Modulation did not reserve Excursion plus "
                     "the fixed Interpolation margin for Channel "
                  << channel << '\n';
        return 1;
      }
    }

    const auto renderModulated = [&]() {
      rvrbotron::dsp::FeedbackLoop loop(activeLoopStage);
      constexpr std::size_t modulatedFrames = 20000;
      std::vector<std::array<rvrbotron::dsp::Sample, 2>> outputs(
          modulatedFrames);
      const std::array<rvrbotron::dsp::Sample, 2> impulse{1.0F, -1.0F};
      const std::array<rvrbotron::dsp::Sample, 2> silence{0.0F, 0.0F};
      for (std::size_t frame = 0; frame < modulatedFrames; ++frame) {
        const auto& in = frame == 0 ? impulse : silence;
        loop.processFrame(in.data(), outputs[frame].data());
      }
      return outputs;
    };

    const auto firstRun = renderModulated();
    for (const auto& frame : firstRun) {
      for (const auto sample : frame) {
        if (!std::isfinite(static_cast<double>(sample))) {
          std::cerr
              << "Modulated Feedback Loop produced a non-finite sample\n";
          return 1;
        }
      }
    }
    const auto secondRun = renderModulated();
    if (firstRun != secondRun) {
      std::cerr << "Modulated Feedback Loop was not deterministic across "
                   "repeat renders\n";
      return 1;
    }

    // linear interpolation (issue #92): the deliberate ablation, at the
    // same depthMs/rateHz/seed as the lagrange3 case just above, so the
    // two are directly comparable. The Interpolation margin -- and
    // therefore the resolved buffer size -- does not move when the
    // method changes, but the rendered output does, proving processFrame
    // actually dispatched to DelayLine::readFractionLinear rather than
    // silently reusing Lagrange3's path.
    auto linearModulation = activeModulation;
    linearModulation.interpolation = rvrbotron::dsp::ModulationInterpolation::linear;
    const auto linearConfig = resolvedModulatedLoopConfig(
        2,
        1.5,
        40.0,
        60.0,
        linearModulation,
        rvrbotron::dsp::DelayStrategy::even,
        sampleRate);
    const auto& linearLoopStage =
        std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
            linearConfig.composition.stages[1]);
    if (!linearLoopStage.modulation.has_value() ||
        linearLoopStage.modulation->interpolation !=
            rvrbotron::dsp::ModulationInterpolation::linear) {
      std::cerr << "Requested linear interpolation did not resolve onto "
                   "the Modulation object\n";
      return 1;
    }
    if (linearLoopStage.bufferSizes != activeLoopStage.bufferSizes) {
      std::cerr << "linear interpolation resolved different buffer sizes "
                   "than lagrange3 at the same depthMs\n";
      return 1;
    }

    rvrbotron::dsp::FeedbackLoop linearLoop(linearLoopStage);
    constexpr std::size_t modulatedFrames = 20000;
    std::vector<std::array<rvrbotron::dsp::Sample, 2>> linearRun(
        modulatedFrames);
    const std::array<rvrbotron::dsp::Sample, 2> impulse{1.0F, -1.0F};
    const std::array<rvrbotron::dsp::Sample, 2> silence{0.0F, 0.0F};
    auto linearOutputDiffered = false;
    for (std::size_t frame = 0; frame < modulatedFrames; ++frame) {
      const auto& in = frame == 0 ? impulse : silence;
      linearLoop.processFrame(in.data(), linearRun[frame].data());
      for (const auto sample : linearRun[frame]) {
        if (!std::isfinite(static_cast<double>(sample))) {
          std::cerr << "linear-interpolated Feedback Loop produced a "
                       "non-finite sample\n";
          return 1;
        }
      }
      if (linearRun[frame] != firstRun[frame]) {
        linearOutputDiffered = true;
      }
    }
    if (!linearOutputDiffered) {
      std::cerr << "linear interpolation rendered output identical to "
                   "lagrange3 at every frame\n";
      return 1;
    }

    // allpass interpolation (issue #93): the third deliberate ablation,
    // at the same depthMs/rateHz/seed as lagrange3 and linear above, so
    // all three are directly comparable. Resolved buffer sizes stay
    // unchanged, but DSP-owned memory grows -- allpass's own per-Channel
    // filter state, counted toward the owning FeedbackLoop -- and the
    // rendered output differs from both other methods, over a long
    // enough drive (20000 frames at a nonzero rate) to cross many
    // integer-lookback boundaries, where the filter's coefficient sits
    // exactly at its marginally-stable extreme (see
    // DelayLine::readFractionAllpass). Every sample must still stay
    // finite and bounded: an unbounded run here would be exactly the
    // evidence issue #93 permits recording as "not viable inside the
    // Feedback Loop."
    auto allpassModulation = activeModulation;
    allpassModulation.interpolation =
        rvrbotron::dsp::ModulationInterpolation::allpass;
    const auto allpassConfig = resolvedModulatedLoopConfig(
        2,
        1.5,
        40.0,
        60.0,
        allpassModulation,
        rvrbotron::dsp::DelayStrategy::even,
        sampleRate);
    const auto& allpassLoopStage =
        std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
            allpassConfig.composition.stages[1]);
    if (!allpassLoopStage.modulation.has_value() ||
        allpassLoopStage.modulation->interpolation !=
            rvrbotron::dsp::ModulationInterpolation::allpass) {
      std::cerr << "Requested allpass interpolation did not resolve onto "
                   "the Modulation object\n";
      return 1;
    }
    if (allpassLoopStage.bufferSizes != activeLoopStage.bufferSizes) {
      std::cerr << "allpass interpolation resolved different buffer "
                   "sizes than lagrange3 at the same depthMs\n";
      return 1;
    }

    rvrbotron::dsp::FeedbackLoop lagrange3LoopForBytes(activeLoopStage);
    rvrbotron::dsp::FeedbackLoop allpassLoop(allpassLoopStage);
    if (allpassLoop.ownedBytes() <= lagrange3LoopForBytes.ownedBytes()) {
      std::cerr << "an allpass-modulated Feedback Loop did not own more "
                   "bytes than an identically shaped lagrange3 one ("
                << allpassLoop.ownedBytes()
                << " <= " << lagrange3LoopForBytes.ownedBytes() << ")\n";
      return 1;
    }

    std::vector<std::array<rvrbotron::dsp::Sample, 2>> allpassRun(
        modulatedFrames);
    auto allpassOutputDiffered = false;
    double allpassMaxAbsSample = 0.0;
    for (std::size_t frame = 0; frame < modulatedFrames; ++frame) {
      const auto& in = frame == 0 ? impulse : silence;
      allpassLoop.processFrame(in.data(), allpassRun[frame].data());
      for (const auto sample : allpassRun[frame]) {
        const auto magnitude = std::abs(static_cast<double>(sample));
        if (!std::isfinite(magnitude)) {
          std::cerr << "allpass-interpolated Feedback Loop produced a "
                       "non-finite sample at frame "
                    << frame << '\n';
          return 1;
        }
        allpassMaxAbsSample = std::max(allpassMaxAbsSample, magnitude);
      }
      if (allpassRun[frame] != firstRun[frame] &&
          allpassRun[frame] != linearRun[frame]) {
        allpassOutputDiffered = true;
      }
    }
    if (!allpassOutputDiffered) {
      std::cerr << "allpass interpolation rendered output identical to "
                   "both lagrange3 and linear at every frame\n";
      return 1;
    }
    // The driving impulse has magnitude 1.0 per Channel; a healthy
    // decaying tail should never exceed a handful of multiples of that,
    // let alone grow toward the marginal pole's own theoretical ceiling.
    // This is a coarse smoke bound, not the full evidence recorded in
    // the design doc (see docs/design/reverb/stages/06-modulation.md's
    // "allpass viability inside the Feedback Loop").
    if (allpassMaxAbsSample > 100.0) {
      std::cerr << "allpass-interpolated Feedback Loop output grew far "
                   "past the driving impulse's own magnitude (max |sample| "
                << allpassMaxAbsSample
                << ") -- possible instability at the marginal pole\n";
      return 1;
    }
  }

  // Modulation at the resolution boundary (issue #89): construct
  // DelayLine/Modulation directly at the shortest delay resolution's own
  // rejection rule (config::modulationFitsDelay) still permits, drive the
  // trajectory hard every frame, and confirm every read stays finite --
  // "no buffer overrun at maximum depth, verified at the extreme of the
  // permitted range."
  {
    constexpr double excursionSamples = 4.0;
    constexpr auto margin =
        rvrbotron::config::kModulationInterpolationMarginSamples;
    const auto minimumRejectedDelay =
        static_cast<std::uint64_t>(std::ceil(excursionSamples)) + margin;
    const auto minimumSafeDelay = minimumRejectedDelay + 1;
    if (rvrbotron::config::modulationFitsDelay(
            minimumRejectedDelay, excursionSamples) ||
        !rvrbotron::config::modulationFitsDelay(
            minimumSafeDelay, excursionSamples)) {
      std::cerr << "modulationFitsDelay's rejection boundary was not where "
                   "this fixture expected\n";
      return 1;
    }
    const auto bufferSize = minimumSafeDelay +
        static_cast<std::uint64_t>(std::ceil(excursionSamples)) + margin;
    rvrbotron::dsp::DelayLine boundaryDelayLine(
        {minimumSafeDelay}, {bufferSize});
    rvrbotron::dsp::ResolvedModulation boundaryModulationConfig;
    boundaryModulationConfig.channelSeeds = {987654321ULL};
    boundaryModulationConfig.channelTargetsPerSample = {0.9};
    boundaryModulationConfig.channelPhases = {0.37};
    boundaryModulationConfig.excursionSamples = excursionSamples;
    rvrbotron::dsp::Modulation boundaryModulation(boundaryModulationConfig);

    for (std::uint64_t frame = 0; frame < 5000; ++frame) {
      const auto lookback =
          boundaryModulation.lookbackSamples(0, minimumSafeDelay);
      const auto value = boundaryDelayLine.readFraction(0, lookback);
      if (!std::isfinite(static_cast<double>(value))) {
        std::cerr << "Modulation at the resolution boundary produced a "
                     "non-finite read at frame " << frame << '\n';
        return 1;
      }
      boundaryDelayLine.write(
          0,
          static_cast<rvrbotron::dsp::Sample>(
              std::sin(0.3 * static_cast<double>(frame))));
      boundaryModulation.advanceFrame();
    }
  }

  // rateHz of 0 freezes each Channel's fractional offset as a static
  // per-Channel detune spread (issue #89): the trajectory must stay
  // exactly constant across many advanced frames, and away from the
  // nominal delay (otherwise this fixture's seed would not be exercising
  // the static offset at all).
  {
    rvrbotron::dsp::ResolvedModulation frozenModulationConfig;
    frozenModulationConfig.channelSeeds = {12345ULL};
    frozenModulationConfig.channelTargetsPerSample = {0.0};
    frozenModulationConfig.channelPhases = {0.61};
    frozenModulationConfig.excursionSamples = 7.5;
    rvrbotron::dsp::Modulation frozenModulation(frozenModulationConfig);
    const auto firstLookback = frozenModulation.lookbackSamples(0, 100);
    for (int frame = 0; frame < 50; ++frame) {
      frozenModulation.advanceFrame();
    }
    const auto laterLookback = frozenModulation.lookbackSamples(0, 100);
    if (firstLookback != laterLookback) {
      std::cerr << "Zero-rate Modulation trajectory drifted instead of "
                   "staying frozen\n";
      return 1;
    }
    if (firstLookback == 100.0) {
      std::cerr << "Zero-rate Modulation trajectory landed exactly on the "
                   "nominal delay; this fixture's seed did not exercise "
                   "the static detune spread\n";
      return 1;
    }
  }

  // An extreme, but validly resolved (finite, >= 0), per-Channel rate
  // pushes the target counter far beyond what int64_t can represent
  // after only a handful of frames (issue #89, PR #97 review): the
  // lookback must stay finite rather than inheriting undefined behavior
  // from an out-of-range double-to-int64_t conversion.
  {
    rvrbotron::dsp::ResolvedModulation extremeRateConfig;
    extremeRateConfig.channelSeeds = {7ULL};
    extremeRateConfig.channelTargetsPerSample = {1.0e20};
    extremeRateConfig.channelPhases = {0.0};
    extremeRateConfig.excursionSamples = 3.0;
    rvrbotron::dsp::Modulation extremeRateModulation(extremeRateConfig);
    for (int frame = 0; frame < 5; ++frame) {
      const auto lookback =
          extremeRateModulation.lookbackSamples(0, 50);
      if (!std::isfinite(lookback)) {
        std::cerr << "An extreme resolved rate produced a non-finite "
                     "lookback at frame " << frame << '\n';
        return 1;
      }
      extremeRateModulation.advanceFrame();
    }
  }

  // Per-Channel phase decorrelation (issue #89, PR #97 review): two
  // Channels sharing the same trajectory seed and rate but resolved to
  // different phases must read different lookbacks at the very first
  // frame, isolating the phase term's own effect from the seed's --
  // proof that phase actually shifts each Channel's starting position on
  // its own trajectory rather than being a documented no-op.
  {
    rvrbotron::dsp::ResolvedModulation sharedSeedConfig;
    sharedSeedConfig.channelSeeds = {42ULL, 42ULL};
    sharedSeedConfig.channelTargetsPerSample = {0.05, 0.05};
    sharedSeedConfig.channelPhases = {0.0, 0.5};
    sharedSeedConfig.excursionSamples = 10.0;
    rvrbotron::dsp::Modulation sharedSeedModulation(sharedSeedConfig);
    const auto firstChannelLookback =
        sharedSeedModulation.lookbackSamples(0, 200);
    const auto secondChannelLookback =
        sharedSeedModulation.lookbackSamples(1, 200);
    if (firstChannelLookback == secondChannelLookback) {
      std::cerr << "Channels with the same trajectory seed and rate but "
                   "different resolved phases read identical lookbacks "
                   "at frame 0\n";
      return 1;
    }
  }

  // sine and triangle shapes (issue #90): rateHz means the same thing
  // for every shape -- one full target-grid cycle per 1/rateHz seconds
  // -- so both waveforms must hit the same checkpoints (zero at t=0,
  // peak +1 at t=0.25, zero at t=0.5, trough -1 at t=0.75) at the exact
  // same frame indices, while still differing from each other, and from
  // smoothed-random, in between those checkpoints.
  {
    const auto trajectoryIsClose =
        [](const rvrbotron::dsp::ModulationShape shape,
           const int frame,
           const double expectedTrajectory) {
          rvrbotron::dsp::ResolvedModulation config;
          config.shape = shape;
          config.channelSeeds = {1ULL};
          // 0.125 per frame: frame 0/2/4/6 land exactly on t = 0, 0.25,
          // 0.5, 0.75.
          config.channelTargetsPerSample = {0.125};
          config.channelPhases = {0.0};
          config.excursionSamples = 1.0;
          rvrbotron::dsp::Modulation modulation(config);
          for (int step = 0; step < frame; ++step) {
            modulation.advanceFrame();
          }
          const auto lookback = modulation.lookbackSamples(0, 1000);
          return close(
              static_cast<rvrbotron::dsp::Sample>(lookback),
              1000.0 + expectedTrajectory);
        };
    const struct {
      rvrbotron::dsp::ModulationShape shape;
      const char* name;
    } shapes[] = {
        {rvrbotron::dsp::ModulationShape::sine, "sine"},
        {rvrbotron::dsp::ModulationShape::triangle, "triangle"},
    };
    for (const auto& entry : shapes) {
      if (!trajectoryIsClose(entry.shape, 0, 0.0) ||
          !trajectoryIsClose(entry.shape, 2, 1.0) ||
          !trajectoryIsClose(entry.shape, 4, 0.0) ||
          !trajectoryIsClose(entry.shape, 6, -1.0)) {
        std::cerr << entry.name
                  << " did not hit the expected checkpoints at rateHz's "
                     "documented meaning (zero/peak/zero/trough at t = "
                     "0, 0.25, 0.5, 0.75)\n";
        return 1;
      }
    }
    // Between checkpoints (t = 0.125) the two shapes must differ from
    // each other -- sine ~= 0.7071, triangle == 0.5 -- proving they are
    // genuinely different waveforms rather than aliasing to the same
    // shape away from the checkpoints they share.
    if (trajectoryIsClose(
            rvrbotron::dsp::ModulationShape::sine, 1, 0.5) ||
        trajectoryIsClose(
            rvrbotron::dsp::ModulationShape::triangle, 1, 1.0 / std::sqrt(2.0))) {
      std::cerr << "sine and triangle produced the same trajectory "
                   "value between their shared checkpoints\n";
      return 1;
    }
  }

  // The fixed +-10% per-Channel rate spread applies to every shape
  // (issue #90): resolving each of the three shapes still gives distinct
  // Channels distinct resolved rates -- checked per shape directly,
  // rather than through only one of them, so a shape-specific
  // resolution regression cannot pass unnoticed (PR #98 review).
  {
    const rvrbotron::dsp::ModulationShape shapes[] = {
        rvrbotron::dsp::ModulationShape::smoothedRandom,
        rvrbotron::dsp::ModulationShape::sine,
        rvrbotron::dsp::ModulationShape::triangle,
    };
    for (const auto shape : shapes) {
      rvrbotron::config::ModulationConfig shapeModulation;
      shapeModulation.depthMs = 0.5;
      shapeModulation.shape = shape;
      const auto shapeConfig = resolvedModulatedLoopConfig(
          2, 1.5, 100.0, 200.0, shapeModulation);
      const auto& shapeLoop = std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
          shapeConfig.composition.stages[1]);
      const auto& shapeRates = shapeLoop.modulation->channelTargetsPerSample;
      if (shapeRates.size() != 2 || shapeRates[0] == shapeRates[1]) {
        std::cerr << "shape index " << static_cast<int>(shape)
                  << " did not resolve distinct per-Channel rates from "
                     "the +-10% seeded spread\n";
        return 1;
      }
    }
  }

  // channelFraction (issue #90): the modulated Channels are the first
  // ceil(fraction * N) entries of a positionally seeded FIXED
  // permutation, so raising the fraction only lengthens the prefix --
  // every Channel already modulating at a smaller fraction stays
  // modulating at a larger one, with no reshuffle.
  {
    rvrbotron::config::ModulationConfig quarterModulation;
    quarterModulation.depthMs = 0.5;
    quarterModulation.channelFraction = 0.25;
    const auto quarterConfig = resolvedModulatedLoopConfig(
        8, 1.5, 100.0, 200.0, quarterModulation);
    const auto& quarterMask =
        std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
            quarterConfig.composition.stages[1])
            .modulation->channelModulated;

    rvrbotron::config::ModulationConfig halfModulation = quarterModulation;
    halfModulation.channelFraction = 0.5;
    const auto halfConfig =
        resolvedModulatedLoopConfig(8, 1.5, 100.0, 200.0, halfModulation);
    const auto& halfMask = std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
        halfConfig.composition.stages[1])
                                .modulation->channelModulated;

    std::size_t quarterCount = 0;
    std::size_t halfCount = 0;
    for (std::size_t channel = 0; channel < 8; ++channel) {
      quarterCount += quarterMask[channel] ? 1 : 0;
      halfCount += halfMask[channel] ? 1 : 0;
      // Every Channel modulating at fraction 0.25 must still be
      // modulating at fraction 0.5 -- the prefix never shrinks or
      // reshuffles as the fraction grows.
      if (quarterMask[channel] && !halfMask[channel]) {
        std::cerr << "raising channelFraction dropped Channel "
                  << channel << " that a smaller fraction had selected\n";
        return 1;
      }
    }
    if (quarterCount != 2 || halfCount != 4) {
      std::cerr << "channelFraction did not resolve ceil(fraction * N) "
                   "modulated Channels: quarter=" << quarterCount
                << " half=" << halfCount << '\n';
      return 1;
    }
  }

  // channelFraction * N can land a few ULPs above the intended integer
  // in binary64 (0.14 * 100 == 14.000000000000002; PR #98 review): the
  // resolved count must still be exactly ceil's documented, mathematical
  // result (14), not one Channel more from that rounding noise.
  {
    rvrbotron::config::ModulationConfig boundaryModulation;
    boundaryModulation.depthMs = 0.5;
    boundaryModulation.channelFraction = 0.14;
    const auto boundaryConfig = resolvedModulatedLoopConfig(
        100, 1.5, 100.0, 200.0, boundaryModulation);
    const auto& boundaryMask = std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
        boundaryConfig.composition.stages[1])
                                    .modulation->channelModulated;
    const auto boundaryCount =
        std::count(boundaryMask.begin(), boundaryMask.end(), true);
    if (boundaryCount != 14) {
      std::cerr << "channelFraction 0.14 over 100 Channels resolved "
                << boundaryCount
                << " modulated Channels, not the documented ceil(0.14 * "
                   "100) = 14 (floating-point boundary regression)\n";
      return 1;
    }
  }

  // Channel selection is independent of delay ordering (issue #90): the
  // same seed and channelFraction give the identical bypass mask
  // regardless of how the Feedback Loop's own delays are ordered by
  // Channel. Comparing two "even" configs at different delay ranges
  // (as this fixture originally did) never actually exercises this --
  // "even" always assigns Channel 0 the shortest delay and Channel N-1
  // the longest, so the Channel-index-to-delay-rank mapping is identical
  // either way, and a (buggy) implementation that selected by delay
  // rank instead of Channel index would pass just as easily (PR #98
  // review). Comparing "even" against "uniform-random" at the same
  // delay range instead gives genuinely different index-to-rank
  // mappings, verified explicitly below rather than assumed.
  {
    rvrbotron::config::ModulationConfig fractionModulation;
    fractionModulation.depthMs = 0.5;
    fractionModulation.channelFraction = 0.5;
    const auto evenConfig = resolvedModulatedLoopConfig(
        8,
        1.5,
        100.0,
        200.0,
        fractionModulation,
        rvrbotron::dsp::DelayStrategy::even);
    const auto randomConfig = resolvedModulatedLoopConfig(
        8,
        1.5,
        100.0,
        200.0,
        fractionModulation,
        rvrbotron::dsp::DelayStrategy::uniformRandom);
    const auto& evenLoop = std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
        evenConfig.composition.stages[1]);
    const auto& randomLoop = std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
        randomConfig.composition.stages[1]);

    const auto delayRankOrder =
        [](const std::vector<std::uint64_t>& delays) {
          std::vector<std::uint32_t> order(delays.size());
          std::iota(order.begin(), order.end(), 0U);
          std::sort(
              order.begin(),
              order.end(),
              [&delays](const std::uint32_t a, const std::uint32_t b) {
                return delays[a] < delays[b];
              });
          return order;
        };
    if (delayRankOrder(evenLoop.delaysSamples) ==
        delayRankOrder(randomLoop.delaysSamples)) {
      std::cerr << "this fixture's \"even\" and \"uniform-random\" delays "
                   "happened to share the same Channel rank order, so it "
                   "did not actually exercise differing delay orderings\n";
      return 1;
    }

    if (evenLoop.modulation->channelModulated !=
        randomLoop.modulation->channelModulated) {
      std::cerr << "Channel selection changed when only the delay "
                   "ordering changed, correlating selection with delay "
                   "ordering\n";
      return 1;
    }
  }

  // channelFraction of 0 disables Modulation for that stage (issue #90),
  // bit-identical to Modulation omitted; any non-zero fraction modulates
  // at least one Channel even when N is large and the fraction is tiny.
  {
    const auto omittedConfig =
        resolvedModulatedLoopConfig(4, 1.5, 100.0, 200.0, std::nullopt);
    rvrbotron::config::ModulationConfig zeroFractionModulation;
    zeroFractionModulation.depthMs = 5.0;
    zeroFractionModulation.channelFraction = 0.0;
    const auto zeroFractionConfig = resolvedModulatedLoopConfig(
        4, 1.5, 100.0, 200.0, zeroFractionModulation);
    const auto& zeroFractionLoop =
        std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
            zeroFractionConfig.composition.stages[1]);
    if (!zeroFractionLoop.modulation->channelModulated.empty()) {
      std::cerr << "channelFraction of 0 still resolved a non-empty "
                   "bypass mask\n";
      return 1;
    }
    if (zeroFractionLoop.bufferSizes !=
        std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
            omittedConfig.composition.stages[1])
            .bufferSizes) {
      std::cerr << "channelFraction of 0 grew the resolved buffer sizes "
                   "over Modulation omitted\n";
      return 1;
    }

    rvrbotron::config::ModulationConfig tinyFractionModulation;
    tinyFractionModulation.depthMs = 0.5;
    tinyFractionModulation.channelFraction = 0.01;
    const auto tinyFractionConfig = resolvedModulatedLoopConfig(
        16, 1.5, 100.0, 200.0, tinyFractionModulation);
    const auto& tinyMask = std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
        tinyFractionConfig.composition.stages[1])
                                .modulation->channelModulated;
    const auto tinyCount =
        std::count(tinyMask.begin(), tinyMask.end(), true);
    if (tinyCount != 1) {
      std::cerr << "a tiny non-zero channelFraction did not modulate "
                   "exactly one Channel (ceil never rounds to zero): got "
                << tinyCount << '\n';
      return 1;
    }
  }

  // Unmodulated Channels inside a modulated stage keep the integer read
  // path, with no Channel reordering (issue #90): a Channel
  // channelFraction excludes renders bit-identical to that same Channel
  // with Modulation omitted entirely, while an included Channel differs.
  {
    // Short delays (5-10ms, a few hundred samples) so the impulse
    // actually arrives well within this fixture's frame budget --
    // delayMinMs/delayMaxMs of 100/200 (milliseconds, thousands of
    // samples) would leave every Channel silent for the whole window.
    rvrbotron::config::ModulationConfig halfFractionModulation;
    halfFractionModulation.depthMs = 1.0;
    halfFractionModulation.rateHz = 2.0;
    halfFractionModulation.channelFraction = 0.5;
    const auto halfFractionConfig = resolvedModulatedLoopConfig(
        2, 1.5, 5.0, 10.0, halfFractionModulation);
    const auto& halfFractionLoopStage =
        std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
            halfFractionConfig.composition.stages[1]);
    const auto& mask = halfFractionLoopStage.modulation->channelModulated;
    const auto bypassedChannel = mask[0] ? std::size_t{1} : std::size_t{0};
    const auto modulatedChannel = mask[0] ? std::size_t{0} : std::size_t{1};

    const auto omittedConfig = resolvedModulatedLoopConfig(
        2, 1.5, 5.0, 10.0, std::nullopt);
    const auto& omittedLoopStage =
        std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
            omittedConfig.composition.stages[1]);

    rvrbotron::dsp::FeedbackLoop halfFractionLoop(halfFractionLoopStage);
    rvrbotron::dsp::FeedbackLoop omittedLoop(omittedLoopStage);
    // Both Channels' own first arrivals (at their own delay lengths,
    // ~240 and ~480 samples) land well inside this window, but the
    // mixing matrix hasn't yet had a chance to circulate the modulated
    // Channel's content into the bypassed one's delay line and back
    // out again (see docs/design/reverb/stages/06-modulation.md's
    // "only some channels need modulating; the mixing matrix
    // distributes detuned content to the rest" -- expected to happen,
    // just not yet, this early). This fixture checks the bypassed
    // Channel's own direct read, not what the matrix eventually mixes
    // into it.
    constexpr std::size_t partialFrames = 600;
    const std::array<rvrbotron::dsp::Sample, 2> impulse{1.0F, 1.0F};
    const std::array<rvrbotron::dsp::Sample, 2> silence{0.0F, 0.0F};
    bool sawDifference = false;
    for (std::size_t frame = 0; frame < partialFrames; ++frame) {
      std::array<rvrbotron::dsp::Sample, 2> halfFractionOutput{};
      std::array<rvrbotron::dsp::Sample, 2> omittedOutput{};
      const auto& in = frame == 0 ? impulse : silence;
      halfFractionLoop.processFrame(in.data(), halfFractionOutput.data());
      omittedLoop.processFrame(in.data(), omittedOutput.data());
      if (halfFractionOutput[bypassedChannel] !=
          omittedOutput[bypassedChannel]) {
        std::cerr << "channelFraction-excluded Channel "
                  << bypassedChannel
                  << " diverged from Modulation omitted at frame " << frame
                  << '\n';
        return 1;
      }
      if (halfFractionOutput[modulatedChannel] !=
          omittedOutput[modulatedChannel]) {
        sawDifference = true;
      }
    }
    if (!sawDifference) {
      std::cerr << "the channelFraction-included Channel never diverged "
                   "from Modulation omitted; this fixture did not "
                   "exercise Modulation at all\n";
      return 1;
    }
  }

  // The Excursion rejection rule applies to modulated Channels only
  // (issue #90): a Channel channelFraction excludes may carry a delay
  // too short to ever serve the requested Excursion without being
  // rejected, while including that same Channel does reject it.
  {
    constexpr double exemptionDelayMinMs = 2.0;
    constexpr double exemptionDelayMaxMs = 250.0;
    rvrbotron::config::ModulationConfig halfFractionModulation;
    halfFractionModulation.channelFraction = 0.5;
    halfFractionModulation.depthMs = 0.5;

    // "even" deterministically assigns Channel 0 the shortest delay
    // (delayMinMs) and Channel 1 the longest (delayMaxMs); try candidate
    // seeds until Channel 0 lands in the excluded half -- the only
    // arrangement this fixture can use, since delayMaxMs is structurally
    // never shorter than delayMinMs.
    std::optional<std::uint64_t> foundSeed;
    for (std::uint64_t candidateSeed = 0; candidateSeed < 64;
         ++candidateSeed) {
      const auto learnConfig = resolvedModulatedLoopConfig(
          2,
          1.5,
          exemptionDelayMinMs,
          exemptionDelayMaxMs,
          halfFractionModulation,
          rvrbotron::dsp::DelayStrategy::even,
          48000,
          candidateSeed);
      const auto& mask = std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
          learnConfig.composition.stages[1])
                              .modulation->channelModulated;
      if (!mask[0] && mask[1]) {
        foundSeed = candidateSeed;
        break;
      }
    }
    if (!foundSeed.has_value()) {
      std::cerr << "could not find a seed placing Channel 0 outside the "
                   "modulated half within 64 tries\n";
      return 1;
    }

    // depthMs high enough that Channel 0's short delay would fail the
    // Excursion rejection rule if it were ever checked (excursion 240
    // samples against a ~96-sample delay).
    rvrbotron::config::ModulationConfig unsafeModulation =
        halfFractionModulation;
    unsafeModulation.depthMs = 5.0;
    bool halfFractionRejected = false;
    try {
      resolvedModulatedLoopConfig(
          2,
          1.5,
          exemptionDelayMinMs,
          exemptionDelayMaxMs,
          unsafeModulation,
          rvrbotron::dsp::DelayStrategy::even,
          48000,
          *foundSeed);
    } catch (const rvrbotron::HarnessError&) {
      halfFractionRejected = true;
    }
    if (halfFractionRejected) {
      std::cerr << "channelFraction excluded Channel 0 but its short "
                   "delay was still rejected\n";
      return 1;
    }

    rvrbotron::config::ModulationConfig fullFractionModulation =
        unsafeModulation;
    fullFractionModulation.channelFraction = 1.0;
    bool fullFractionRejected = false;
    try {
      resolvedModulatedLoopConfig(
          2,
          1.5,
          exemptionDelayMinMs,
          exemptionDelayMaxMs,
          fullFractionModulation,
          rvrbotron::dsp::DelayStrategy::even,
          48000,
          *foundSeed);
    } catch (const rvrbotron::HarnessError&) {
      fullFractionRejected = true;
    }
    if (!fullFractionRejected) {
      std::cerr << "including Channel 0 via channelFraction 1.0 did not "
                   "trigger its short-delay rejection\n";
      return 1;
    }
  }

  // Zero depth remains bit-identical to Modulation omitted for every
  // shape (issue #90): the resolved bypass does not depend on which
  // shape was requested.
  {
    // Short delays (5-10ms) so the impulse actually circulates within
    // this fixture's frame budget -- 100/200ms (thousands of samples)
    // would leave both loops silent for the whole window, making the
    // comparison pass vacuously regardless of correctness (PR #97
    // follow-up review).
    const auto omittedConfig =
        resolvedModulatedLoopConfig(2, 1.5, 5.0, 10.0, std::nullopt);
    const auto& omittedLoopStage =
        std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
            omittedConfig.composition.stages[1]);

    const rvrbotron::dsp::ModulationShape shapes[] = {
        rvrbotron::dsp::ModulationShape::smoothedRandom,
        rvrbotron::dsp::ModulationShape::sine,
        rvrbotron::dsp::ModulationShape::triangle,
    };
    for (const auto shape : shapes) {
      // A fresh FeedbackLoop per shape on both sides of the comparison,
      // since neither is copy- nor move-assignable and each must start
      // silent at frame 0.
      rvrbotron::dsp::FeedbackLoop omittedLoop(omittedLoopStage);

      rvrbotron::config::ModulationConfig zeroDepthModulation;
      zeroDepthModulation.depthMs = 0.0;
      zeroDepthModulation.shape = shape;
      const auto zeroDepthConfig = resolvedModulatedLoopConfig(
          2, 1.5, 5.0, 10.0, zeroDepthModulation);
      const auto& zeroDepthLoopStage =
          std::get<rvrbotron::dsp::ResolvedFeedbackLoop>(
              zeroDepthConfig.composition.stages[1]);
      rvrbotron::dsp::FeedbackLoop zeroDepthLoop(zeroDepthLoopStage);

      const std::array<rvrbotron::dsp::Sample, 2> impulse{1.0F, -1.0F};
      const std::array<rvrbotron::dsp::Sample, 2> silence{0.0F, 0.0F};
      for (std::size_t frame = 0; frame < 2000; ++frame) {
        std::array<rvrbotron::dsp::Sample, 2> zeroDepthOutput{};
        std::array<rvrbotron::dsp::Sample, 2> omittedOutput{};
        const auto& in = frame == 0 ? impulse : silence;
        zeroDepthLoop.processFrame(in.data(), zeroDepthOutput.data());
        omittedLoop.processFrame(in.data(), omittedOutput.data());
        if (zeroDepthOutput != omittedOutput) {
          std::cerr << "zero-depth Modulation output diverged from "
                       "Modulation omitted at frame " << frame
                    << " for shape index "
                    << static_cast<int>(shape) << '\n';
          return 1;
        }
      }
    }
  }

  // Diffusion Step Modulation (issue #91): one-shot, non-compounding
  // detuning scoped to a single step, as distinct from the Feedback
  // Loop's own compounding Modulation above. Probe this seed's own
  // resolved delays with Modulation omitted first, so the depthMs chosen
  // below stays safely within every step's shortest delay regardless of
  // which values segmented-random happens to draw.
  {
    constexpr std::uint32_t channels = 8;
    constexpr double totalMs = 200.0;
    constexpr std::uint32_t stepCount = 2;
    const auto probeConfig = resolvedDiffuserStepModulatedConfig(
        channels, totalMs, stepCount, std::nullopt, std::nullopt);
    const auto& probeDiffuser = std::get<rvrbotron::dsp::ResolvedDiffuser>(
        probeConfig.composition.stages[1]);
    auto minDelay = std::numeric_limits<std::uint64_t>::max();
    for (const auto& step : probeDiffuser.steps) {
      for (const auto delay : step.delaysSamples) {
        minDelay = std::min(minDelay, delay);
      }
    }
    if (minDelay == std::numeric_limits<std::uint64_t>::max() ||
        minDelay < 10) {
      std::cerr << "Diffusion Step Modulation probe resolved an "
                   "unexpectedly short delay -- adjust the test fixture\n";
      return 1;
    }
    // A third of the shortest resolved delay less the fixed Interpolation
    // margin, converted to depthMs at 48 kHz -- comfortably inside the
    // Excursion rejection rule regardless of which Channel ends up
    // shortest.
    const auto safeExcursionSamples =
        static_cast<double>(
            minDelay - rvrbotron::config::kModulationInterpolationMarginSamples) /
        3.0;
    const auto safeDepthMs = safeExcursionSamples * 1000.0 / 48000.0;

    // Explicit zero depth on a step is the resolved bypass: it records
    // evidence that Modulation was configured, grows no buffer headroom,
    // and renders bit-identical to Modulation omitted from that step --
    // mirroring the Feedback Loop's own zero-depth invariant.
    rvrbotron::config::ModulationConfig zeroDepthModulation;
    zeroDepthModulation.depthMs = 0.0;
    const auto zeroDepthConfig = resolvedDiffuserStepModulatedConfig(
        channels,
        totalMs,
        stepCount,
        std::nullopt,
        std::make_pair(std::uint32_t{0}, zeroDepthModulation));
    const auto& zeroDepthDiffuser =
        std::get<rvrbotron::dsp::ResolvedDiffuser>(
            zeroDepthConfig.composition.stages[1]);
    if (!zeroDepthDiffuser.steps[0].modulation.has_value()) {
      std::cerr << "explicit zero depth did not resolve a Modulation "
                   "object on the Diffusion Step\n";
      return 1;
    }
    if (zeroDepthDiffuser.steps[0].bufferSizes !=
        probeDiffuser.steps[0].bufferSizes) {
      std::cerr << "zero-depth Diffusion Step Modulation grew the "
                   "resolved buffer sizes over Modulation omitted\n";
      return 1;
    }

    std::vector<rvrbotron::dsp::Sample> impulse(
        channels, rvrbotron::dsp::Sample{0});
    impulse[0] = rvrbotron::dsp::Sample{1};
    std::vector<rvrbotron::dsp::Sample> silence(
        channels, rvrbotron::dsp::Sample{0});

    {
      // A fresh Diffuser per side of the comparison: neither is copy- nor
      // move-assignable and each must start silent at frame 0 (mirrors
      // the Feedback Loop's own zero-depth comparison above).
      rvrbotron::dsp::Diffuser omittedDiffuser(probeDiffuser);
      rvrbotron::dsp::Diffuser zeroDepthDiffuserDsp(zeroDepthDiffuser);
      for (std::size_t frame = 0; frame < 4000; ++frame) {
        std::vector<rvrbotron::dsp::Sample> omittedOutput(channels);
        std::vector<rvrbotron::dsp::Sample> zeroDepthOutput(channels);
        const auto& in = frame == 0 ? impulse : silence;
        omittedDiffuser.processFrame(in.data(), omittedOutput.data());
        zeroDepthDiffuserDsp.processFrame(in.data(), zeroDepthOutput.data());
        if (omittedOutput != zeroDepthOutput) {
          std::cerr << "zero-depth Diffusion Step Modulation output "
                       "diverged from Modulation omitted at frame "
                    << frame << '\n';
          return 1;
        }
      }
    }

    // Active Modulation on step 0 alone moves that step's delays and
    // renders end to end: buffer headroom grows only on the modulated
    // step, the untouched step 1 keeps its buffer sizes exactly equal to
    // its delays, and the rendered output differs from the unmodulated
    // baseline.
    rvrbotron::config::ModulationConfig activeModulation;
    activeModulation.depthMs = safeDepthMs;
    activeModulation.rateHz = 5.0;
    const auto activeConfig = resolvedDiffuserStepModulatedConfig(
        channels,
        totalMs,
        stepCount,
        std::nullopt,
        std::make_pair(std::uint32_t{0}, activeModulation));
    const auto& activeDiffuser = std::get<rvrbotron::dsp::ResolvedDiffuser>(
        activeConfig.composition.stages[1]);
    const auto& activeStep0 = activeDiffuser.steps[0];
    const auto& activeStep1 = activeDiffuser.steps[1];
    if (activeStep1.modulation.has_value()) {
      std::cerr << "an untouched Diffusion Step resolved Modulation it "
                   "was never configured with\n";
      return 1;
    }
    if (activeStep1.bufferSizes != activeStep1.delaysSamples) {
      std::cerr << "an unmodulated Diffusion Step's buffer sizes grew "
                   "even though it has no Modulation configured\n";
      return 1;
    }
    const auto& activeMod = *activeStep0.modulation;
    const auto headroom =
        static_cast<std::uint64_t>(std::ceil(activeMod.excursionSamples)) +
        rvrbotron::config::kModulationInterpolationMarginSamples;
    for (std::size_t channel = 0; channel < channels; ++channel) {
      const auto expected = activeMod.channelModulated[channel]
          ? activeStep0.delaysSamples[channel] + headroom
          : activeStep0.delaysSamples[channel];
      if (activeStep0.bufferSizes[channel] != expected) {
        std::cerr << "active Diffusion Step Modulation did not reserve "
                     "Excursion plus the fixed Interpolation margin on "
                     "Channel "
                  << channel << '\n';
        return 1;
      }
    }

    {
      rvrbotron::dsp::Diffuser omittedDiffuser(probeDiffuser);
      rvrbotron::dsp::Diffuser activeDiffuserDsp(activeDiffuser);
      auto activeOutputDiffered = false;
      for (std::size_t frame = 0; frame < 4000; ++frame) {
        std::vector<rvrbotron::dsp::Sample> omittedOutput(channels);
        std::vector<rvrbotron::dsp::Sample> activeOutput(channels);
        const auto& in = frame == 0 ? impulse : silence;
        omittedDiffuser.processFrame(in.data(), omittedOutput.data());
        activeDiffuserDsp.processFrame(in.data(), activeOutput.data());
        if (omittedOutput != activeOutput) {
          activeOutputDiffered = true;
        }
      }
      if (!activeOutputDiffered) {
        std::cerr << "active Diffusion Step Modulation rendered output "
                     "identical to the unmodulated baseline\n";
        return 1;
      }
    }

    // linear interpolation on a Diffusion Step (issue #92): the
    // deliberate ablation, at the same depthMs/rateHz/seed as the
    // lagrange3 case just above, so the two are directly comparable. The
    // Interpolation margin -- and therefore the resolved buffer size --
    // does not move when the method changes, but the rendered output
    // does, proving processFrame actually dispatched to
    // DelayLine::readFractionLinear rather than silently reusing
    // Lagrange3's path.
    {
      auto linearModulation = activeModulation;
      linearModulation.interpolation =
          rvrbotron::dsp::ModulationInterpolation::linear;
      const auto linearConfig = resolvedDiffuserStepModulatedConfig(
          channels,
          totalMs,
          stepCount,
          std::nullopt,
          std::make_pair(std::uint32_t{0}, linearModulation));
      const auto& linearDiffuser = std::get<rvrbotron::dsp::ResolvedDiffuser>(
          linearConfig.composition.stages[1]);
      const auto& linearStep0 = linearDiffuser.steps[0];
      if (!linearStep0.modulation.has_value() ||
          linearStep0.modulation->interpolation !=
              rvrbotron::dsp::ModulationInterpolation::linear) {
        std::cerr << "Requested linear interpolation did not resolve onto "
                     "the Diffusion Step's Modulation object\n";
        return 1;
      }
      if (linearStep0.bufferSizes != activeStep0.bufferSizes) {
        std::cerr << "linear interpolation resolved different buffer "
                     "sizes than lagrange3 at the same depthMs on a "
                     "Diffusion Step\n";
        return 1;
      }

      rvrbotron::dsp::Diffuser lagrange3Diffuser(activeDiffuser);
      rvrbotron::dsp::Diffuser linearDiffuserDsp(linearDiffuser);
      auto linearOutputDiffered = false;
      for (std::size_t frame = 0; frame < 4000; ++frame) {
        std::vector<rvrbotron::dsp::Sample> lagrange3Output(channels);
        std::vector<rvrbotron::dsp::Sample> linearOutput(channels);
        const auto& in = frame == 0 ? impulse : silence;
        lagrange3Diffuser.processFrame(in.data(), lagrange3Output.data());
        linearDiffuserDsp.processFrame(in.data(), linearOutput.data());
        for (const auto sample : linearOutput) {
          if (!std::isfinite(static_cast<double>(sample))) {
            std::cerr << "linear-interpolated Diffusion Step produced a "
                         "non-finite sample\n";
            return 1;
          }
        }
        if (lagrange3Output != linearOutput) {
          linearOutputDiffered = true;
        }
      }
      if (!linearOutputDiffered) {
        std::cerr << "linear interpolation on a Diffusion Step rendered "
                     "output identical to lagrange3 at every frame\n";
        return 1;
      }
    }

    // Diffusion Step trajectories are seeded per step and per Channel:
    // two steps modulated with identical parameters (via the shared step
    // defaults) never share a trajectory.
    const auto bothStepsConfig = resolvedDiffuserStepModulatedConfig(
        channels, totalMs, stepCount, activeModulation, std::nullopt);
    const auto& bothStepsDiffuser =
        std::get<rvrbotron::dsp::ResolvedDiffuser>(
            bothStepsConfig.composition.stages[1]);
    if (!bothStepsDiffuser.steps[0].modulation.has_value() ||
        !bothStepsDiffuser.steps[1].modulation.has_value()) {
      std::cerr << "shared step-default Modulation did not apply to "
                   "every Diffusion Step\n";
      return 1;
    }
    if (bothStepsDiffuser.steps[0].modulation->channelSeeds ==
        bothStepsDiffuser.steps[1].modulation->channelSeeds) {
      std::cerr << "two Diffusion Steps modulated with identical "
                   "parameters shared the exact same per-Channel "
                   "trajectory seeds\n";
      return 1;
    }

    // The Excursion rejection rule applies identically to a Diffusion
    // Step, naming the responsible step's own Modulation parameter path
    // -- a short step delay is as safe as a short loop delay.
    rvrbotron::config::ModulationConfig unsafeModulation;
    unsafeModulation.depthMs = 1000.0;
    auto rejected = false;
    try {
      resolvedDiffuserStepModulatedConfig(
          channels,
          totalMs,
          stepCount,
          std::nullopt,
          std::make_pair(std::uint32_t{0}, unsafeModulation));
    } catch (const rvrbotron::HarnessError& error) {
      rejected = true;
      if (!error.location().has_value() ||
          error.location()->find("/steps/0/modulation/depthMs") ==
              std::string::npos) {
        std::cerr << "Diffusion Step Excursion rejection did not name "
                     "the responsible step's own Modulation parameter "
                     "path: "
                  << error.location().value_or("<none>") << '\n';
        return 1;
      }
    }
    if (!rejected) {
      std::cerr << "an Excursion far exceeding the resolved delay was "
                   "not rejected for a Diffusion Step\n";
      return 1;
    }
  }

  return 0;
}
