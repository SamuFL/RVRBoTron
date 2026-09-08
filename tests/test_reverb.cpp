#include "rvrbotron/HarnessError.h"
#include "rvrbotron/config/ModulationResolution.h"
#include "rvrbotron/config/ResolveConfig.h"
#include "rvrbotron/dsp/DelayLine.h"
#include "rvrbotron/dsp/DiffusionStep.h"
#include "rvrbotron/dsp/Diffuser.h"
#include "rvrbotron/dsp/FeedbackLoop.h"
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
          rvrbotron::dsp::EnergyNormalisation::energy,
          1.0,
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

  rvrbotron::config::DownmixConfig downmix;
  downmix.strategy = rvrbotron::dsp::DownmixStrategy::select;
  downmix.normalisation =
      rvrbotron::dsp::EnergyNormalisation::energy;

  rvrbotron::config::CompositionConfig composition;
  composition.stagesSpecified = true;
  composition.stages.emplace_back(split);
  composition.stages.emplace_back(diffuser);
  composition.stages.emplace_back(downmix);

  rvrbotron::config::ReverbConfig requested;
  requested.formatVersion = 1;
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

  rvrbotron::config::DownmixConfig downmix;
  downmix.strategy = rvrbotron::dsp::DownmixStrategy::select;
  downmix.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;

  rvrbotron::config::CompositionConfig composition;
  composition.stagesSpecified = true;
  composition.stages.emplace_back(split);
  composition.stages.emplace_back(loop);
  composition.stages.emplace_back(downmix);

  rvrbotron::config::ReverbConfig requested;
  requested.formatVersion = 1;
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

  rvrbotron::config::DownmixConfig downmix;
  downmix.strategy = rvrbotron::dsp::DownmixStrategy::select;
  downmix.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;

  rvrbotron::config::CompositionConfig composition;
  composition.stagesSpecified = true;
  composition.stages.emplace_back(split);
  composition.stages.emplace_back(loop);
  composition.stages.emplace_back(downmix);

  rvrbotron::config::ReverbConfig requested;
  requested.formatVersion = 1;
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

  rvrbotron::config::DownmixConfig downmix;
  downmix.strategy = rvrbotron::dsp::DownmixStrategy::select;
  downmix.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;

  rvrbotron::config::CompositionConfig composition;
  composition.stagesSpecified = true;
  composition.stages.emplace_back(split);
  composition.stages.emplace_back(diffuser);
  composition.stages.emplace_back(downmix);

  rvrbotron::config::ReverbConfig requested;
  requested.formatVersion = 1;
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

  rvrbotron::config::DownmixConfig downmix;
  downmix.strategy = rvrbotron::dsp::DownmixStrategy::select;
  downmix.normalisation = rvrbotron::dsp::EnergyNormalisation::energy;

  rvrbotron::config::CompositionConfig composition;
  composition.stagesSpecified = true;
  composition.stages.emplace_back(split);
  composition.stages.emplace_back(diffuser);
  composition.stages.emplace_back(loop);
  composition.stages.emplace_back(downmix);

  rvrbotron::config::ReverbConfig requested;
  requested.formatVersion = 1;
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
      1,
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
