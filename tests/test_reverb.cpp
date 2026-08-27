#include "rvrbotron/config/ResolveConfig.h"
#include "rvrbotron/dsp/DiffusionStep.h"
#include "rvrbotron/dsp/Diffuser.h"
#include "rvrbotron/dsp/MixMatrix.h"
#include "rvrbotron/dsp/Reverb.h"
#include "rvrbotron/dsp/Split.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
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

bool reverbDiffusionStepIsAllPass(
    const std::uint32_t channels,
    const rvrbotron::dsp::MixMatrixType mix =
        rvrbotron::dsp::MixMatrixType::hadamard) {
  constexpr std::size_t inputFrames = 257;
  const auto config = resolvedDiffusionConfig(channels, mix);
  EnergyCapture capture(channels);
  rvrbotron::dsp::Reverb reverb(config, &capture);
  const auto responseFrames =
      inputFrames + static_cast<std::size_t>(reverb.finiteTailFrames());
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

  return 0;
}
