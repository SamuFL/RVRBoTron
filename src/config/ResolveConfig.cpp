#include "rvrbotron/config/ResolveConfig.h"

#include "rvrbotron/HarnessError.h"
#include "rvrbotron/config/DampingResolution.h"
#include "rvrbotron/config/MixMatrixResolution.h"
#include "rvrbotron/config/ModulationResolution.h"
#include "rvrbotron/dsp/MathConstants.h"
#include "rvrbotron/dsp/PositionalRandom.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace rvrbotron::config {
namespace {

constexpr std::uint64_t kDiffusionDelayUsage = 0x445354455044454cULL;
constexpr std::uint64_t kDiffusionShuffleUsage = 0x4453544550534846ULL;
constexpr std::uint64_t kDiffusionPolarityUsage = 0x4453544550504f4cULL;
constexpr std::uint64_t kFeedbackLoopDelayUsage = 0x464c4f4f5044454cULL;
constexpr std::uint64_t kFeedbackLoopModulationSeedUsage =
    0x4d4f44554c534544ULL;
constexpr std::uint64_t kFeedbackLoopModulationChannelSelectionUsage =
    0x4d4f44434853454cULL;
constexpr std::uint64_t kDiffusionModulationSeedUsage =
    0x44535445504d5344ULL;
constexpr std::uint64_t kDiffusionModulationChannelSelectionUsage =
    0x44535445504d4353ULL;
constexpr std::uint64_t kMainDownmixRandomOrthogonalUsage =
    0x4d41494e444e4d58ULL;
constexpr std::uint64_t kEarlyDownmixRandomOrthogonalUsage =
    0x4541524c444e4d58ULL;

[[noreturn]] void fail(const std::string_view path,
                       const std::string_view reason) {
  throw HarnessError(
      ErrorCategory::invalidConfiguration,
      std::string(reason),
      std::string(path));
}

std::string stagePath(const std::size_t stageIndex) {
  return "/composition/stages/" + std::to_string(stageIndex);
}

bool isPowerOfTwo(const std::uint32_t value) noexcept {
  return value != 0 && (value & (value - 1U)) == 0;
}

enum class SampleBudgetStatus {
  valid,
  nonPositive,
  tooLarge,
  belowOneSample,
  tooFewPositions,
};

struct SampleBudget {
  SampleBudgetStatus status = SampleBudgetStatus::nonPositive;
  std::uint64_t samples = 0;
};

struct DiffuserDerivation {
  std::uint32_t stepCount = kDefaultDiffuserStepCount;
  double totalMs = kDefaultDiffuserTotalMs;
  SampleBudget sampleBudget;
  bool deriveChannelValues = true;
  std::vector<std::uint64_t> expectedStepLengths;
};

struct ResolutionEvidence {
  std::optional<DiffuserDerivation> diffuser;
};

SampleBudget deriveSampleBudget(const double totalMs,
                                const std::uint32_t sampleRate) noexcept {
  if (!(totalMs > 0.0)) {
    return {SampleBudgetStatus::nonPositive, 0};
  }
  const auto exactSamples =
      static_cast<long double>(totalMs) * sampleRate / 1000.0L;
  if (!std::isfinite(exactSamples) ||
      exactSamples >= static_cast<long double>(
                          std::numeric_limits<std::uint64_t>::max())) {
    return {SampleBudgetStatus::tooLarge, 0};
  }
  const auto roundedSamples = std::floor(exactSamples + 0.5L);
  if (roundedSamples >= static_cast<long double>(
                            std::numeric_limits<std::uint64_t>::max())) {
    return {SampleBudgetStatus::tooLarge, 0};
  }
  const auto samples = static_cast<std::uint64_t>(roundedSamples);
  if (samples == 0) {
    return {SampleBudgetStatus::belowOneSample, 0};
  }
  return {SampleBudgetStatus::valid, samples};
}

bool requiresDistinctChannelPositions(
    const dsp::DelayStrategy strategy) noexcept {
  return strategy == dsp::DelayStrategy::segmentedRandom ||
         strategy == dsp::DelayStrategy::even;
}

bool hasEnoughPositionsForChannels(
    const std::uint64_t lengthSamples,
    const std::uint32_t channels) noexcept {
  return lengthSamples + 1 >= channels;
}

std::optional<std::uint64_t> checkedMul(
    const std::uint64_t a, const std::uint64_t b) noexcept {
  if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
    return std::nullopt;
  }
  return a * b;
}

std::optional<std::uint64_t> checkedAdd(
    const std::uint64_t a, const std::uint64_t b) noexcept {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) {
    return std::nullopt;
  }
  return a + b;
}

std::optional<std::uint64_t> estimateDiffuserMemoryBytes(
    const std::uint32_t channels,
    const std::uint64_t maxBufferSamples,
    const std::uint64_t stepCount,
    const std::optional<std::uint64_t> modulationBytes) noexcept {
  constexpr std::uint64_t kSampleBytes = 8;
  constexpr std::uint64_t kMetadataBytesPerChannel = 24;
  auto delayBytes = checkedMul(channels, maxBufferSamples);
  delayBytes =
      delayBytes ? checkedMul(*delayBytes, kSampleBytes) : std::nullopt;
  const auto matrixElements = checkedMul(channels, channels);
  auto matrixBytesPerStep =
      matrixElements ? checkedMul(*matrixElements, kSampleBytes)
                     : std::nullopt;
  auto matrixBytes = matrixBytesPerStep
                         ? checkedMul(*matrixBytesPerStep, stepCount)
                         : std::nullopt;
  auto metadataBytesPerStep = checkedMul(channels, kMetadataBytesPerChannel);
  auto metadataBytes = metadataBytesPerStep
                           ? checkedMul(*metadataBytesPerStep, stepCount)
                           : std::nullopt;
  if (!delayBytes.has_value() || !matrixBytes.has_value() ||
      !metadataBytes.has_value() || !modulationBytes.has_value()) {
    return std::nullopt;
  }
  auto total = checkedAdd(*delayBytes, *matrixBytes);
  total = total.has_value() ? checkedAdd(*total, *metadataBytes)
                             : std::nullopt;
  total = total.has_value() ? checkedAdd(*total, *modulationBytes)
                             : std::nullopt;
  return total;
}

void checkDiffuserMemoryBudget(
    const std::uint32_t channels,
    const std::uint64_t maxBufferSamples,
    const std::uint64_t stepCount,
    const std::optional<std::uint64_t> modulationBytes,
    const std::uint64_t budgetBytes,
    const std::string_view path) {
  const auto estimate = estimateDiffuserMemoryBytes(
      channels, maxBufferSamples, stepCount, modulationBytes);
  if (!estimate.has_value() || *estimate > budgetBytes) {
    fail(
        path,
        "resolved Diffuser DSP memory footprint exceeds the configured "
        "memory budget");
  }
}

constexpr std::uint64_t kModulationBytesPerChannel = 25;

std::optional<std::uint64_t> estimateDiffuserModulationBytes(
    const std::uint32_t channels,
    const std::vector<dsp::ResolvedDiffusionStep>& steps) noexcept {
  std::uint64_t total = 0;
  for (const auto& step : steps) {
    if (!step.modulation.has_value() ||
        step.modulation->channelModulated.empty()) {
      continue;
    }
    const auto stepBytes = checkedMul(channels, kModulationBytesPerChannel);
    const auto accumulated =
        stepBytes ? checkedAdd(total, *stepBytes) : std::nullopt;
    if (!accumulated.has_value()) {
      return std::nullopt;
    }
    total = *accumulated;
  }
  return total;
}

std::optional<std::uint64_t> estimateFeedbackLoopMemoryBytes(
    const std::uint32_t channels,
    const std::uint64_t maxDelaySamples) noexcept {
  constexpr std::uint64_t kSampleBytes = 8;
  constexpr std::uint64_t kMetadataBytesPerChannel = 16;
  auto delayBytes = checkedMul(channels, maxDelaySamples);
  delayBytes =
      delayBytes ? checkedMul(*delayBytes, kSampleBytes) : std::nullopt;
  const auto matrixElements = checkedMul(channels, channels);
  auto matrixBytes = matrixElements
                         ? checkedMul(*matrixElements, kSampleBytes)
                         : std::nullopt;
  auto metadataBytes = checkedMul(channels, kMetadataBytesPerChannel);
  if (!delayBytes.has_value() || !matrixBytes.has_value() ||
      !metadataBytes.has_value()) {
    return std::nullopt;
  }
  auto total = checkedAdd(*delayBytes, *matrixBytes);
  total =
      total.has_value() ? checkedAdd(*total, *metadataBytes) : std::nullopt;
  return total;
}

void checkFeedbackLoopMemoryBudget(
    const std::uint32_t channels,
    const std::uint64_t maxDelaySamples,
    const std::uint64_t budgetBytes,
    const std::string_view path) {
  const auto estimate =
      estimateFeedbackLoopMemoryBytes(channels, maxDelaySamples);
  if (!estimate.has_value() || *estimate > budgetBytes) {
    fail(
        path,
        "resolved Feedback Loop DSP memory footprint exceeds the "
        "configured memory budget");
  }
}

std::vector<std::uint64_t> apportionStepSamples(
    const std::uint64_t totalSamples,
    const std::vector<long double>& weights) {
  const auto stepCount = weights.size();
  if (stepCount == 0) {
    return {};
  }
  std::vector<std::uint64_t> base(stepCount, 0);
  std::vector<long double> fractional(stepCount, 0.0L);
  const auto weightSum =
      std::accumulate(weights.begin(), weights.end(), 0.0L);
  std::uint64_t baseSum = 0;
  for (std::size_t index = 0; index < stepCount; ++index) {
    const auto ideal =
        static_cast<long double>(totalSamples) * weights[index] / weightSum;
    base[index] = static_cast<std::uint64_t>(ideal);
    fractional[index] = ideal - static_cast<long double>(base[index]);
    baseSum += base[index];
  }
  const auto remaining = totalSamples - baseSum;
  std::vector<std::size_t> order(stepCount);
  std::iota(order.begin(), order.end(), 0);
  std::sort(
      order.begin(), order.end(),
      [&fractional](const std::size_t a, const std::size_t b) {
        if (fractional[a] != fractional[b]) {
          return fractional[a] > fractional[b];
        }
        return a < b;
      });
  for (std::uint64_t index = 0; index < remaining; ++index) {
    base[order[index]] += 1;
  }
  return base;
}

std::optional<std::vector<long double>> diffuserStepWeights(
    const DiffuserConfig& requested,
    const std::uint32_t stepCount) {
  std::vector<long double> weights;
  if (requested.lengthsMs.has_value()) {
    weights.reserve(requested.lengthsMs->size());
    for (const auto lengthMs : *requested.lengthsMs) {
      weights.push_back(static_cast<long double>(lengthMs));
    }
  } else {
    const auto distribution =
        requested.distribution.value_or(DiffusionDistribution::doubling);
    weights.assign(stepCount, 1.0L);
    if (distribution == DiffusionDistribution::doubling) {
      long double value = 1.0L;
      for (std::uint32_t index = 0; index < stepCount; ++index) {
        if (!std::isfinite(value)) {
          return std::nullopt;
        }
        weights[index] = value;
        value *= 2.0L;
      }
    }
  }
  const auto weightSum =
      std::accumulate(weights.begin(), weights.end(), 0.0L);
  if (!std::isfinite(weightSum) || weightSum <= 0.0L) {
    return std::nullopt;
  }
  return weights;
}

// Hadamard is valid only for a power-of-two Channel count; Householder and
// RandomOrthogonal are valid for any resource-permitted N.
bool isValidChannelCountForMix(
    const dsp::MixMatrixType mix, const std::uint32_t channels) noexcept {
  switch (mix) {
  case dsp::MixMatrixType::hadamard:
    return isPowerOfTwo(channels);
  case dsp::MixMatrixType::householder:
  case dsp::MixMatrixType::randomOrthogonal:
    return channels > 0;
  }
  return false;
}

std::uint64_t boundedRandom(const std::uint64_t seed,
                            const std::uint64_t usage,
                            const std::uint64_t itemIndex,
                            const std::uint64_t valueIndex,
                            const std::uint64_t bound) {
  const auto threshold = (std::uint64_t{0} - bound) % bound;
  for (std::uint64_t drawIndex = 0;; ++drawIndex) {
    const auto value = dsp::positionalSplitMix64V1(
        seed, usage, itemIndex, valueIndex, drawIndex);
    if (value >= threshold) {
      return value % bound;
    }
    if (drawIndex == std::numeric_limits<std::uint64_t>::max()) {
      fail("/seed", "positional random rejection sampling did not converge");
    }
  }
}

// A Fisher-Yates permutation of Channel indices [0, channels), seeded
// positionally by (seed, usage, itemIndex) alone -- shared by the
// Diffuser's own per-step shuffle and by Modulation's channelFraction
// selection below, so the two draws never reimplement the same shape
// differently. `channels` must be > 0.
std::vector<std::uint32_t> seededPermutation(
    const std::uint64_t seed,
    const std::uint64_t usage,
    const std::uint64_t itemIndex,
    const std::uint32_t channels) {
  std::vector<std::uint32_t> permutation(channels);
  std::iota(permutation.begin(), permutation.end(), 0U);
  for (std::uint32_t position = channels - 1U; position > 0; --position) {
    const auto selected = static_cast<std::uint32_t>(boundedRandom(
        seed,
        usage,
        itemIndex,
        position,
        static_cast<std::uint64_t>(position) + 1));
    std::swap(permutation[position], permutation[selected]);
  }
  return permutation;
}

std::vector<bool> resolveModulationChannelMask(
    const std::uint64_t seed,
    const ModulationOwner owner,
    const std::uint64_t itemIndex,
    const std::uint32_t channels,
    const double channelFraction) {
  const auto usage = owner == ModulationOwner::feedbackLoop
      ? kFeedbackLoopModulationChannelSelectionUsage
      : kDiffusionModulationChannelSelectionUsage;
  const auto permutation =
      seededPermutation(seed, usage, itemIndex, channels);
  constexpr double kChannelFractionTolerance = 1e-9;
  const auto rawCount = channelFraction * static_cast<double>(channels);
  const auto modulatedCount = std::min(
      channels,
      std::max(
          std::uint32_t{1},
          static_cast<std::uint32_t>(
              std::ceil(rawCount - kChannelFractionTolerance))));
  std::vector<bool> mask(channels, false);
  for (std::uint32_t index = 0; index < modulatedCount; ++index) {
    mask[permutation[index]] = true;
  }
  return mask;
}

dsp::ResolvedModulation resolveModulation(
    const ModulationConfig& requested,
    const std::uint32_t channels,
    const std::uint32_t sampleRate,
    const std::uint64_t seed,
    const ModulationOwner owner,
    const std::uint64_t itemIndex,
    const std::vector<std::uint64_t>& delaysSamples,
    std::vector<std::uint64_t>& bufferSizes,
    const std::string& rejectionPath) {
  dsp::ResolvedModulation modulation;
  modulation.depthMs = requested.depthMs.value_or(kDefaultModulationDepthMs);
  modulation.rateHz = requested.rateHz.value_or(kDefaultModulationRateHz);
  modulation.shape =
      requested.shape.value_or(dsp::ModulationShape::smoothedRandom);
  modulation.channelFraction = requested.channelFraction.value_or(1.0);
  modulation.interpolation = requested.interpolation.value_or(
      dsp::ModulationInterpolation::lagrange3);
  modulation.interpolationMarginSamples =
      kModulationInterpolationMarginSamples;

  // Only solvable once every Channel's own resolved delay is known;
  // finiteness/range validation of depthMs/rateHz/channelFraction runs
  // regardless, in this stage's own validator (mirroring Damping's
  // pattern).
  if (!(std::isfinite(modulation.depthMs) && modulation.depthMs >= 0.0 &&
        std::isfinite(modulation.rateHz) && modulation.rateHz >= 0.0 &&
        std::isfinite(modulation.channelFraction) &&
        modulation.channelFraction >= 0.0 &&
        modulation.channelFraction <= 1.0)) {
    return modulation;
  }
  modulation.excursionSamples =
      resolveExcursionSamples(modulation.depthMs, sampleRate);

  if (!(modulation.depthMs > 0.0 && modulation.channelFraction > 0.0)) {
    return modulation;
  }

  modulation.channelModulated = resolveModulationChannelMask(
      seed, owner, itemIndex, channels, modulation.channelFraction);

  const auto seedUsage = owner == ModulationOwner::feedbackLoop
      ? kFeedbackLoopModulationSeedUsage
      : kDiffusionModulationSeedUsage;
  modulation.channelSeeds.reserve(channels);
  modulation.channelTargetsPerSample.reserve(channels);
  modulation.channelPhases.reserve(channels);
  for (std::uint32_t channel = 0; channel < channels; ++channel) {
    modulation.channelSeeds.push_back(
        dsp::positionalSplitMix64V1(seed, seedUsage, itemIndex, channel));
    const auto spread =
        resolveModulationRateSpread(seed, owner, itemIndex, channel);
    modulation.channelTargetsPerSample.push_back(
        modulation.rateHz * spread / sampleRate);
    modulation.channelPhases.push_back(
        resolveModulationPhase(seed, owner, itemIndex, channel));
  }

  for (std::uint32_t channel = 0; channel < channels; ++channel) {
    if (!modulation.channelModulated[channel]) {
      continue;
    }
    if (!modulationFitsDelay(
            delaysSamples[channel], modulation.excursionSamples)) {
      fail(
          rejectionPath,
          "Channel " + std::to_string(channel) +
              "'s resolved delay less Excursion does not exceed the "
              "fixed Interpolation margin");
    }
  }

  const auto headroomSamples =
      resolveModulationHeadroomSamples(modulation.excursionSamples);
  for (std::uint32_t channel = 0; channel < channels; ++channel) {
    if (modulation.channelModulated[channel]) {
      bufferSizes[channel] = delaysSamples[channel] + headroomSamples;
    }
  }

  return modulation;
}

std::uint64_t partitionBoundary(const std::uint64_t positions,
                                const std::uint32_t channels,
                                const std::uint32_t index) noexcept {
  const auto quotient = positions / channels;
  const auto remainder = positions % channels;
  return quotient * index +
         (static_cast<std::uint64_t>(index) * remainder) / channels;
}

std::uint64_t evenDelay(const std::uint64_t lastPosition,
                        const std::uint32_t channels,
                        const std::uint32_t channel) noexcept {
  if (channels == 1) {
    return 0;
  }
  const auto intervals = channels - 1U;
  const auto quotient = lastPosition / intervals;
  const auto remainder = lastPosition % intervals;
  return quotient * channel +
         (remainder * channel) / intervals;
}

bool isStereoPreservingSplit(
    const dsp::SplitStrategyType strategy) noexcept {
  return strategy == dsp::SplitStrategyType::stereoHalves ||
         strategy == dsp::SplitStrategyType::stereoInterleave;
}

dsp::ResolvedSplit resolveSplit(const SplitConfig& requested,
                                const std::uint32_t inputChannels) {
  const auto channels = requested.channels.value_or(8);
  const auto strategy =
      requested.strategy.value_or(dsp::SplitStrategyType::duplicate);
  const auto normalisation = requested.normalisation.value_or(
      dsp::EnergyNormalisation::energy);
  const auto stereoPreserving =
      inputChannels == 2 && isStereoPreservingSplit(strategy);
  const auto sourceGain =
      inputChannels == 1
          ? 1.0
          : stereoPreserving
                ? 1.0
                : inputChannels == 2
                      ? 1.0 / std::sqrt(2.0)
                      : 0.0;
  const auto channelGain =
      normalisation == dsp::EnergyNormalisation::energy
          ? channels == 0
                ? 0.0
                : stereoPreserving
                      ? std::sqrt(2.0 / static_cast<double>(channels))
                      : 1.0 / std::sqrt(static_cast<double>(channels))
          : 1.0;
  return {
      inputChannels,
      channels,
      strategy,
      normalisation,
      sourceGain,
      channelGain,
  };
}

std::uint64_t resolveDiffuserModulationReachSamples(
    const std::vector<dsp::ResolvedDiffusionStep>& steps) {
  std::uint64_t reach = 0;
  for (const auto& step : steps) {
    if (step.modulation.has_value() &&
        !step.modulation->channelModulated.empty()) {
      reach +=
          resolveModulationHeadroomSamples(step.modulation->excursionSamples);
    }
  }
  return reach;
}

dsp::ResolvedDiffuser resolveDiffuser(
    const DiffuserConfig& requested,
    const std::uint32_t channels,
    const std::uint32_t sampleRate,
    const std::uint64_t seed,
    const DiffuserDerivation& derivation) {
  const auto stepDefaults = requested.step.value_or(DiffusionStepConfig{});

  dsp::ResolvedDiffuser diffuser;
  diffuser.totalSamples = derivation.sampleBudget.samples;
  if (derivation.expectedStepLengths.size() != derivation.stepCount) {
    return diffuser;
  }

  const auto matrixElements = matrixElementCount(channels);
  std::array<std::optional<std::vector<double>>, static_cast<std::size_t>(dsp::MixMatrixType::randomOrthogonal) + 1> sharedMatrixByType;

  diffuser.steps.reserve(derivation.stepCount);
  for (std::uint32_t index = 0; index < derivation.stepCount; ++index) {
    const DiffusionStepConfig* stepOverride = nullptr;
    if (requested.stepOverrides.has_value()) {
      for (const auto& candidate : *requested.stepOverrides) {
        if (candidate.index == index) {
          stepOverride = &candidate.step;
          break;
        }
      }
    }
    const auto delayStrategy =
        (stepOverride != nullptr && stepOverride->delayStrategy.has_value())
            ? *stepOverride->delayStrategy
            : stepDefaults.delayStrategy.value_or(
                  dsp::DelayStrategy::segmentedRandom);
    const auto mix =
        (stepOverride != nullptr && stepOverride->mix.has_value())
            ? *stepOverride->mix
            : stepDefaults.mix.value_or(dsp::MixMatrixType::hadamard);
    const auto shuffle =
        (stepOverride != nullptr && stepOverride->shuffle.has_value())
            ? *stepOverride->shuffle
            : stepDefaults.shuffle.value_or(true);
    const auto polarity =
        (stepOverride != nullptr && stepOverride->polarity.has_value())
            ? *stepOverride->polarity
            : stepDefaults.polarity.value_or(
                  dsp::PolarityStrategy::seededRandom);
    const auto* const stepModulationRequested =
        (stepOverride != nullptr && stepOverride->modulation.has_value())
            ? &*stepOverride->modulation
            : stepDefaults.modulation.has_value() ? &*stepDefaults.modulation
                                                   : nullptr;

    dsp::ResolvedDiffusionStep step;
    step.index = index;
    step.lengthSamples = derivation.expectedStepLengths[index];
    step.lengthMs =
        sampleRate == 0
            ? 0.0
            : static_cast<double>(step.lengthSamples) * 1000.0 /
                  sampleRate;
    step.delayStrategy = delayStrategy;
    step.shuffle = shuffle;
    step.polarity = polarity;
    step.mix = mix;

    const auto supportedDelayStrategy =
        delayStrategy == dsp::DelayStrategy::segmentedRandom ||
        delayStrategy == dsp::DelayStrategy::uniformRandom ||
        delayStrategy == dsp::DelayStrategy::even;
    const auto supportedPolarity =
        polarity == dsp::PolarityStrategy::seededRandom ||
        polarity == dsp::PolarityStrategy::none;
    const auto canDeriveChannelValues =
        derivation.deriveChannelValues &&
        derivation.sampleBudget.status == SampleBudgetStatus::valid &&
        isValidChannelCountForMix(mix, channels) &&
        supportedDelayStrategy &&
        supportedPolarity &&
        matrixElements.has_value() &&
        (!requiresDistinctChannelPositions(delayStrategy) ||
         hasEnoughPositionsForChannels(step.lengthSamples, channels));
    if (!canDeriveChannelValues) {
      diffuser.steps.push_back(std::move(step));
      continue;
    }

    step.delaysSamples.reserve(channels);
    step.delaysMs.reserve(channels);
    step.bufferSizes.reserve(channels);
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
      std::uint64_t delay = 0;
      if (delayStrategy == dsp::DelayStrategy::segmentedRandom) {
        const auto positions = step.lengthSamples + 1;
        const auto start =
            partitionBoundary(positions, channels, channel);
        const auto end =
            partitionBoundary(positions, channels, channel + 1U);
        delay =
            start + boundedRandom(
                        seed,
                        kDiffusionDelayUsage,
                        step.index,
                        channel,
                        end - start);
      } else if (delayStrategy == dsp::DelayStrategy::even) {
        delay = evenDelay(step.lengthSamples, channels, channel);
      } else {
        const auto positions = step.lengthSamples + 1;
        delay = boundedRandom(
            seed, kDiffusionDelayUsage, step.index, channel, positions);
      }
      step.delaysSamples.push_back(delay);
      step.delaysMs.push_back(
          static_cast<double>(delay) * 1000.0 / sampleRate);
      step.bufferSizes.push_back(delay);
    }

    if (shuffle) {
      step.permutation = seededPermutation(
          seed, kDiffusionShuffleUsage, step.index, channels);
    } else {
      step.permutation.resize(channels);
      std::iota(step.permutation.begin(), step.permutation.end(), 0U);
    }

    step.polaritySigns.reserve(channels);
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
      if (polarity == dsp::PolarityStrategy::none) {
        step.polaritySigns.push_back(1);
      } else {
        const auto value = dsp::positionalSplitMix64V1(
            seed, kDiffusionPolarityUsage, step.index, channel);
        step.polaritySigns.push_back((value & 1U) == 0 ? 1 : -1);
      }
    }
    auto& cachedMatrix = sharedMatrixByType[static_cast<std::size_t>(mix)];
    if (!cachedMatrix.has_value()) {
      switch (mix) {
      case dsp::MixMatrixType::hadamard:
        cachedMatrix = resolveHadamardMatrix(channels);
        break;
      case dsp::MixMatrixType::householder:
        cachedMatrix = resolveHouseholderMatrix(channels);
        break;
      case dsp::MixMatrixType::randomOrthogonal: {
        auto randomOrthogonal = resolveRandomOrthogonalMatrix(
            channels, seed, kMixMatrixRandomOrthogonalUsage);
        if (!randomOrthogonal.has_value()) {
          fail(
              "/composition/stages/1/steps/" + std::to_string(index) +
                  "/mix",
              "RandomOrthogonal construction was singular or "
              "near-singular");
        }
        cachedMatrix = std::move(randomOrthogonal);
        break;
      }
      }
    }
    step.matrix = *cachedMatrix;

    if (stepModulationRequested != nullptr) {
      step.modulation = resolveModulation(
          *stepModulationRequested,
          channels,
          sampleRate,
          seed,
          ModulationOwner::diffusionStep,
          /*itemIndex=*/step.index,
          step.delaysSamples,
          step.bufferSizes,
          "/composition/stages/1/steps/" + std::to_string(index) +
              "/modulation/depthMs");
    }

    diffuser.steps.push_back(std::move(step));
  }
  diffuser.totalSamples += resolveDiffuserModulationReachSamples(diffuser.steps);
  return diffuser;
}

std::uint64_t resolveTailBudgetSamples(
    const double rt60Sec,
    const double decayMargin,
    const std::uint32_t sampleRate) noexcept {
  if (sampleRate == 0 || !(rt60Sec > 0.0) || !std::isfinite(rt60Sec) ||
      !(decayMargin > 0.0) || !std::isfinite(decayMargin)) {
    return 0;
  }
  const auto exactSamples =
      static_cast<long double>(rt60Sec) * decayMargin * sampleRate;
  if (!std::isfinite(exactSamples)) {
    return 0;
  }
  const auto ceiledSamples = std::ceil(exactSamples);
  if (ceiledSamples >= 0x1p64L) {
    return 0;
  }
  return static_cast<std::uint64_t>(ceiledSamples);
}

dsp::ResolvedFeedbackLoop resolveFeedbackLoop(
    const FeedbackLoopConfig& requested,
    const std::uint32_t channels,
    const std::uint32_t sampleRate,
    const std::uint64_t seed,
    const bool deriveChannelValues,
    const std::size_t stageIndex) {
  dsp::ResolvedFeedbackLoop loop;
  loop.channels = channels;
  loop.delayStrategy =
      requested.delayStrategy.value_or(dsp::DelayStrategy::segmentedRandom);
  loop.rt60Sec = requested.rt60Sec.value_or(kDefaultFeedbackLoopRt60Sec);
  loop.decayMargin =
      requested.decayMargin.value_or(kDefaultFeedbackLoopDecayMargin);
  loop.mix = requested.mix.value_or(dsp::MixMatrixType::householder);
  loop.gainMode = requested.gainMode.value_or(dsp::GainMode::perChannel);
  loop.delayMinMs =
      requested.delayMinMs.value_or(kDefaultFeedbackLoopDelayMinMs);
  loop.delayMaxMs =
      requested.delayMaxMs.value_or(kDefaultFeedbackLoopDelayMaxMs);
  // Disabled by default (nullopt); there is no numeric default to fall
  // back to since "disabled" is the Reference configuration itself (#54).
  loop.silenceFloorDb = requested.silenceFloorDb;

  loop.tailBudgetSamples =
      resolveTailBudgetSamples(loop.rt60Sec, loop.decayMargin, sampleRate);

  if (!deriveChannelValues || sampleRate == 0 ||
      !(loop.delayMinMs > 0.0) || !std::isfinite(loop.delayMinMs) ||
      !(loop.delayMaxMs >= loop.delayMinMs) ||
      !std::isfinite(loop.delayMaxMs)) {
    return loop;
  }

  const auto minSamplesExact =
      static_cast<long double>(loop.delayMinMs) * sampleRate / 1000.0L;
  const auto maxSamplesExact =
      static_cast<long double>(loop.delayMaxMs) * sampleRate / 1000.0L;
  if (!std::isfinite(minSamplesExact) || !std::isfinite(maxSamplesExact) ||
      maxSamplesExact >=
          static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
    return loop;
  }
  const auto delayMinSamples =
      static_cast<std::uint64_t>(std::floor(minSamplesExact + 0.5L));
  const auto delayMaxSamples =
      static_cast<std::uint64_t>(std::floor(maxSamplesExact + 0.5L));
  if (delayMaxSamples < delayMinSamples) {
    return loop;
  }
  loop.delayMinSamples = delayMinSamples;
  loop.delayMaxSamples = delayMaxSamples;

  if (!isValidChannelCountForMix(loop.mix, channels)) {
    return loop;
  }

  const auto positions = delayMaxSamples - delayMinSamples + 1;
  const auto matrixElements = matrixElementCount(channels);
  const auto canDeriveChannelValues =
      matrixElements.has_value() &&
      (!requiresDistinctChannelPositions(loop.delayStrategy) ||
       hasEnoughPositionsForChannels(positions - 1, channels));
  if (!canDeriveChannelValues) {
    return loop;
  }

  loop.delaysSamples.reserve(channels);
  loop.delaysMs.reserve(channels);
  loop.bufferSizes.reserve(channels);
  for (std::uint32_t channel = 0; channel < channels; ++channel) {
    std::uint64_t offset = 0;
    if (loop.delayStrategy == dsp::DelayStrategy::segmentedRandom) {
      const auto start = partitionBoundary(positions, channels, channel);
      const auto end =
          partitionBoundary(positions, channels, channel + 1U);
      offset =
          start + boundedRandom(
                      seed,
                      kFeedbackLoopDelayUsage,
                      0,
                      channel,
                      end - start);
    } else if (loop.delayStrategy == dsp::DelayStrategy::even) {
      offset = evenDelay(positions - 1, channels, channel);
    } else {
      offset = boundedRandom(
          seed, kFeedbackLoopDelayUsage, 0, channel, positions);
    }
    const auto delay = delayMinSamples + offset;
    loop.delaysSamples.push_back(delay);
    loop.delaysMs.push_back(
        static_cast<double>(delay) * 1000.0 / sampleRate);
    loop.bufferSizes.push_back(delay);
  }
  loop.blockSizeBoundSamples = *std::min_element(
      loop.delaysSamples.begin(), loop.delaysSamples.end());

  loop.gains.reserve(channels);
  if (loop.gainMode == dsp::GainMode::uniform) {
    double meanLoopTimeSec = 0.0;
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
      meanLoopTimeSec +=
          static_cast<double>(loop.delaysSamples[channel]) / sampleRate;
    }
    meanLoopTimeSec /= channels;
    const auto sharedGain =
        std::pow(10.0, -3.0 * meanLoopTimeSec / loop.rt60Sec);
    loop.gains.assign(channels, sharedGain);
  } else {
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
      const auto loopTimeSec =
          static_cast<double>(loop.delaysSamples[channel]) / sampleRate;
      loop.gains.push_back(std::pow(10.0, -3.0 * loopTimeSec / loop.rt60Sec));
    }
  }

  switch (loop.mix) {
  case dsp::MixMatrixType::hadamard:
    loop.matrix = resolveHadamardMatrix(channels);
    break;
  case dsp::MixMatrixType::householder:
    loop.matrix = resolveHouseholderMatrix(channels);
    break;
  case dsp::MixMatrixType::randomOrthogonal: {
    auto randomOrthogonal = resolveRandomOrthogonalMatrix(
        channels, seed, kMixMatrixRandomOrthogonalUsage);
    if (!randomOrthogonal.has_value()) {
      fail(
          stagePath(stageIndex) + "/mix",
          "RandomOrthogonal construction was singular or near-singular");
    }
    loop.matrix = std::move(*randomOrthogonal);
    break;
  }
  }

  if (requested.damping.has_value()) {
    dsp::ResolvedDamping damping;
    damping.highRatio =
        requested.damping->highRatio.value_or(kDefaultDampingHighRatio);
    damping.highHz =
        requested.damping->highHz.value_or(kDefaultDampingHighHz);
    damping.lowRatio =
        requested.damping->lowRatio.value_or(kDefaultDampingLowRatio);
    damping.lowHz = requested.damping->lowHz.value_or(kDefaultDampingLowHz);

    if (std::isfinite(damping.highRatio) && damping.highRatio > 0.0 &&
        std::isfinite(damping.highHz) && damping.highHz > 0.0 &&
        damping.highHz < sampleRate / 2.0 &&
        std::isfinite(damping.lowRatio) && damping.lowRatio > 0.0 &&
        std::isfinite(damping.lowHz) && damping.lowHz > 0.0 &&
        damping.lowHz < sampleRate / 2.0) {
      damping.highShelfGains.reserve(channels);
      damping.highShelfB0.reserve(channels);
      damping.highShelfB1.reserve(channels);
      damping.highShelfA1.reserve(channels);
      damping.lowShelfGains.reserve(channels);
      damping.lowShelfB0.reserve(channels);
      damping.lowShelfB1.reserve(channels);
      damping.lowShelfA1.reserve(channels);
      damping.expectedLowRt60Sec.reserve(channels);
      damping.expectedReferenceRt60Sec.reserve(channels);
      damping.expectedHighRt60Sec.reserve(channels);
      damping.contractionBoundFloat32.reserve(channels);
      damping.contractionMarginFloat32.reserve(channels);
      damping.contractionBoundFloat64.reserve(channels);
      damping.contractionMarginFloat64.reserve(channels);
      // Computed once, shared by every Channel's certificate (see
      // resolveMatrixContractionBound): the mixing matrix is common to the
      // whole Feedback Loop, not per-Channel.
      const auto matrixBoundFloat32 = resolveMatrixContractionBound(
          channels, std::numeric_limits<float>::epsilon());
      const auto matrixBoundFloat64 = resolveMatrixContractionBound(
          channels, std::numeric_limits<double>::epsilon());
      auto slowestResolvedRt60Sec = loop.rt60Sec;
      for (std::uint32_t channel = 0; channel < channels; ++channel) {
        const auto channelGain = loop.gains[channel];
        const auto lossDb = 20.0 * std::log10(channelGain);
        const auto loopTimeSec =
            static_cast<double>(loop.delaysSamples[channel]) / sampleRate;

        const auto highGain = resolveShelfGain(channelGain, damping.highRatio);
        damping.highShelfGains.push_back(highGain);
        const auto highCoefficients = resolveHighShelfCoefficients(
            highGain, damping.highHz, sampleRate);
        damping.highShelfB0.push_back(highCoefficients.b0);
        damping.highShelfB1.push_back(highCoefficients.b1);
        damping.highShelfA1.push_back(highCoefficients.a1);

        const auto lowGain = resolveShelfGain(channelGain, damping.lowRatio);
        damping.lowShelfGains.push_back(lowGain);
        const auto lowCoefficients = resolveLowShelfCoefficients(
            lowGain, damping.lowHz, sampleRate);
        damping.lowShelfB0.push_back(lowCoefficients.b0);
        damping.lowShelfB1.push_back(lowCoefficients.b1);
        damping.lowShelfA1.push_back(lowCoefficients.a1);

        const auto impliedUndampedRt60Sec = -60.0 * loopTimeSec / lossDb;
        damping.expectedLowRt60Sec.push_back(
            damping.lowRatio * impliedUndampedRt60Sec);
        damping.expectedHighRt60Sec.push_back(
            damping.highRatio * impliedUndampedRt60Sec);

        // The Reference band uses both shelves' actual combined response
        // at 1 kHz, since a nearby corner can shift it away from unity.
        const auto referenceMagnitude =
            shelfMagnitudeAtFrequency(lowCoefficients, 1000.0, sampleRate) *
            shelfMagnitudeAtFrequency(highCoefficients, 1000.0, sampleRate);
        const auto referenceLossDb =
            lossDb + 20.0 * std::log10(referenceMagnitude);
        damping.expectedReferenceRt60Sec.push_back(
            -60.0 * loopTimeSec / referenceLossDb);

        const auto boundFloat32 = resolveChannelContractionBound(
            channelGain, lowGain, highGain, matrixBoundFloat32);
        const auto boundFloat64 = resolveChannelContractionBound(
            channelGain, lowGain, highGain, matrixBoundFloat64);
        damping.contractionBoundFloat32.push_back(boundFloat32);
        damping.contractionMarginFloat32.push_back(1.0 - boundFloat32);
        damping.contractionBoundFloat64.push_back(boundFloat64);
        damping.contractionMarginFloat64.push_back(1.0 - boundFloat64);

        if (damping.highRatio > 1.0) {
          slowestResolvedRt60Sec = std::max(
              slowestResolvedRt60Sec, damping.expectedHighRt60Sec.back());
          slowestResolvedRt60Sec = std::max(
              slowestResolvedRt60Sec,
              resolveShelfSettlingTimeSec(highCoefficients.a1, sampleRate));
        }
        if (damping.lowRatio > 1.0) {
          slowestResolvedRt60Sec = std::max(
              slowestResolvedRt60Sec, damping.expectedLowRt60Sec.back());
          slowestResolvedRt60Sec = std::max(
              slowestResolvedRt60Sec,
              resolveShelfSettlingTimeSec(lowCoefficients.a1, sampleRate));
        }
      }
      damping.slowestResolvedRt60Sec = slowestResolvedRt60Sec;
      loop.tailBudgetSamples = resolveTailBudgetSamples(
          slowestResolvedRt60Sec, loop.decayMargin, sampleRate);
    }
    loop.damping = std::move(damping);
  }

  if (requested.modulation.has_value()) {
    loop.modulation = resolveModulation(
        *requested.modulation,
        channels,
        sampleRate,
        seed,
        ModulationOwner::feedbackLoop,
        /*itemIndex=*/0,
        loop.delaysSamples,
        loop.bufferSizes,
        stagePath(stageIndex) + "/modulation/depthMs");

    if (!loop.modulation->channelModulated.empty()) {
      loop.blockSizeBoundSamples = resolveModulationBlockSizeBoundSamples(
          loop.delaysSamples,
          loop.modulation->channelModulated,
          loop.modulation->excursionSamples);
    }
  }

  return loop;
}

const char* downmixStrategyLabel(const dsp::DownmixStrategy strategy) {
  switch (strategy) {
  case dsp::DownmixStrategy::select:
    return "select";
  case dsp::DownmixStrategy::orthogonalRows:
    return "orthogonal-rows";
  case dsp::DownmixStrategy::halves:
    return "halves";
  case dsp::DownmixStrategy::alternating:
    return "alternating";
  case dsp::DownmixStrategy::sumAll:
    return "sum-all";
  }
  fail("/composition", "unsupported Downmix strategy");
}

std::vector<double> selectRow(
    const std::uint32_t channel, const std::uint32_t channels) {
  std::vector<double> row(channels, 0.0);
  if (channel < channels) {
    row[channel] = 1.0;
  }
  return row;
}

std::vector<double> halvesRow(
    const std::uint32_t channels, const bool leftGroup) {
  std::vector<double> row(channels, 0.0);
  if (channels < 2) {
    return row;
  }
  const std::uint32_t leftCount = (channels + 1U) / 2U;
  const std::uint32_t begin = leftGroup ? 0U : leftCount;
  const std::uint32_t end = leftGroup ? leftCount : channels;
  const auto coefficient =
      1.0 / std::sqrt(static_cast<double>(end - begin));
  for (std::uint32_t index = begin; index < end; ++index) {
    row[index] = coefficient;
  }
  return row;
}

std::vector<double> alternatingRow(
    const std::uint32_t channels, const bool leftGroup) {
  std::vector<double> row(channels, 0.0);
  if (channels < 2) {
    return row;
  }
  const std::uint32_t first = leftGroup ? 0U : 1U;
  std::uint32_t groupSize = 0;
  for (std::uint32_t index = first; index < channels; index += 2U) {
    ++groupSize;
  }
  const auto coefficient = 1.0 / std::sqrt(static_cast<double>(groupSize));
  for (std::uint32_t index = first; index < channels; index += 2U) {
    row[index] = coefficient;
  }
  return row;
}

std::vector<double> sumAllRow(const std::uint32_t channels) {
  std::vector<double> row(channels, 0.0);
  if (channels == 0) {
    return row;
  }
  const auto coefficient = 1.0 / std::sqrt(static_cast<double>(channels));
  std::fill(row.begin(), row.end(), coefficient);
  return row;
}

bool resolveCoherentDownmixAblation(
    const dsp::DownmixStrategy strategy,
    const dsp::DownmixAlignment alignment) noexcept {
  return strategy == dsp::DownmixStrategy::sumAll &&
      alignment == dsp::DownmixAlignment::aligned;
}

std::vector<double> scaledRow(
    const std::vector<double>& row, const double compensation) {
  std::vector<double> scaled(row.size());
  for (std::size_t index = 0; index < row.size(); ++index) {
    scaled[index] = row[index] * compensation;
  }
  return scaled;
}

struct DownmixRowPair {
  std::vector<double> left;
  std::vector<double> right;
};

DownmixRowPair extractDownmixRows(
    const std::vector<double>& matrix, const std::uint32_t channels) {
  return {
      std::vector<double>(matrix.begin(), matrix.begin() + channels),
      std::vector<double>(
          matrix.begin() + channels, matrix.begin() + 2 * channels),
  };
}

std::optional<std::uint64_t> estimateDownmixOrthogonalMatrixBytes(
    const std::uint32_t channels) noexcept {
  constexpr std::uint64_t kSampleBytes = 8;
  const auto matrixElements = checkedMul(channels, channels);
  return matrixElements ? checkedMul(*matrixElements, kSampleBytes)
                        : std::nullopt;
}

std::vector<double> resolveWidthMatrix(const double widthDeg) {
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
  const auto halfAngleRad = widthDeg * (dsp::kPi / 180.0) / 2.0;
  const auto cosHalf = std::cos(halfAngleRad);
  const auto sinHalf = std::sin(halfAngleRad);
  const auto a = (cosHalf + sinHalf) * half;
  const auto b = (cosHalf - sinHalf) * half;
  return {a, b, b, a};
}

void checkDownmixMemoryBudget(
    const std::uint32_t channels,
    const std::uint64_t budgetBytes,
    const std::string_view path) {
  const auto estimate = estimateDownmixOrthogonalMatrixBytes(channels);
  if (!estimate.has_value() || *estimate > budgetBytes) {
    fail(
        path,
        "resolved Downmix DSP memory footprint exceeds the configured "
        "memory budget");
  }
}

dsp::ResolvedDownmix resolveDownmix(
    const DownmixConfig& requested,
    const std::uint32_t channels,
    const std::uint64_t seed,
    const std::uint64_t orthogonalUsage,
    const dsp::DownmixAlignment alignment,
    const std::string& path,
    const std::uint64_t memoryBudgetBytes) {
  const auto strategy =
      requested.strategy.value_or(dsp::DownmixStrategy::select);
  const auto normalisation = requested.normalisation.value_or(
      dsp::EnergyNormalisation::energy);
  const auto compensation =
      normalisation == dsp::EnergyNormalisation::none
          ? 1.0
          : channels == 0
                ? 0.0
                : channels == 1
                ? 1.0 / std::sqrt(2.0)
                : std::sqrt(static_cast<double>(channels) / 2.0);

  std::optional<std::uint32_t> leftChannel;
  std::optional<std::uint32_t> rightChannel;
  std::vector<double> leftRow;
  std::vector<double> rightRow;

  if (strategy == dsp::DownmixStrategy::select) {
    if (!requested.leftChannel.has_value()) {
      fail(path + "/leftChannel", "required field is missing");
    }
    leftChannel = requested.leftChannel;
    rightChannel = requested.rightChannel;
    leftRow = selectRow(*leftChannel, channels);
    rightRow = rightChannel.has_value()
        ? selectRow(*rightChannel, channels)
        : leftRow;
  } else {
    if (requested.leftChannel.has_value()) {
      fail(
          path + "/leftChannel",
          std::string("not applicable to strategy ") +
              downmixStrategyLabel(strategy));
    }
    if (requested.rightChannel.has_value()) {
      fail(
          path + "/rightChannel",
          std::string("not applicable to strategy ") +
              downmixStrategyLabel(strategy));
    }
    if (strategy == dsp::DownmixStrategy::sumAll) {
      leftRow = sumAllRow(channels);
      rightRow = leftRow;
    } else if (channels < 2) {
      leftRow.assign(channels, 0.0);
      rightRow.assign(channels, 0.0);
    } else if (strategy == dsp::DownmixStrategy::halves) {
      leftRow = halvesRow(channels, /*leftGroup=*/true);
      rightRow = halvesRow(channels, /*leftGroup=*/false);
    } else if (strategy == dsp::DownmixStrategy::alternating) {
      leftRow = alternatingRow(channels, /*leftGroup=*/true);
      rightRow = alternatingRow(channels, /*leftGroup=*/false);
    } else {
      checkDownmixMemoryBudget(
          channels, memoryBudgetBytes, path + "/strategy");
      auto matrix = resolveRandomOrthogonalMatrix(
          channels, seed, orthogonalUsage);
      if (!matrix.has_value()) {
        fail(
            path + "/strategy",
            "RandomOrthogonal construction was singular or near-singular");
      }
      auto rows = extractDownmixRows(*matrix, channels);
      leftRow = std::move(rows.left);
      rightRow = std::move(rows.right);
    }
  }

  auto effectiveLeftRow = scaledRow(leftRow, compensation);
  auto effectiveRightRow = scaledRow(rightRow, compensation);
  const auto widthDeg = requested.widthDeg.value_or(90.0);
  auto widthMatrix = resolveWidthMatrix(widthDeg);
  return {
      channels,
      2,
      strategy,
      leftChannel,
      rightChannel,
      normalisation,
      compensation,
      std::move(leftRow),
      std::move(rightRow),
      std::move(effectiveLeftRow),
      std::move(effectiveRightRow),
      alignment,
      widthDeg,
      std::move(widthMatrix),
      resolveCoherentDownmixAblation(strategy, alignment),
  };
}

double resolveLinearGainFromDb(const double levelDb) noexcept {
  return std::pow(10.0, levelDb / 20.0);
}

void validateFloatRepresentableGain(
    const std::string_view path, const double gain) {
  const auto gainAtFloatPrecision = static_cast<float>(gain);
  if (!(gain > 0.0) || !std::isfinite(gain) ||
      !std::isfinite(gainAtFloatPrecision) ||
      !(gainAtFloatPrecision > 0.0f)) {
    fail(
        path,
        "expected finite positive gain representable at float "
        "precision");
  }
}

std::uint64_t resolvePreDelaySamples(
    const double preDelayMs, const std::uint32_t sampleRate) noexcept {
  if (!std::isfinite(preDelayMs) || preDelayMs < 0.0) {
    return 0;
  }
  const auto exactSamples =
      static_cast<long double>(preDelayMs) * sampleRate / 1000.0L;
  if (!std::isfinite(exactSamples) ||
      exactSamples >=
          static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
    return 0;
  }
  return static_cast<std::uint64_t>(std::floor(exactSamples + 0.5L));
}

DownmixConfig withEarlyDownmixDefaults(
    DownmixConfig requested, const std::uint32_t channels) {
  const auto strategy =
      requested.strategy.value_or(dsp::DownmixStrategy::select);
  requested.strategy = strategy;
  if (strategy == dsp::DownmixStrategy::select &&
      !requested.leftChannel.has_value()) {
    requested.leftChannel = 0;
    if (!requested.rightChannel.has_value() && channels > 1) {
      requested.rightChannel = 1;
    }
  }
  return requested;
}

struct TapSupportBounds {
  std::uint64_t nominalSupportMinSamples = 0;
  std::uint64_t nominalSupportMaxSamples = 0;
  std::uint64_t conservativeSupportMinSamples = 0;
  std::uint64_t conservativeSupportMaxSamples = 0;
};

TapSupportBounds resolveTapSupport(
    const dsp::ResolvedDiffuser& diffuser, const std::uint32_t stepIndex) {
  TapSupportBounds bounds;
  for (std::size_t index = 0;
       index <= stepIndex && index < diffuser.steps.size();
       ++index) {
    const auto& step = diffuser.steps[index];
    if (step.delaysSamples.empty()) {
      continue;
    }
    const auto minMax = std::minmax_element(
        step.delaysSamples.begin(), step.delaysSamples.end());
    const auto stepMinSamples = *minMax.first;
    const auto stepMaxSamples = *minMax.second;
    bounds.nominalSupportMinSamples += stepMinSamples;
    bounds.nominalSupportMaxSamples += stepMaxSamples;
    std::uint64_t reachSamples = 0;
    if (step.modulation.has_value() &&
        !step.modulation->channelModulated.empty()) {
      reachSamples =
          resolveModulationHeadroomSamples(step.modulation->excursionSamples);
    }
    bounds.conservativeSupportMinSamples +=
        stepMinSamples > reachSamples ? stepMinSamples - reachSamples : 0;
    bounds.conservativeSupportMaxSamples += stepMaxSamples + reachSamples;
  }
  return bounds;
}

dsp::ResolvedEarlyTap resolveEarlyTap(
    const std::uint32_t stepIndex,
    const double gainDb,
    const dsp::ResolvedDiffuser& diffuser,
    const double decayDbPerSec,
    const std::uint32_t sampleRate) {
  dsp::ResolvedEarlyTap tap;
  tap.stepIndex = stepIndex;
  tap.gainDb = gainDb;

  const auto bounds = resolveTapSupport(diffuser, tap.stepIndex);
  tap.nominalSupportMinSamples = bounds.nominalSupportMinSamples;
  tap.nominalSupportMaxSamples = bounds.nominalSupportMaxSamples;
  tap.conservativeSupportMinSamples = bounds.conservativeSupportMinSamples;
  tap.conservativeSupportMaxSamples = bounds.conservativeSupportMaxSamples;
  if (sampleRate != 0) {
    tap.nominalSupportMinMs = static_cast<double>(
                                   bounds.nominalSupportMinSamples) *
        1000.0 / sampleRate;
    tap.nominalSupportMaxMs = static_cast<double>(
                                   bounds.nominalSupportMaxSamples) *
        1000.0 / sampleRate;
    tap.conservativeSupportMinMs =
        static_cast<double>(bounds.conservativeSupportMinSamples) * 1000.0 /
        sampleRate;
    tap.conservativeSupportMaxMs =
        static_cast<double>(bounds.conservativeSupportMaxSamples) * 1000.0 /
        sampleRate;
  }

  const auto effectiveDecayDbPerSec =
      (std::isfinite(decayDbPerSec) && decayDbPerSec >= 0.0) ? decayDbPerSec
                                                               : 0.0;
  tap.shapingGainDb = tap.gainDb -
      effectiveDecayDbPerSec * (tap.nominalSupportMaxMs / 1000.0);
  tap.gain = resolveLinearGainFromDb(tap.shapingGainDb);
  return tap;
}

void validateShape(const dsp::ResolvedComposition& composition) {
  if (composition.stages.empty()) {
    return;
  }
  const auto stageCount = composition.stages.size();
  const auto threeStageShape =
      stageCount == 3 &&
      (std::holds_alternative<dsp::ResolvedDiffuser>(
           composition.stages[1]) ||
       std::holds_alternative<dsp::ResolvedFeedbackLoop>(
           composition.stages[1]));
  // The Feedback Loop sits in series after the Diffuser and never inside
  // it, so the four-stage shape only ever admits this one middle order.
  const auto fourStageShape =
      stageCount == 4 &&
      std::holds_alternative<dsp::ResolvedDiffuser>(
          composition.stages[1]) &&
      std::holds_alternative<dsp::ResolvedFeedbackLoop>(
          composition.stages[2]);
  if ((!threeStageShape && !fourStageShape) ||
      !std::holds_alternative<dsp::ResolvedSplit>(composition.stages.front()) ||
      !std::holds_alternative<dsp::ResolvedDownmix>(
          composition.stages.back())) {
    fail(
        "/composition/stages",
        "expected [split, diffuser, downmix], "
        "[split, feedback-loop, downmix], or "
        "[split, diffuser, feedback-loop, downmix]");
  }
}

void validateResolvedConfig(
    const dsp::ResolvedConfig& resolved,
    const ResolutionEvidence* resolutionEvidence,
    std::uint64_t memoryBudgetBytes);

} // namespace

dsp::ResolvedConfig resolveConfig(const ReverbConfig& requested,
                                  const std::uint32_t sampleRate,
                                  const std::uint32_t inputChannels,
                                  const std::uint64_t memoryBudgetBytes) {
  dsp::ResolvedConfig resolved{
      requested.formatVersion.value_or(kReverbConfigFormatVersion),
      requested.seed.value_or(0),
      sampleRate,
      {},
  };

  const auto* requestedComposition =
      requested.composition.has_value() ? &*requested.composition : nullptr;
  ResolutionEvidence resolutionEvidence;
  if (requestedComposition != nullptr &&
      requestedComposition->stages.empty()) {
    if (requestedComposition->mainEnabled.has_value()) {
      fail(
          "/composition/mainEnabled",
          "not applicable to the empty identity Composition");
    }
    if (requestedComposition->mainLevelDb.has_value()) {
      fail(
          "/composition/mainLevelDb",
          "not applicable to the empty identity Composition");
    }
    if (requestedComposition->early.has_value()) {
      fail(
          "/composition/early",
          "not applicable to the empty identity Composition");
    }
    if (requestedComposition->dryDb.has_value()) {
      fail(
          "/composition/dryDb",
          "not applicable to the empty identity Composition");
    }
    if (requestedComposition->wetDb.has_value()) {
      fail(
          "/composition/wetDb",
          "not applicable to the empty identity Composition");
    }
    if (requestedComposition->wetOnly.has_value()) {
      fail(
          "/composition/wetOnly",
          "not applicable to the empty identity Composition");
    }
    if (requestedComposition->preDelayMs.has_value()) {
      fail(
          "/composition/preDelayMs",
          "not applicable to the empty identity Composition");
    }
  }
  if (requestedComposition != nullptr &&
      !requestedComposition->stages.empty()) {
    resolved.composition.mainEnabled =
        requestedComposition->mainEnabled.value_or(true);
    resolved.composition.mainLevelDb =
        requestedComposition->mainLevelDb.value_or(0.0);
    resolved.composition.mainGain =
        resolveLinearGainFromDb(resolved.composition.mainLevelDb);
    resolved.composition.dryDb = requestedComposition->dryDb.value_or(0.0);
    resolved.composition.dryGain =
        resolveLinearGainFromDb(resolved.composition.dryDb);
    resolved.composition.wetDb = requestedComposition->wetDb.value_or(0.0);
    resolved.composition.wetGain =
        resolveLinearGainFromDb(resolved.composition.wetDb);
    resolved.composition.wetOnly =
        requestedComposition->wetOnly.value_or(true);
    resolved.composition.preDelayMs =
        requestedComposition->preDelayMs.value_or(0.0);
    resolved.composition.preDelaySamples = resolvePreDelaySamples(
        resolved.composition.preDelayMs, sampleRate);
    const auto requestedStageCount = requestedComposition->stages.size();
    const auto canonicalShape =
        std::holds_alternative<SplitConfig>(
            requestedComposition->stages.front()) &&
        std::holds_alternative<DownmixConfig>(
            requestedComposition->stages.back()) &&
        ((requestedStageCount == 3 &&
          (std::holds_alternative<DiffuserConfig>(
               requestedComposition->stages[1]) ||
           std::holds_alternative<FeedbackLoopConfig>(
               requestedComposition->stages[1]))) ||
         (requestedStageCount == 4 &&
          std::holds_alternative<DiffuserConfig>(
              requestedComposition->stages[1]) &&
          std::holds_alternative<FeedbackLoopConfig>(
              requestedComposition->stages[2])));
    const auto deriveChannelValues =
        resolved.formatVersion == kReverbConfigFormatVersion &&
        sampleRate != 0 &&
        inputChannels > 0 &&
        inputChannels <= 2 &&
        canonicalShape;
    const auto hasFeedbackLoop = std::any_of(
        requestedComposition->stages.begin(),
        requestedComposition->stages.end(),
        [](const StageConfig& stage) {
          return std::holds_alternative<FeedbackLoopConfig>(stage);
        });
    const auto mainAlignment = hasFeedbackLoop
        ? dsp::DownmixAlignment::unaligned
        : dsp::DownmixAlignment::aligned;
    std::uint32_t channels = 0;
    for (std::size_t stageIndex = 0; stageIndex < requestedStageCount;
         ++stageIndex) {
      const auto& stage = requestedComposition->stages[stageIndex];
      std::visit(
          [&](const auto& stageConfig) {
            using Stage = std::decay_t<decltype(stageConfig)>;
            if constexpr (std::is_same_v<Stage, SplitConfig>) {
              auto split = resolveSplit(stageConfig, inputChannels);
              channels = split.channels;
              resolved.composition.stages.emplace_back(
                  std::move(split));
            } else if constexpr (
                std::is_same_v<Stage, DiffuserConfig>) {
              DiffuserDerivation derivation;
              if (stageConfig.lengthsMs.has_value()) {
                derivation.stepCount = static_cast<std::uint32_t>(
                    stageConfig.lengthsMs->size());
                derivation.totalMs = std::accumulate(
                    stageConfig.lengthsMs->begin(),
                    stageConfig.lengthsMs->end(),
                    0.0);
              } else {
                derivation.stepCount =
                    stageConfig.steps.value_or(kDefaultDiffuserStepCount);
                derivation.totalMs = stageConfig.totalMs.value_or(
                    kDefaultDiffuserTotalMs);
              }
              derivation.sampleBudget =
                  deriveSampleBudget(derivation.totalMs, sampleRate);
              derivation.deriveChannelValues = deriveChannelValues;
              if (derivation.deriveChannelValues &&
                  derivation.stepCount > 0 &&
                  derivation.sampleBudget.status ==
                      SampleBudgetStatus::valid) {
                const auto weights = diffuserStepWeights(
                    stageConfig, derivation.stepCount);
                if (weights.has_value() &&
                    weights->size() == derivation.stepCount) {
                  checkDiffuserMemoryBudget(
                      channels,
                      derivation.sampleBudget.samples,
                      derivation.stepCount,
                      /*modulationBytes=*/0,
                      memoryBudgetBytes,
                      "/composition/stages/1");
                  derivation.expectedStepLengths = apportionStepSamples(
                      derivation.sampleBudget.samples, *weights);
                }
              }
              resolutionEvidence.diffuser = derivation;
              resolved.composition.stages.emplace_back(
                  resolveDiffuser(
                      stageConfig,
                      channels,
                      sampleRate,
                      resolved.seed,
                      derivation));
            } else if constexpr (
                std::is_same_v<Stage, FeedbackLoopConfig>) {
              resolved.composition.stages.emplace_back(
                  resolveFeedbackLoop(
                      stageConfig,
                      channels,
                      sampleRate,
                      resolved.seed,
                      deriveChannelValues,
                      stageIndex));
            } else {
              resolved.composition.stages.emplace_back(
                  resolveDownmix(
                      stageConfig,
                      channels,
                      resolved.seed,
                      kMainDownmixRandomOrthogonalUsage,
                      mainAlignment,
                      stagePath(stageIndex),
                      memoryBudgetBytes));
            }
          },
          stage);
    }

    if (requestedComposition->early.has_value()) {
      const auto& earlyRequested = *requestedComposition->early;
      const auto hasTaps =
          earlyRequested.taps.has_value() && !earlyRequested.taps->empty();
      if (!hasTaps) {
        if (earlyRequested.enabled.has_value() ||
            earlyRequested.levelDb.has_value() ||
            earlyRequested.decayDbPerSec.has_value() ||
            earlyRequested.downmix.has_value()) {
          fail(
              "/composition/early",
              "enabled/levelDb/decayDbPerSec/downmix are not applicable "
              "without at least one tap");
        }
      } else {
        dsp::ResolvedEarlyReflections early;
        early.enabled = earlyRequested.enabled.value_or(true);
        early.levelDb = earlyRequested.levelDb.value_or(0.0);
        early.gain = resolveLinearGainFromDb(early.levelDb);
        early.decayDbPerSec = earlyRequested.decayDbPerSec.value_or(0.0);

        auto sortedTaps = *earlyRequested.taps;
        std::sort(
            sortedTaps.begin(),
            sortedTaps.end(),
            [](const EarlyTapConfig& a, const EarlyTapConfig& b) {
              return a.stepIndex < b.stepIndex;
            });
        for (std::size_t index = 1; index < sortedTaps.size(); ++index) {
          if (sortedTaps[index].stepIndex ==
              sortedTaps[index - 1].stepIndex) {
            fail(
                "/composition/early/taps",
                "expected unique Diffusion Step indices");
          }
        }

        const auto* const diffuserStage =
            dsp::findResolvedDiffuser(resolved.composition);
        const dsp::ResolvedDiffuser noDiffuser;
        const auto& diffuserForSupport =
            diffuserStage != nullptr ? *diffuserStage : noDiffuser;
        early.taps.reserve(sortedTaps.size());
        for (const auto& tapRequested : sortedTaps) {
          early.taps.push_back(
              resolveEarlyTap(
                  tapRequested.stepIndex,
                  tapRequested.gainDb.value_or(0.0),
                  diffuserForSupport,
                  early.decayDbPerSec,
                  sampleRate));
        }

        const auto downmixRequested = withEarlyDownmixDefaults(
            earlyRequested.downmix.value_or(DownmixConfig{}), channels);
        early.downmix = resolveDownmix(
            downmixRequested,
            channels,
            resolved.seed,
            kEarlyDownmixRandomOrthogonalUsage,
            dsp::DownmixAlignment::aligned,
            "/composition/early/downmix",
            memoryBudgetBytes);
        resolved.composition.early = std::move(early);
      }
    }
  }

  validateResolvedConfig(resolved, &resolutionEvidence, memoryBudgetBytes);
  return resolved;
}

void validateResolvedConfig(const dsp::ResolvedConfig& resolved,
                            const std::uint64_t memoryBudgetBytes) {
  validateResolvedConfig(resolved, nullptr, memoryBudgetBytes);
}

namespace {

void validateResolvedMixMatrix(
    const dsp::MixMatrixType mix,
    const std::uint32_t channels,
    const std::vector<double>& matrix,
    const std::string_view path) {
  switch (mix) {
  case dsp::MixMatrixType::hadamard: {
    const auto canonical = resolveHadamardMatrix(channels);
    const auto tolerance =
        4.0 * std::numeric_limits<double>::epsilon() * canonical.front();
    for (std::size_t element = 0; element < canonical.size(); ++element) {
      const auto actual = matrix[element];
      if (!std::isfinite(actual) ||
          std::abs(actual - canonical[element]) > tolerance) {
        fail(
            path,
            "expected the normalized canonical Sylvester-Hadamard "
            "matrix");
      }
    }
    break;
  }
  case dsp::MixMatrixType::householder: {
    const auto canonical = resolveHouseholderMatrix(channels);
    const auto tolerance =
        4.0 * std::numeric_limits<double>::epsilon() *
        std::max(1.0, std::abs(canonical.front()));
    for (std::size_t element = 0; element < canonical.size(); ++element) {
      const auto actual = matrix[element];
      if (!std::isfinite(actual) ||
          std::abs(actual - canonical[element]) > tolerance) {
        fail(
            path,
            "expected the normalized canonical Householder matrix");
      }
    }
    break;
  }
  case dsp::MixMatrixType::randomOrthogonal: {
    constexpr double kOrthogonalityTolerance = 1e-9;
    for (std::uint32_t rowA = 0; rowA < channels; ++rowA) {
      for (std::uint32_t rowB = 0; rowB < channels; ++rowB) {
        double dot = 0.0;
        for (std::uint32_t column = 0; column < channels; ++column) {
          dot +=
              matrix[static_cast<std::size_t>(rowA) * channels + column] *
              matrix[static_cast<std::size_t>(rowB) * channels + column];
        }
        const auto expected = rowA == rowB ? 1.0 : 0.0;
        if (!std::isfinite(dot) ||
            std::abs(dot - expected) > kOrthogonalityTolerance) {
          fail(path, "expected an orthogonal resolved matrix (M M^T = I)");
        }
      }
    }
    break;
  }
  }
}

void validateResolvedModulation(
    const dsp::ResolvedConfig& resolved,
    const dsp::ResolvedModulation& modulation,
    const std::uint32_t channels,
    const ModulationOwner owner,
    const std::uint64_t itemIndex,
    const std::vector<std::uint64_t>& delaysSamples,
    const std::vector<std::uint64_t>& bufferSizes,
    const std::string& modulationPath,
    const std::string& bufferSizesPath) {
  if (!std::isfinite(modulation.depthMs) || !(modulation.depthMs >= 0.0)) {
    fail(
        modulationPath + "/depthMs",
        "expected finite value at least zero");
  }
  if (!std::isfinite(modulation.rateHz) || !(modulation.rateHz >= 0.0)) {
    fail(
        modulationPath + "/rateHz",
        "expected finite value at least zero");
  }
  if (modulation.shape != dsp::ModulationShape::smoothedRandom &&
      modulation.shape != dsp::ModulationShape::sine &&
      modulation.shape != dsp::ModulationShape::triangle) {
    fail(
        modulationPath + "/shape",
        "expected smoothed-random, sine, or triangle");
  }
  if (!std::isfinite(modulation.channelFraction) ||
      !(modulation.channelFraction >= 0.0) ||
      !(modulation.channelFraction <= 1.0)) {
    fail(
        modulationPath + "/channelFraction",
        "expected finite value in [0, 1]");
  }
  if (modulation.interpolation != dsp::ModulationInterpolation::lagrange3 &&
      modulation.interpolation != dsp::ModulationInterpolation::linear &&
      modulation.interpolation != dsp::ModulationInterpolation::allpass) {
    fail(
        modulationPath + "/interpolation",
        "expected lagrange3, linear, or allpass");
  }
  if (modulation.interpolationMarginSamples !=
      kModulationInterpolationMarginSamples) {
    fail(
        modulationPath + "/interpolationMarginSamples",
        "expected the fixed worst-case Interpolation margin");
  }
  const auto expectedExcursionSamples =
      resolveExcursionSamples(modulation.depthMs, resolved.sampleRate);
  const auto excursionTolerance =
      1e-9 * std::max(1.0, expectedExcursionSamples);
  if (!std::isfinite(modulation.excursionSamples) ||
      std::abs(modulation.excursionSamples - expectedExcursionSamples) >
          excursionTolerance) {
    fail(
        modulationPath + "/excursionSamples",
        "expected depthMs resolved to samples at this Composition's "
        "sample rate");
  }
  const auto modulationActive = !modulation.channelModulated.empty();
  if (modulationActive !=
      (modulation.depthMs > 0.0 && modulation.channelFraction > 0.0)) {
    fail(
        modulationPath + "/channelModulated",
        "expected an empty bypass mask exactly when depthMs or "
        "channelFraction is zero");
  }
  if (modulationActive) {
    const auto requireModulationChannelValues =
        [channels, &modulationPath](
            const std::size_t size, const std::string_view field) {
          if (size != channels) {
            fail(
                modulationPath + std::string(field),
                "expected one value per Channel");
          }
        };
    requireModulationChannelValues(
        modulation.channelSeeds.size(), "/channelSeeds");
    requireModulationChannelValues(
        modulation.channelTargetsPerSample.size(),
        "/channelTargetsPerSample");
    requireModulationChannelValues(
        modulation.channelPhases.size(), "/channelPhases");
    requireModulationChannelValues(
        modulation.channelModulated.size(), "/channelModulated");

    const auto expectedMask = resolveModulationChannelMask(
        resolved.seed, owner, itemIndex, channels, modulation.channelFraction);
    if (modulation.channelModulated != expectedMask) {
      fail(
          modulationPath + "/channelModulated",
          "expected the first ceil(channelFraction * N) entries of the "
          "positionally seeded fixed Channel-selection permutation");
    }

    const auto seedUsage = owner == ModulationOwner::feedbackLoop
        ? kFeedbackLoopModulationSeedUsage
        : kDiffusionModulationSeedUsage;
    const auto headroomSamples =
        resolveModulationHeadroomSamples(modulation.excursionSamples);
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
      const auto expectedSeed = dsp::positionalSplitMix64V1(
          resolved.seed, seedUsage, itemIndex, channel);
      if (modulation.channelSeeds[channel] != expectedSeed) {
        fail(
            modulationPath + "/channelSeeds",
            "expected the derived per-Channel trajectory seed");
      }
      const auto expectedSpread = resolveModulationRateSpread(
          resolved.seed, owner, itemIndex, channel);
      const auto expectedTargetPerSample =
          modulation.rateHz * expectedSpread / resolved.sampleRate;
      const auto targetTolerance =
          1e-9 * std::max(1.0, std::abs(expectedTargetPerSample));
      if (!std::isfinite(modulation.channelTargetsPerSample[channel]) ||
          std::abs(
              modulation.channelTargetsPerSample[channel] -
              expectedTargetPerSample) > targetTolerance) {
        fail(
            modulationPath + "/channelTargetsPerSample",
            "expected rateHz times that Channel's own fixed +-10% "
            "seeded spread, divided by the sample rate");
      }
      const auto expectedPhase =
          resolveModulationPhase(resolved.seed, owner, itemIndex, channel);
      if (!std::isfinite(modulation.channelPhases[channel]) ||
          modulation.channelPhases[channel] != expectedPhase) {
        fail(
            modulationPath + "/channelPhases",
            "expected the derived per-Channel positional phase");
      }

      // The Excursion rejection rule, and the buffer headroom it gates,
      // apply to modulated Channels only.
      if (modulation.channelModulated[channel]) {
        if (!modulationFitsDelay(
                delaysSamples[channel], modulation.excursionSamples)) {
          fail(
              modulationPath + "/depthMs",
              "Channel " + std::to_string(channel) +
                  "'s resolved delay less Excursion does not exceed the "
                  "fixed Interpolation margin");
        }
        const auto expectedBufferSize =
            delaysSamples[channel] + headroomSamples;
        if (bufferSizes[channel] != expectedBufferSize) {
          fail(
              bufferSizesPath,
              "expected the resolved delay plus Excursion plus the "
              "fixed Interpolation margin for a Channel actually "
              "modulated");
        }
      } else if (bufferSizes[channel] != delaysSamples[channel]) {
        fail(
            bufferSizesPath,
            "expected buffer size equal to delay for a Channel "
            "excluded by channelFraction");
      }
    }
  } else {
    if (!modulation.channelSeeds.empty() ||
        !modulation.channelTargetsPerSample.empty() ||
        !modulation.channelPhases.empty() ||
        !modulation.channelModulated.empty()) {
      fail(
          modulationPath,
          "expected no per-Channel trajectory seeds, rates, phases, or "
          "bypass mask when depthMs or channelFraction is zero -- the "
          "resolved bypass");
    }
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
      if (bufferSizes[channel] != delaysSamples[channel]) {
        fail(
            bufferSizesPath,
            "expected buffer size equal to delay when depthMs or "
            "channelFraction is zero -- the resolved bypass");
      }
    }
  }
}

void validateDiffuserStage(
    const dsp::ResolvedConfig& resolved,
    const dsp::ResolvedDiffuser& diffuser,
    const std::uint32_t channels,
    const std::optional<std::size_t>& matrixElements,
    const ResolutionEvidence* const resolutionEvidence,
    const std::uint64_t memoryBudgetBytes,
    const std::size_t stageIndex) {
  const auto path = stagePath(stageIndex);
  const auto validatingRequest =
      resolutionEvidence != nullptr &&
      resolutionEvidence->diffuser.has_value();
  if (validatingRequest &&
      resolutionEvidence->diffuser->expectedStepLengths.size() !=
          resolutionEvidence->diffuser->stepCount) {
    switch (resolutionEvidence->diffuser->sampleBudget.status) {
    case SampleBudgetStatus::valid:
      break;
    case SampleBudgetStatus::nonPositive:
      fail(path + "/totalMs", "expected value greater than zero");
    case SampleBudgetStatus::tooLarge:
      fail(path + "/totalMs", "resolved sample budget is too large");
    case SampleBudgetStatus::belowOneSample:
      fail(
          path + "/totalMs",
          "resolved sample budget must be at least one sample");
    case SampleBudgetStatus::tooFewPositions:
      fail(
          path + "/totalMs",
          "delay strategy requires at least one sample position per Channel");
    }
    fail(path + "/steps", "expected a valid step count and distribution");
  }
  if (diffuser.steps.empty()) {
    fail(path + "/steps", "expected at least one step");
  }
  if (diffuser.totalSamples == 0) {
    fail(path + "/totalSamples", "expected value greater than zero");
  }
  auto maxResolvedBufferSamples = diffuser.totalSamples;
  for (const auto& step : diffuser.steps) {
    if (!step.bufferSizes.empty()) {
      maxResolvedBufferSamples = std::max(
          maxResolvedBufferSamples,
          *std::max_element(
              step.bufferSizes.begin(), step.bufferSizes.end()));
    }
  }
  checkDiffuserMemoryBudget(
      channels,
      maxResolvedBufferSamples,
      diffuser.steps.size(),
      estimateDiffuserModulationBytes(channels, diffuser.steps),
      memoryBudgetBytes,
      path);
  if (validatingRequest &&
      diffuser.steps.size() != resolutionEvidence->diffuser->stepCount) {
    fail(path + "/steps", "expected one resolved step per requested step");
  }
  std::uint64_t stepLengthSum = 0;
  for (std::size_t stepPosition = 0; stepPosition < diffuser.steps.size();
       ++stepPosition) {
    const auto& step = diffuser.steps[stepPosition];
    const auto stepPath = path + "/steps/" + std::to_string(stepPosition);
    if (step.index != stepPosition) {
      fail(stepPath + "/index", "expected the resolved step position");
    }
    if (step.lengthSamples == 0) {
      fail(
          stepPath + "/lengthSamples",
          "expected the complete nonzero step sample budget");
    }
    if (step.lengthSamples == std::numeric_limits<std::uint64_t>::max()) {
      fail(
          stepPath + "/lengthSamples",
          "delay strategy requires at least one sample position per "
          "Channel");
    }
    if (validatingRequest &&
        stepPosition <
            resolutionEvidence->diffuser->expectedStepLengths.size() &&
        step.lengthSamples !=
            resolutionEvidence->diffuser
                ->expectedStepLengths[stepPosition]) {
      fail(
          stepPath + "/lengthSamples",
          "expected the largest-remainder apportioned step sample budget");
    }
    stepLengthSum += step.lengthSamples;
    const auto expectedLengthMs =
        static_cast<double>(step.lengthSamples) * 1000.0 /
        resolved.sampleRate;
    if (step.lengthMs != expectedLengthMs) {
      fail(
          stepPath + "/lengthMs",
          "expected milliseconds derived from the integer sample budget");
    }
    if (step.delayStrategy != dsp::DelayStrategy::segmentedRandom &&
        step.delayStrategy != dsp::DelayStrategy::uniformRandom &&
        step.delayStrategy != dsp::DelayStrategy::even) {
      fail(
          stepPath + "/delayStrategy",
          "expected segmented-random, uniform-random, or even");
    }
    if (step.polarity != dsp::PolarityStrategy::seededRandom &&
        step.polarity != dsp::PolarityStrategy::none) {
      fail(
          stepPath + "/polarity",
          "expected seeded-random or none");
    }
    if (step.mix != dsp::MixMatrixType::hadamard &&
        step.mix != dsp::MixMatrixType::householder &&
        step.mix != dsp::MixMatrixType::randomOrthogonal) {
      fail(
          stepPath + "/mix",
          "expected hadamard, householder, or random-orthogonal");
    }
    if (step.mix == dsp::MixMatrixType::hadamard && !isPowerOfTwo(channels)) {
      fail(
          stepPath + "/mix",
          "hadamard requires a power-of-two Channel count");
    }
    if (!matrixElements.has_value()) {
      fail(stepPath + "/mix", "mixing matrix is too large");
    }
    if (requiresDistinctChannelPositions(step.delayStrategy) &&
        step.lengthSamples + 1 < channels) {
      fail(
          stepPath + "/lengthSamples",
          "delay strategy requires at least one sample position per "
          "Channel");
    }
    const auto requireChannelValues =
        [channels, &stepPath](const std::size_t size,
                              const std::string_view field) {
          if (size != channels) {
            fail(stepPath + std::string(field), "expected one value per Channel");
          }
        };
    requireChannelValues(step.delaysSamples.size(), "/delaysSamples");
    requireChannelValues(step.delaysMs.size(), "/delaysMs");
    requireChannelValues(step.bufferSizes.size(), "/bufferSizes");
    requireChannelValues(step.permutation.size(), "/permutation");
    requireChannelValues(step.polaritySigns.size(), "/polaritySigns");

    const auto stepModulationActive = step.modulation.has_value() &&
        !step.modulation->channelModulated.empty();

    auto sortedDelays = step.delaysSamples;
    std::sort(sortedDelays.begin(), sortedDelays.end());
    if (step.delayStrategy != dsp::DelayStrategy::uniformRandom &&
        std::adjacent_find(sortedDelays.begin(), sortedDelays.end()) !=
            sortedDelays.end()) {
      fail(
          stepPath + "/delaysSamples",
          "expected distinct delays within the step sample budget");
    }
    if (step.delayStrategy == dsp::DelayStrategy::segmentedRandom) {
      const auto positions = step.lengthSamples + 1;
      for (std::uint32_t channel = 0; channel < channels; ++channel) {
        const auto start =
            partitionBoundary(positions, channels, channel);
        const auto end =
            partitionBoundary(positions, channels, channel + 1U);
        if (sortedDelays[channel] < start ||
            sortedDelays[channel] >= end) {
          fail(
              stepPath + "/delaysSamples",
              "segmented-random requires one delay in every segment");
        }
      }
    }
    std::vector<bool> seenPermutation(channels, false);
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
      const auto delay = step.delaysSamples[channel];
      if (delay > step.lengthSamples) {
        fail(
            stepPath + "/delaysSamples",
            "expected distinct delays within the step sample budget");
      }
      const auto expectedDelayMs =
          static_cast<double>(delay) * 1000.0 / resolved.sampleRate;
      if (step.delaysMs[channel] != expectedDelayMs) {
        fail(
            stepPath + "/delaysMs",
            "expected milliseconds derived from integer delays");
      }
      // Guards the mask index defensively rather than trusting its size
      // yet -- that size is itself checked in the same place, below.
      const auto channelIsModulated = stepModulationActive &&
          channel < step.modulation->channelModulated.size() &&
          step.modulation->channelModulated[channel];
      if (!channelIsModulated && step.bufferSizes[channel] != delay) {
        fail(
            stepPath + "/bufferSizes",
            "expected each buffer size to equal its integer delay");
      }
      const auto source = step.permutation[channel];
      if (source >= channels || seenPermutation[source]) {
        fail(
            stepPath + "/permutation",
            "expected a permutation of Channel indices");
      }
      if (!step.shuffle && source != channel) {
        fail(
            stepPath + "/permutation",
            "shuffle false requires the identity permutation");
      }
      seenPermutation[source] = true;
      if (step.polaritySigns[channel] != -1 &&
          step.polaritySigns[channel] != 1) {
        fail(stepPath + "/polaritySigns", "expected -1 or 1");
      }
      if (step.polarity == dsp::PolarityStrategy::none &&
          step.polaritySigns[channel] != 1) {
        fail(
            stepPath + "/polaritySigns",
            "polarity none requires all +1 signs");
      }
      if (step.delayStrategy == dsp::DelayStrategy::even &&
          delay != evenDelay(step.lengthSamples, channels, channel)) {
        fail(
            stepPath + "/delaysSamples",
            "even requires delays distributed over the available "
            "positions");
      }
    }
    if (step.matrix.size() != *matrixElements) {
      fail(stepPath + "/matrix", "expected an N by N matrix");
    }
    validateResolvedMixMatrix(
        step.mix, channels, step.matrix, stepPath + "/matrix");

    if (step.modulation.has_value()) {
      validateResolvedModulation(
          resolved,
          *step.modulation,
          channels,
          ModulationOwner::diffusionStep,
          /*itemIndex=*/step.index,
          step.delaysSamples,
          step.bufferSizes,
          stepPath + "/modulation",
          stepPath + "/bufferSizes");
    }
  }
  const auto expectedTotalSamples = stepLengthSum +
      resolveDiffuserModulationReachSamples(diffuser.steps);
  if (expectedTotalSamples != diffuser.totalSamples) {
    fail(
        path + "/steps",
        "expected step sample budgets, plus every actively modulated "
        "step's own resolved Modulation reach, to sum to the resolved "
        "total");
  }
}

// Validates one resolved Feedback Loop stage at `stageIndex` -- 1 in the
// loop-only shape, 2 when it follows a Diffuser.
void validateFeedbackLoopStage(
    const dsp::ResolvedConfig& resolved,
    const dsp::ResolvedFeedbackLoop& feedbackLoop,
    const std::uint32_t channels,
    const std::optional<std::size_t>& matrixElements,
    const std::uint64_t memoryBudgetBytes,
    const std::size_t stageIndex) {
  const auto path = stagePath(stageIndex);
  if (feedbackLoop.channels != channels) {
    fail(
        path + "/channels",
        "expected the resolved Split Channel count");
  }
  if (feedbackLoop.delayStrategy != dsp::DelayStrategy::segmentedRandom &&
      feedbackLoop.delayStrategy != dsp::DelayStrategy::uniformRandom &&
      feedbackLoop.delayStrategy != dsp::DelayStrategy::even) {
    fail(
        path + "/delayStrategy",
        "expected segmented-random, uniform-random, or even");
  }
  if (!(feedbackLoop.delayMinMs > 0.0) ||
      !std::isfinite(feedbackLoop.delayMinMs)) {
    fail(
        path + "/delayMinMs",
        "expected finite value greater than zero");
  }
  if (!(feedbackLoop.delayMaxMs >= feedbackLoop.delayMinMs) ||
      !std::isfinite(feedbackLoop.delayMaxMs)) {
    fail(
        path + "/delayMaxMs",
        "expected finite value at least delayMinMs");
  }
  const auto expectedDelayMinSamples = static_cast<std::uint64_t>(
      std::floor(
          static_cast<long double>(feedbackLoop.delayMinMs) *
              resolved.sampleRate / 1000.0L +
          0.5L));
  const auto expectedDelayMaxSamples = static_cast<std::uint64_t>(
      std::floor(
          static_cast<long double>(feedbackLoop.delayMaxMs) *
              resolved.sampleRate / 1000.0L +
          0.5L));
  if (feedbackLoop.delayMinSamples != expectedDelayMinSamples ||
      feedbackLoop.delayMaxSamples != expectedDelayMaxSamples) {
    fail(
        path,
        "expected delayMinSamples/delayMaxSamples derived from "
        "delayMinMs/delayMaxMs");
  }
  if (feedbackLoop.delayMaxSamples < feedbackLoop.delayMinSamples) {
    fail(
        path,
        "expected delayMaxSamples at least delayMinSamples");
  }
  const auto maxResolvedBufferSamples = feedbackLoop.bufferSizes.empty()
      ? feedbackLoop.delayMaxSamples
      : *std::max_element(
            feedbackLoop.bufferSizes.begin(), feedbackLoop.bufferSizes.end());
  checkFeedbackLoopMemoryBudget(
      channels,
      std::max(feedbackLoop.delayMaxSamples, maxResolvedBufferSamples),
      memoryBudgetBytes,
      path);
  if (!(feedbackLoop.rt60Sec > 0.0) ||
      !std::isfinite(feedbackLoop.rt60Sec)) {
    fail(
        path + "/rt60Sec",
        "expected finite value greater than zero");
  }
  if (!(feedbackLoop.decayMargin > 0.0) ||
      !std::isfinite(feedbackLoop.decayMargin)) {
    fail(
        path + "/decayMargin",
        "expected finite value greater than zero");
  }
  if (feedbackLoop.silenceFloorDb.has_value() &&
      !std::isfinite(*feedbackLoop.silenceFloorDb)) {
    fail(path + "/silenceFloorDb", "expected a finite value when present");
  }
  if (!feedbackLoop.damping.has_value() &&
      feedbackLoop.tailBudgetSamples !=
          resolveTailBudgetSamples(
              feedbackLoop.rt60Sec,
              feedbackLoop.decayMargin,
              resolved.sampleRate)) {
    fail(
        path + "/tailBudgetSamples",
        "expected value derived from rt60Sec and decayMargin");
  }
  if (feedbackLoop.tailBudgetSamples == 0) {
    fail(
        path + "/tailBudgetSamples",
        "expected value greater than zero");
  }
  if (feedbackLoop.mix != dsp::MixMatrixType::hadamard &&
      feedbackLoop.mix != dsp::MixMatrixType::householder &&
      feedbackLoop.mix != dsp::MixMatrixType::randomOrthogonal) {
    fail(
        path + "/mix",
        "expected hadamard, householder, or random-orthogonal");
  }
  if (feedbackLoop.mix == dsp::MixMatrixType::hadamard &&
      !isPowerOfTwo(channels)) {
    fail(
        path + "/mix",
        "hadamard requires a power-of-two Channel count");
  }
  if (!matrixElements.has_value()) {
    fail(path + "/mix", "mixing matrix is too large");
  }
  const auto requireChannelValues =
      [channels, &path](const std::size_t size, const std::string_view field) {
        if (size != channels) {
          fail(
              path + std::string(field),
              "expected one value per Channel");
        }
      };
  requireChannelValues(feedbackLoop.delaysSamples.size(), "/delaysSamples");
  requireChannelValues(feedbackLoop.delaysMs.size(), "/delaysMs");
  requireChannelValues(feedbackLoop.bufferSizes.size(), "/bufferSizes");
  requireChannelValues(feedbackLoop.gains.size(), "/gains");

  const auto modulationActive = feedbackLoop.modulation.has_value() &&
      !feedbackLoop.modulation->channelModulated.empty();
  if (!modulationActive) {
    const auto expectedBlockSizeBound = *std::min_element(
        feedbackLoop.delaysSamples.begin(), feedbackLoop.delaysSamples.end());
    if (feedbackLoop.blockSizeBoundSamples != expectedBlockSizeBound) {
      fail(
          path + "/blockSizeBoundSamples",
          "expected the minimum of the resolved per-Channel delays");
    }
  }
  if (feedbackLoop.blockSizeBoundSamples == 0) {
    fail(
        path + "/blockSizeBoundSamples",
        "expected value greater than zero");
  }

  if (feedbackLoop.gainMode != dsp::GainMode::perChannel &&
      feedbackLoop.gainMode != dsp::GainMode::uniform) {
    fail(path + "/gainMode", "expected per-channel or uniform");
  }
  double meanLoopTimeSec = 0.0;
  if (feedbackLoop.gainMode == dsp::GainMode::uniform) {
    for (const auto delay : feedbackLoop.delaysSamples) {
      meanLoopTimeSec += static_cast<double>(delay) / resolved.sampleRate;
    }
    meanLoopTimeSec /= channels;
  }

  auto sortedDelays = feedbackLoop.delaysSamples;
  std::sort(sortedDelays.begin(), sortedDelays.end());
  if (feedbackLoop.delayStrategy != dsp::DelayStrategy::uniformRandom &&
      std::adjacent_find(sortedDelays.begin(), sortedDelays.end()) !=
          sortedDelays.end()) {
    fail(
        path + "/delaysSamples",
        "expected distinct delays within the delay range");
  }
  const auto positions =
      feedbackLoop.delayMaxSamples - feedbackLoop.delayMinSamples + 1;
  if (feedbackLoop.delayStrategy == dsp::DelayStrategy::segmentedRandom) {
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
      const auto start = partitionBoundary(positions, channels, channel);
      const auto end =
          partitionBoundary(positions, channels, channel + 1U);
      const auto offset =
          sortedDelays[channel] - feedbackLoop.delayMinSamples;
      if (offset < start || offset >= end) {
        fail(
            path + "/delaysSamples",
            "segmented-random requires one delay in every segment");
      }
    }
  }
  for (std::uint32_t channel = 0; channel < channels; ++channel) {
    const auto delay = feedbackLoop.delaysSamples[channel];
    if (delay < feedbackLoop.delayMinSamples ||
        delay > feedbackLoop.delayMaxSamples) {
      fail(
          path + "/delaysSamples",
          "expected each delay within [delayMinSamples, delayMaxSamples]");
    }
    const auto expectedDelayMs =
        static_cast<double>(delay) * 1000.0 / resolved.sampleRate;
    if (feedbackLoop.delaysMs[channel] != expectedDelayMs) {
      fail(
          path + "/delaysMs",
          "expected milliseconds derived from integer delays");
    }
    const auto channelIsModulated = modulationActive &&
        channel < feedbackLoop.modulation->channelModulated.size() &&
        feedbackLoop.modulation->channelModulated[channel];
    if (!channelIsModulated && feedbackLoop.bufferSizes[channel] != delay) {
      fail(
          path + "/bufferSizes",
          "expected each buffer size to equal its integer delay");
    }
    if (feedbackLoop.delayStrategy == dsp::DelayStrategy::even &&
        delay != feedbackLoop.delayMinSamples +
                     evenDelay(positions - 1, channels, channel)) {
      fail(
          path + "/delaysSamples",
          "even requires delays distributed over the available "
          "positions");
    }
    const auto loopTimeSec =
        static_cast<double>(delay) / resolved.sampleRate;
    const auto expectedGain = std::pow(
        10.0,
        -3.0 *
            (feedbackLoop.gainMode == dsp::GainMode::uniform
                 ? meanLoopTimeSec
                 : loopTimeSec) /
            feedbackLoop.rt60Sec);
    if (!(feedbackLoop.gains[channel] > 0.0) ||
        !(feedbackLoop.gains[channel] < 1.0) ||
        !std::isfinite(feedbackLoop.gains[channel])) {
      fail(
          path + "/gains",
          "expected finite gain strictly between zero and one");
    }
    const auto gainTolerance = 4.0 * std::numeric_limits<double>::epsilon();
    if (std::abs(feedbackLoop.gains[channel] - expectedGain) >
        gainTolerance) {
      fail(
          path + "/gains",
          "expected gain solved from that Channel's own loop time and "
          "rt60Sec");
    }
  }
  if (feedbackLoop.matrix.size() != *matrixElements) {
    fail(path + "/matrix", "expected an N by N matrix");
  }
  validateResolvedMixMatrix(
      feedbackLoop.mix,
      channels,
      feedbackLoop.matrix,
      path + "/matrix");

  if (feedbackLoop.damping.has_value()) {
    const auto& damping = *feedbackLoop.damping;
    const auto dampingPath = path + "/damping";
    if (!std::isfinite(damping.highRatio) || !(damping.highRatio > 0.0)) {
      fail(
          dampingPath + "/highRatio",
          "expected finite value greater than zero");
    }
    if (!std::isfinite(damping.highHz) || !(damping.highHz > 0.0) ||
        !(damping.highHz < resolved.sampleRate / 2.0)) {
      fail(
          dampingPath + "/highHz",
          "expected finite value strictly between 0 Hz and Nyquist");
    }
    if (!std::isfinite(damping.lowRatio) || !(damping.lowRatio > 0.0)) {
      fail(
          dampingPath + "/lowRatio",
          "expected finite value greater than zero");
    }
    if (!std::isfinite(damping.lowHz) || !(damping.lowHz > 0.0) ||
        !(damping.lowHz < resolved.sampleRate / 2.0)) {
      fail(
          dampingPath + "/lowHz",
          "expected finite value strictly between 0 Hz and Nyquist");
    }

    const auto requireDampingChannelValues =
        [channels, &dampingPath](
            const std::size_t size, const std::string_view field) {
          if (size != channels) {
            fail(
                dampingPath + std::string(field),
                "expected one value per Channel");
          }
        };
    requireDampingChannelValues(
        damping.highShelfGains.size(), "/highShelfGains");
    requireDampingChannelValues(damping.highShelfB0.size(), "/highShelfB0");
    requireDampingChannelValues(damping.highShelfB1.size(), "/highShelfB1");
    requireDampingChannelValues(damping.highShelfA1.size(), "/highShelfA1");
    requireDampingChannelValues(
        damping.lowShelfGains.size(), "/lowShelfGains");
    requireDampingChannelValues(damping.lowShelfB0.size(), "/lowShelfB0");
    requireDampingChannelValues(damping.lowShelfB1.size(), "/lowShelfB1");
    requireDampingChannelValues(damping.lowShelfA1.size(), "/lowShelfA1");
    requireDampingChannelValues(
        damping.expectedLowRt60Sec.size(), "/expectedLowRt60Sec");
    requireDampingChannelValues(
        damping.expectedReferenceRt60Sec.size(),
        "/expectedReferenceRt60Sec");
    requireDampingChannelValues(
        damping.expectedHighRt60Sec.size(), "/expectedHighRt60Sec");
    requireDampingChannelValues(
        damping.contractionBoundFloat32.size(), "/contractionBoundFloat32");
    requireDampingChannelValues(
        damping.contractionMarginFloat32.size(),
        "/contractionMarginFloat32");
    requireDampingChannelValues(
        damping.contractionBoundFloat64.size(), "/contractionBoundFloat64");
    requireDampingChannelValues(
        damping.contractionMarginFloat64.size(),
        "/contractionMarginFloat64");

    const auto relativeTolerance = [](const double expected) noexcept {
      return 1e-9 * std::max(1.0, std::abs(expected));
    };
    const auto checkShelf =
        [&](const double resolvedGain,
            const double resolvedB0,
            const double resolvedB1,
            const double resolvedA1,
            const double channelGain,
            const double ratio,
            const double cornerHz,
            const bool isHighShelf,
            const std::string_view gainsField) -> ShelfCoefficients {
      const auto expectedGain = resolveShelfGain(channelGain, ratio);
      if (!std::isfinite(resolvedGain) || !(resolvedGain > 0.0)) {
        fail(
            dampingPath + std::string(gainsField),
            "expected finite gain greater than zero");
      }
      if (std::abs(resolvedGain - expectedGain) >
          relativeTolerance(expectedGain)) {
        fail(
            dampingPath + std::string(gainsField),
            "expected gain solved from that Channel's own decay gain and "
            "ratio");
      }
      const auto expectedCoefficients = isHighShelf
          ? resolveHighShelfCoefficients(
                expectedGain, cornerHz, resolved.sampleRate)
          : resolveLowShelfCoefficients(
                expectedGain, cornerHz, resolved.sampleRate);
      if (!std::isfinite(resolvedB0) || !std::isfinite(resolvedB1) ||
          !std::isfinite(resolvedA1) ||
          std::abs(resolvedB0 - expectedCoefficients.b0) >
              relativeTolerance(expectedCoefficients.b0) ||
          std::abs(resolvedB1 - expectedCoefficients.b1) >
              relativeTolerance(expectedCoefficients.b1) ||
          std::abs(resolvedA1 - expectedCoefficients.a1) >
              relativeTolerance(expectedCoefficients.a1)) {
        fail(
            dampingPath,
            "expected shelf coefficients solved from the resolved gain, "
            "corner, and sampleRate");
      }
      return expectedCoefficients;
    };
    const auto matrixBoundFloat32 = resolveMatrixContractionBound(
        channels, std::numeric_limits<float>::epsilon());
    const auto matrixBoundFloat64 = resolveMatrixContractionBound(
        channels, std::numeric_limits<double>::epsilon());
    auto expectedSlowestResolvedRt60Sec = feedbackLoop.rt60Sec;
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
      const auto channelGain = feedbackLoop.gains[channel];
      const auto highCoefficients = checkShelf(
          damping.highShelfGains[channel],
          damping.highShelfB0[channel],
          damping.highShelfB1[channel],
          damping.highShelfA1[channel],
          channelGain,
          damping.highRatio,
          damping.highHz,
          /*isHighShelf=*/true,
          "/highShelfGains");
      const auto lowCoefficients = checkShelf(
          damping.lowShelfGains[channel],
          damping.lowShelfB0[channel],
          damping.lowShelfB1[channel],
          damping.lowShelfA1[channel],
          channelGain,
          damping.lowRatio,
          damping.lowHz,
          /*isHighShelf=*/false,
          "/lowShelfGains");

      const auto lossDb = 20.0 * std::log10(channelGain);
      const auto loopTimeSec =
          static_cast<double>(feedbackLoop.delaysSamples[channel]) /
          resolved.sampleRate;
      const auto impliedUndampedRt60Sec = -60.0 * loopTimeSec / lossDb;
      const auto expectedLowRt60Sec =
          damping.lowRatio * impliedUndampedRt60Sec;
      const auto expectedHighRt60Sec =
          damping.highRatio * impliedUndampedRt60Sec;
      const auto referenceMagnitude =
          shelfMagnitudeAtFrequency(
              lowCoefficients, 1000.0, resolved.sampleRate) *
          shelfMagnitudeAtFrequency(
              highCoefficients, 1000.0, resolved.sampleRate);
      const auto referenceLossDb =
          lossDb + 20.0 * std::log10(referenceMagnitude);
      const auto expectedReferenceRt60Sec =
          -60.0 * loopTimeSec / referenceLossDb;

      if (!std::isfinite(damping.expectedLowRt60Sec[channel]) ||
          std::abs(damping.expectedLowRt60Sec[channel] - expectedLowRt60Sec) >
              relativeTolerance(expectedLowRt60Sec)) {
        fail(
            dampingPath + "/expectedLowRt60Sec",
            "expected lowRatio times that Channel's own implied undamped "
            "RT60");
      }
      if (!std::isfinite(damping.expectedHighRt60Sec[channel]) ||
          std::abs(
              damping.expectedHighRt60Sec[channel] - expectedHighRt60Sec) >
              relativeTolerance(expectedHighRt60Sec)) {
        fail(
            dampingPath + "/expectedHighRt60Sec",
            "expected highRatio times that Channel's own implied undamped "
            "RT60");
      }
      if (!std::isfinite(damping.expectedReferenceRt60Sec[channel]) ||
          std::abs(
              damping.expectedReferenceRt60Sec[channel] -
              expectedReferenceRt60Sec) >
              relativeTolerance(expectedReferenceRt60Sec)) {
        fail(
            dampingPath + "/expectedReferenceRt60Sec",
            "expected the RT60 implied by both shelves' combined response "
            "at 1 kHz");
      }

      const auto expectedBoundFloat32 = resolveChannelContractionBound(
          channelGain,
          damping.lowShelfGains[channel],
          damping.highShelfGains[channel],
          matrixBoundFloat32);
      const auto expectedBoundFloat64 = resolveChannelContractionBound(
          channelGain,
          damping.lowShelfGains[channel],
          damping.highShelfGains[channel],
          matrixBoundFloat64);
      const auto checkContraction =
          [&](const double resolvedBound,
              const double resolvedMargin,
              const double expectedBound,
              const std::string_view field) {
            const auto expectedMargin = 1.0 - expectedBound;
            if (!std::isfinite(resolvedBound) ||
                std::abs(resolvedBound - expectedBound) >
                    relativeTolerance(expectedBound) ||
                !std::isfinite(resolvedMargin) ||
                std::abs(resolvedMargin - expectedMargin) >
                    relativeTolerance(expectedMargin)) {
              fail(
                  dampingPath + std::string(field),
                  "expected the conservative one-circulation contraction "
                  "bound and margin solved from the resolved gains and "
                  "mixing matrix");
            }
          };
      checkContraction(
          damping.contractionBoundFloat32[channel],
          damping.contractionMarginFloat32[channel],
          expectedBoundFloat32,
          "/contractionBoundFloat32");
      checkContraction(
          damping.contractionBoundFloat64[channel],
          damping.contractionMarginFloat64[channel],
          expectedBoundFloat64,
          "/contractionBoundFloat64");
      if (!(damping.contractionBoundFloat32[channel] < 1.0) ||
          !(damping.contractionBoundFloat64[channel] < 1.0)) {
        fail(
            dampingPath,
            "Channel " + std::to_string(channel) +
                "'s conservative one-circulation contraction bound is not "
                "strictly below unity at both float32 and float64 "
                "precision -- reduce highRatio/lowRatio or move a shelf "
                "corner farther from the reference band");
      }

      if (damping.highRatio > 1.0) {
        expectedSlowestResolvedRt60Sec = std::max(
            expectedSlowestResolvedRt60Sec, expectedHighRt60Sec);
        expectedSlowestResolvedRt60Sec = std::max(
            expectedSlowestResolvedRt60Sec,
            resolveShelfSettlingTimeSec(
                highCoefficients.a1, resolved.sampleRate));
      }
      if (damping.lowRatio > 1.0) {
        expectedSlowestResolvedRt60Sec = std::max(
            expectedSlowestResolvedRt60Sec, expectedLowRt60Sec);
        expectedSlowestResolvedRt60Sec = std::max(
            expectedSlowestResolvedRt60Sec,
            resolveShelfSettlingTimeSec(
                lowCoefficients.a1, resolved.sampleRate));
      }
    }

    const auto resolvedSlowestRt60Sec = damping.slowestResolvedRt60Sec;
    if (!std::isfinite(resolvedSlowestRt60Sec) ||
        std::abs(resolvedSlowestRt60Sec - expectedSlowestResolvedRt60Sec) >
            relativeTolerance(expectedSlowestResolvedRt60Sec)) {
      fail(
          dampingPath + "/slowestResolvedRt60Sec",
          "expected the slower of rt60Sec, each boosted shelf's own "
          "conservative feedback-decay estimate, and its state-settling "
          "time");
    }
    const auto expectedTailBudgetSamples = resolveTailBudgetSamples(
        expectedSlowestResolvedRt60Sec,
        feedbackLoop.decayMargin,
        resolved.sampleRate);
    if (feedbackLoop.tailBudgetSamples != expectedTailBudgetSamples) {
      fail(
          path + "/tailBudgetSamples",
          "expected value derived from the slowest resolved RT60 and "
          "decayMargin");
    }
  }

  if (feedbackLoop.modulation.has_value()) {
    validateResolvedModulation(
        resolved,
        *feedbackLoop.modulation,
        channels,
        ModulationOwner::feedbackLoop,
        /*itemIndex=*/0,
        feedbackLoop.delaysSamples,
        feedbackLoop.bufferSizes,
        path + "/modulation",
        path + "/bufferSizes");
    if (modulationActive) {
      const auto expectedBlockSizeBound =
          resolveModulationBlockSizeBoundSamples(
              feedbackLoop.delaysSamples,
              feedbackLoop.modulation->channelModulated,
              feedbackLoop.modulation->excursionSamples);
      if (feedbackLoop.blockSizeBoundSamples != expectedBlockSizeBound) {
        fail(
            path + "/blockSizeBoundSamples",
            "expected the shortest instantaneous per-Channel delay "
            "across every Channel, moved or not");
      }
    }
  }
}

void validateResolvedDownmixFields(
    const dsp::ResolvedConfig& resolved,
    const dsp::ResolvedDownmix& downmix,
    const std::uint32_t channels,
    const std::uint64_t orthogonalUsage,
    const dsp::DownmixAlignment expectedAlignment,
    const std::string& path) {
  if (downmix.inputChannels != channels || downmix.outputChannels != 2) {
    fail(path, "Downmix dimensions must map the internal Channels to stereo");
  }
  if (downmix.strategy == dsp::DownmixStrategy::select) {
    if (!downmix.leftChannel.has_value()) {
      fail(path + "/leftChannel", "required field is missing");
    }
    if (*downmix.leftChannel >= channels) {
      fail(
          path + "/leftChannel", "expected a Channel index within [0, N)");
    }
    if (downmix.rightChannel.has_value()) {
      if (*downmix.rightChannel >= channels) {
        fail(
            path + "/rightChannel",
            "expected a Channel index within [0, N)");
      }
      if (*downmix.rightChannel == *downmix.leftChannel) {
        fail(
            path + "/rightChannel",
            "expected a Channel distinct from leftChannel");
      }
    }
  } else {
    if (downmix.leftChannel.has_value()) {
      fail(
          path + "/leftChannel",
          std::string("not applicable to strategy ") +
              downmixStrategyLabel(downmix.strategy));
    }
    if (downmix.rightChannel.has_value()) {
      fail(
          path + "/rightChannel",
          std::string("not applicable to strategy ") +
              downmixStrategyLabel(downmix.strategy));
    }
    if (downmix.strategy != dsp::DownmixStrategy::sumAll && channels < 2) {
      fail(
          path + "/strategy",
          std::string(downmixStrategyLabel(downmix.strategy)) +
              " requires at least two Channels");
    }
  }
  if (!(downmix.compensation > 0.0) || !std::isfinite(downmix.compensation)) {
    fail(path + "/compensation", "expected finite positive gain");
  }
  double expectedCompensation = 0.0;
  switch (downmix.normalisation) {
  case dsp::EnergyNormalisation::energy:
    expectedCompensation =
        channels == 1
            ? 1.0 / std::sqrt(2.0)
            : std::sqrt(static_cast<double>(channels) / 2.0);
    break;
  case dsp::EnergyNormalisation::none:
    expectedCompensation = 1.0;
    break;
  default:
    fail(path + "/normalisation", "expected energy or none");
  }
  if (downmix.compensation != expectedCompensation) {
    fail(
        path + "/compensation",
        "expected gain derived from Downmix normalisation");
  }
  std::vector<double> expectedLeftRow;
  std::vector<double> expectedRightRow;
  if (downmix.strategy == dsp::DownmixStrategy::select) {
    expectedLeftRow = selectRow(*downmix.leftChannel, channels);
    expectedRightRow = downmix.rightChannel.has_value()
        ? selectRow(*downmix.rightChannel, channels)
        : expectedLeftRow;
  } else if (downmix.strategy == dsp::DownmixStrategy::halves) {
    expectedLeftRow = halvesRow(channels, /*leftGroup=*/true);
    expectedRightRow = halvesRow(channels, /*leftGroup=*/false);
  } else if (downmix.strategy == dsp::DownmixStrategy::alternating) {
    expectedLeftRow = alternatingRow(channels, /*leftGroup=*/true);
    expectedRightRow = alternatingRow(channels, /*leftGroup=*/false);
  } else if (downmix.strategy == dsp::DownmixStrategy::sumAll) {
    expectedLeftRow = sumAllRow(channels);
    expectedRightRow = expectedLeftRow;
  } else {
    auto matrix = resolveRandomOrthogonalMatrix(
        channels, resolved.seed, orthogonalUsage);
    if (!matrix.has_value()) {
      fail(
          path + "/strategy",
          "RandomOrthogonal construction was singular or near-singular");
    }
    auto rows = extractDownmixRows(*matrix, channels);
    expectedLeftRow = std::move(rows.left);
    expectedRightRow = std::move(rows.right);
  }
  if (downmix.leftRow != expectedLeftRow) {
    fail(
        path + "/leftRow", "expected the unit-norm row derived from strategy");
  }
  if (downmix.rightRow != expectedRightRow) {
    fail(
        path + "/rightRow",
        "expected the unit-norm row derived from strategy");
  }
  if (downmix.effectiveLeftRow !=
      scaledRow(expectedLeftRow, downmix.compensation)) {
    fail(
        path + "/effectiveLeftRow", "expected leftRow scaled by compensation");
  }
  if (downmix.effectiveRightRow !=
      scaledRow(expectedRightRow, downmix.compensation)) {
    fail(
        path + "/effectiveRightRow",
        "expected rightRow scaled by compensation");
  }
  if (downmix.alignment != expectedAlignment) {
    fail(
        path + "/alignment",
        "expected the Alignment expectation derived from Composition "
        "wiring");
  }
  if (!std::isfinite(downmix.widthDeg) || downmix.widthDeg < 0.0 ||
      downmix.widthDeg > 180.0) {
    fail(path + "/widthDeg", "expected a finite value within [0, 180]");
  }
  if (downmix.widthMatrix != resolveWidthMatrix(downmix.widthDeg)) {
    fail(path + "/widthMatrix", "expected the matrix derived from widthDeg");
  }
  if (downmix.coherentDownmixAblation !=
      resolveCoherentDownmixAblation(downmix.strategy, downmix.alignment)) {
    fail(
        path + "/coherentDownmixAblation",
        "expected the Coherent Downmix ablation tag derived from "
        "strategy and Alignment together");
  }
}

void validateResolvedConfig(
    const dsp::ResolvedConfig& resolved,
    const ResolutionEvidence* const resolutionEvidence,
    const std::uint64_t memoryBudgetBytes) {
  if (const auto reason = formatVersionRejectionReason(resolved.formatVersion)) {
    fail("/formatVersion", *reason);
  }
  if (resolved.sampleRate == 0) {
    fail("/sampleRate", "expected value greater than zero");
  }
  validateShape(resolved.composition);
  if (resolved.composition.stages.empty()) {
    if (resolved.composition.early.has_value()) {
      fail(
          "/composition/early",
          "not applicable to the empty identity Composition");
    }
    if (resolved.composition.dryDb != 0.0) {
      fail(
          "/composition/dryDb",
          "not applicable to the empty identity Composition");
    }
    if (resolved.composition.dryGain != 1.0) {
      fail(
          "/composition/dryGain",
          "not applicable to the empty identity Composition");
    }
    if (resolved.composition.wetDb != 0.0) {
      fail(
          "/composition/wetDb",
          "not applicable to the empty identity Composition");
    }
    if (resolved.composition.wetGain != 1.0) {
      fail(
          "/composition/wetGain",
          "not applicable to the empty identity Composition");
    }
    if (!resolved.composition.wetOnly) {
      fail(
          "/composition/wetOnly",
          "not applicable to the empty identity Composition");
    }
    if (resolved.composition.preDelayMs != 0.0) {
      fail(
          "/composition/preDelayMs",
          "not applicable to the empty identity Composition");
    }
    if (resolved.composition.preDelaySamples != 0) {
      fail(
          "/composition/preDelaySamples",
          "not applicable to the empty identity Composition");
    }
    return;
  }

  if (!std::isfinite(resolved.composition.mainLevelDb)) {
    fail("/composition/mainLevelDb", "expected a finite value");
  }
  validateFloatRepresentableGain(
      "/composition/mainGain", resolved.composition.mainGain);
  if (resolved.composition.mainGain !=
      resolveLinearGainFromDb(resolved.composition.mainLevelDb)) {
    fail(
        "/composition/mainGain",
        "expected gain derived from mainLevelDb");
  }

  if (!std::isfinite(resolved.composition.dryDb)) {
    fail("/composition/dryDb", "expected a finite value");
  }
  validateFloatRepresentableGain(
      "/composition/dryGain", resolved.composition.dryGain);
  if (resolved.composition.dryGain !=
      resolveLinearGainFromDb(resolved.composition.dryDb)) {
    fail("/composition/dryGain", "expected gain derived from dryDb");
  }
  if (!std::isfinite(resolved.composition.wetDb)) {
    fail("/composition/wetDb", "expected a finite value");
  }
  validateFloatRepresentableGain(
      "/composition/wetGain", resolved.composition.wetGain);
  if (resolved.composition.wetGain !=
      resolveLinearGainFromDb(resolved.composition.wetDb)) {
    fail("/composition/wetGain", "expected gain derived from wetDb");
  }

  if (!std::isfinite(resolved.composition.preDelayMs) ||
      resolved.composition.preDelayMs < 0.0 ||
      resolved.composition.preDelayMs > 200.0) {
    fail(
        "/composition/preDelayMs",
        "expected a finite value within 0-200 ms");
  }
  const auto expectedPreDelaySamples = resolvePreDelaySamples(
      resolved.composition.preDelayMs, resolved.sampleRate);
  if (resolved.composition.preDelaySamples != expectedPreDelaySamples) {
    fail(
        "/composition/preDelaySamples",
        "expected samples derived from preDelayMs by the nearest-frame "
        "rule");
  }

  const auto& split =
      std::get<dsp::ResolvedSplit>(resolved.composition.stages.front());
  const auto downmixIndex = resolved.composition.stages.size() - 1;
  const auto& downmix = std::get<dsp::ResolvedDownmix>(
      resolved.composition.stages[downmixIndex]);
  const auto channels = split.channels;
  if (split.inputChannels == 0 || split.inputChannels > 2) {
    fail(
        "/composition/stages/0/inputChannels",
        "expected one or two input Channels");
  }
  if (channels == 0) {
    fail("/composition/stages/0/channels", "expected value greater than zero");
  }
  if (split.strategy != dsp::SplitStrategyType::duplicate &&
      split.strategy != dsp::SplitStrategyType::stereoHalves &&
      split.strategy != dsp::SplitStrategyType::stereoInterleave) {
    fail(
        "/composition/stages/0/strategy",
        "expected duplicate, stereo-halves, or stereo-interleave");
  }
  const auto stereoPreserving =
      split.inputChannels == 2 && isStereoPreservingSplit(split.strategy);
  if (stereoPreserving && (channels % 2U) != 0U) {
    fail(
        "/composition/stages/0/channels",
        "stereo-halves and stereo-interleave require an even Channel count "
        "for stereo input");
  }
  if (!(split.sourceGain > 0.0) || !std::isfinite(split.sourceGain)) {
    fail(
        "/composition/stages/0/sourceGain",
        "expected finite positive gain");
  }
  const auto expectedSourceGain =
      split.inputChannels == 1
          ? 1.0
          : stereoPreserving
                ? 1.0
                : 1.0 / std::sqrt(2.0);
  if (split.sourceGain != expectedSourceGain) {
    fail(
        "/composition/stages/0/sourceGain",
        "expected gain derived from Split input mapping");
  }
  if (!(split.channelGain > 0.0) || !std::isfinite(split.channelGain)) {
    fail(
        "/composition/stages/0/channelGain",
        "expected finite positive gain");
  }
  double expectedSplitGain = 0.0;
  switch (split.normalisation) {
  case dsp::EnergyNormalisation::energy:
    expectedSplitGain =
        stereoPreserving
            ? std::sqrt(2.0 / static_cast<double>(channels))
            : 1.0 / std::sqrt(static_cast<double>(channels));
    break;
  case dsp::EnergyNormalisation::none:
    expectedSplitGain = 1.0;
    break;
  default:
    fail(
        "/composition/stages/0/normalisation",
        "expected energy or none");
  }
  if (split.channelGain != expectedSplitGain) {
    fail(
        "/composition/stages/0/channelGain",
        "expected gain derived from Split normalisation");
  }

  const auto matrixElements = matrixElementCount(channels);

  auto containsFeedbackLoop = false;
  const dsp::ResolvedDiffuser* diffuserStage = nullptr;
  for (std::size_t stageIndex = 1; stageIndex < downmixIndex; ++stageIndex) {
    std::visit(
        [&](const auto& stage) {
          using Stage = std::decay_t<decltype(stage)>;
          if constexpr (std::is_same_v<Stage, dsp::ResolvedDiffuser>) {
            diffuserStage = &stage;
            validateDiffuserStage(
                resolved,
                stage,
                channels,
                matrixElements,
                resolutionEvidence,
                memoryBudgetBytes,
                stageIndex);
          } else if constexpr (
              std::is_same_v<Stage, dsp::ResolvedFeedbackLoop>) {
            containsFeedbackLoop = true;
            validateFeedbackLoopStage(
                resolved,
                stage,
                channels,
                matrixElements,
                memoryBudgetBytes,
                stageIndex);
          } else {
            fail(
                stagePath(stageIndex),
                "expected a Diffuser or Feedback Loop stage");
          }
        },
        resolved.composition.stages[stageIndex]);
  }

  const auto expectedAlignment = containsFeedbackLoop
      ? dsp::DownmixAlignment::unaligned
      : dsp::DownmixAlignment::aligned;
  validateResolvedDownmixFields(
      resolved,
      downmix,
      channels,
      kMainDownmixRandomOrthogonalUsage,
      expectedAlignment,
      stagePath(downmixIndex));

  if (resolved.composition.early.has_value()) {
    const auto& early = *resolved.composition.early;
    if (diffuserStage == nullptr) {
      fail(
          "/composition/early",
          "requires the Main wet path to contain a Diffuser");
    }
    if (early.taps.empty()) {
      fail("/composition/early/taps", "expected at least one resolved tap");
    }
    if (!std::isfinite(early.decayDbPerSec) || early.decayDbPerSec < 0.0) {
      fail(
          "/composition/early/decayDbPerSec",
          "expected a finite value at least zero");
    }
    const dsp::ResolvedDiffuser noDiffuser;
    const auto& diffuserForSupport =
        diffuserStage != nullptr ? *diffuserStage : noDiffuser;
    for (std::size_t index = 0; index < early.taps.size(); ++index) {
      const auto& tap = early.taps[index];
      const auto tapPath =
          "/composition/early/taps/" + std::to_string(index);
      if (index > 0 && tap.stepIndex <= early.taps[index - 1].stepIndex) {
        fail(
            tapPath + "/stepIndex",
            "expected unique Diffusion Step indices sorted ascending");
      }
      if (diffuserStage != nullptr &&
          tap.stepIndex >= diffuserStage->steps.size()) {
        fail(
            tapPath + "/stepIndex",
            "expected a Diffusion Step index within [0, stepCount)");
      }
      if (!std::isfinite(tap.gainDb)) {
        fail(tapPath + "/gainDb", "expected a finite value");
      }
      const auto expectedTap = resolveEarlyTap(
          tap.stepIndex,
          tap.gainDb,
          diffuserForSupport,
          early.decayDbPerSec,
          resolved.sampleRate);
      if (tap.nominalSupportMinSamples !=
              expectedTap.nominalSupportMinSamples ||
          tap.nominalSupportMaxSamples !=
              expectedTap.nominalSupportMaxSamples ||
          tap.conservativeSupportMinSamples !=
              expectedTap.conservativeSupportMinSamples ||
          tap.conservativeSupportMaxSamples !=
              expectedTap.conservativeSupportMaxSamples) {
        fail(
            tapPath,
            "expected support bounds in samples derived from the "
            "resolved Diffuser");
      }
      if (!std::isfinite(tap.nominalSupportMinMs) ||
          !std::isfinite(tap.nominalSupportMaxMs) ||
          !std::isfinite(tap.conservativeSupportMinMs) ||
          !std::isfinite(tap.conservativeSupportMaxMs) ||
          tap.nominalSupportMinMs != expectedTap.nominalSupportMinMs ||
          tap.nominalSupportMaxMs != expectedTap.nominalSupportMaxMs ||
          tap.conservativeSupportMinMs !=
              expectedTap.conservativeSupportMinMs ||
          tap.conservativeSupportMaxMs !=
              expectedTap.conservativeSupportMaxMs) {
        fail(
            tapPath,
            "expected support bounds in milliseconds derived from the "
            "resolved Diffuser");
      }
      if (tap.shapingGainDb != expectedTap.shapingGainDb) {
        fail(
            tapPath + "/shapingGainDb",
            "expected gainDb minus decayDbPerSec times the nominal "
            "support end");
      }
      validateFloatRepresentableGain(tapPath + "/gain", tap.gain);
      if (tap.gain != expectedTap.gain) {
        fail(tapPath + "/gain", "expected gain derived from shapingGainDb");
      }
    }
    if (!std::isfinite(early.levelDb)) {
      fail("/composition/early/levelDb", "expected a finite value");
    }
    validateFloatRepresentableGain("/composition/early/gain", early.gain);
    if (early.gain != resolveLinearGainFromDb(early.levelDb)) {
      fail("/composition/early/gain", "expected gain derived from levelDb");
    }
    validateResolvedDownmixFields(
        resolved,
        early.downmix,
        channels,
        kEarlyDownmixRandomOrthogonalUsage,
        dsp::DownmixAlignment::aligned,
        "/composition/early/downmix");
  }
}

} // namespace

} // namespace rvrbotron::config
