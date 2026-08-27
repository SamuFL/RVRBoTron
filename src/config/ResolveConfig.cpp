#include "rvrbotron/config/ResolveConfig.h"

#include "rvrbotron/HarnessError.h"
#include "rvrbotron/config/MixMatrixResolution.h"
#include "rvrbotron/config/PositionalRandom.h"

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

[[noreturn]] void fail(const std::string_view path,
                       const std::string_view reason) {
  throw HarnessError(
      ErrorCategory::invalidConfiguration,
      std::string(reason),
      std::string(path));
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
  // Per-step sample lengths apportioned from the total sample budget by
  // largest remainder; empty when the plan could not be computed (for
  // example an unsupported distribution or non-finite weights).
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

// Only segmented-random and even require N distinct sample positions:
// uniform-random deliberately samples with replacement, so it only needs at
// least one position to draw from (always true once the sample budget
// itself is valid).
bool requiresDistinctChannelPositions(
    const dsp::DelayStrategy strategy) noexcept {
  return strategy == dsp::DelayStrategy::segmentedRandom ||
         strategy == dsp::DelayStrategy::even;
}

// Segmented-random and even delay strategies each require one distinct
// integer sample position per Channel within a step's own sample budget.
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

// Conservative (worst-case double-precision Sample) estimate of the
// DSP-owned bytes a resolved Diffuser will occupy: one delay line per
// Channel sized to the shared sample budget, plus a full NxN matrix and
// small per-Channel metadata for every step.
std::optional<std::uint64_t> estimateDiffuserMemoryBytes(
    const std::uint32_t channels,
    const std::uint64_t totalSamples,
    const std::uint64_t stepCount) noexcept {
  constexpr std::uint64_t kSampleBytes = 8;
  constexpr std::uint64_t kMetadataBytesPerChannel = 24;
  auto delayBytes = checkedMul(channels, totalSamples);
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
      !metadataBytes.has_value()) {
    return std::nullopt;
  }
  auto total = checkedAdd(*delayBytes, *matrixBytes);
  total = total.has_value() ? checkedAdd(*total, *metadataBytes)
                             : std::nullopt;
  return total;
}

void checkDiffuserMemoryBudget(
    const std::uint32_t channels,
    const std::uint64_t totalSamples,
    const std::uint64_t stepCount,
    const std::uint64_t budgetBytes,
    const std::string_view path) {
  const auto estimate =
      estimateDiffuserMemoryBytes(channels, totalSamples, stepCount);
  if (!estimate.has_value() || *estimate > budgetBytes) {
    fail(
        path,
        "resolved Diffuser DSP memory footprint exceeds the configured "
        "memory budget");
  }
}

// Apportions `totalSamples` across `weights.size()` steps by largest
// remainder: each step first receives the floor of its ideal (weighted)
// share, then the leftover samples are distributed one at a time to the
// steps with the largest fractional remainder, using ascending step index
// to break ties. The result always sums exactly to `totalSamples`.
//
// Precondition: `weights` sums to a finite, strictly positive value (see
// `diffuserStepWeights`, this function's only caller, which enforces that
// before ever reaching here). A non-finite or non-positive sum would make
// every step's ideal share divide out to zero, leaving `remaining` far
// larger than `stepCount` and reading past the end of `order` below.
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

// Computes the relative per-step weight used for largest-remainder
// apportionment: explicit `lengthsMs` values when present, otherwise an
// even split or a doubling (2^index) split of the shared total. Returns
// nullopt if any individual weight, or their sum, is not a finite positive
// value — for example an unreasonably large doubling step count, or
// `lengthsMs` entries large enough that their sum overflows. Validating the
// sum here (not just each weight) matters because `apportionStepSamples`
// divides by it: a non-finite or non-positive sum would make every step's
// share round down to zero, leaving far more "remaining" samples than
// there are steps to distribute them to.
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
    const auto value = positionalSplitMix64V1(
        seed, usage, itemIndex, valueIndex, drawIndex);
    if (value >= threshold) {
      return value % bound;
    }
    if (drawIndex == std::numeric_limits<std::uint64_t>::max()) {
      fail("/seed", "positional random rejection sampling did not converge");
    }
  }
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
  // Stereo-preserving strategies only diverge from duplicate's mono-summed
  // mapping when the source itself is stereo; mono input always resolves to
  // the same mono duplication mapping regardless of the requested strategy.
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
  // A matrix of a given type is deterministic for a given Channel count, so
  // it is shared across every step that resolves to that type (computed
  // once per type) even though it is fully serialized per step. Indexed by
  // the MixMatrixType enum's underlying value.
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

    step.permutation.resize(channels);
    std::iota(step.permutation.begin(), step.permutation.end(), 0U);
    if (shuffle) {
      for (std::uint32_t position = channels - 1U; position > 0;
           --position) {
        const auto selected = static_cast<std::uint32_t>(boundedRandom(
            seed,
            kDiffusionShuffleUsage,
            step.index,
            position,
            static_cast<std::uint64_t>(position) + 1));
        std::swap(step.permutation[position], step.permutation[selected]);
      }
    }

    step.polaritySigns.reserve(channels);
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
      if (polarity == dsp::PolarityStrategy::none) {
        step.polaritySigns.push_back(1);
      } else {
        const auto value = positionalSplitMix64V1(
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
        auto randomOrthogonal = resolveRandomOrthogonalMatrix(channels, seed);
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

    diffuser.steps.push_back(std::move(step));
  }
  return diffuser;
}

dsp::ResolvedDownmix resolveDownmix(
    const DownmixConfig& requested,
    const std::uint32_t channels) {
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
  return {
      channels,
      2,
      strategy,
      normalisation,
      compensation,
  };
}

void validateShape(const dsp::ResolvedComposition& composition) {
  if (composition.stages.empty()) {
    return;
  }
  if (composition.stages.size() != 3 ||
      !std::holds_alternative<dsp::ResolvedSplit>(
          composition.stages[0]) ||
      !std::holds_alternative<dsp::ResolvedDiffuser>(
          composition.stages[1]) ||
      !std::holds_alternative<dsp::ResolvedDownmix>(
          composition.stages[2])) {
    fail(
        "/composition/stages",
        "expected [split, diffuser, downmix]");
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
      requested.formatVersion.value_or(1),
      requested.seed.value_or(0),
      sampleRate,
      {},
  };

  const auto* requestedComposition =
      requested.composition.has_value() ? &*requested.composition : nullptr;
  ResolutionEvidence resolutionEvidence;
  if (requestedComposition != nullptr &&
      !requestedComposition->stages.empty()) {
    const auto canonicalShape =
        requestedComposition->stages.size() == 3 &&
        std::holds_alternative<SplitConfig>(
            requestedComposition->stages[0]) &&
        std::holds_alternative<DiffuserConfig>(
            requestedComposition->stages[1]) &&
        std::holds_alternative<DownmixConfig>(
            requestedComposition->stages[2]);
    const auto deriveChannelValues =
        resolved.formatVersion == 1 &&
        sampleRate != 0 &&
        inputChannels > 0 &&
        inputChannels <= 2 &&
        canonicalShape;
    std::uint32_t channels = 0;
    for (const auto& stage : requestedComposition->stages) {
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
            } else {
              resolved.composition.stages.emplace_back(
                  resolveDownmix(stageConfig, channels));
            }
          },
          stage);
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

void validateResolvedConfig(
    const dsp::ResolvedConfig& resolved,
    const ResolutionEvidence* const resolutionEvidence,
    const std::uint64_t memoryBudgetBytes) {
  if (resolved.formatVersion != 1) {
    fail("/formatVersion", "expected integer 1");
  }
  if (resolved.sampleRate == 0) {
    fail("/sampleRate", "expected value greater than zero");
  }
  validateShape(resolved.composition);
  if (resolved.composition.stages.empty()) {
    return;
  }

  const auto& split =
      std::get<dsp::ResolvedSplit>(resolved.composition.stages[0]);
  const auto& diffuser =
      std::get<dsp::ResolvedDiffuser>(resolved.composition.stages[1]);
  const auto& downmix =
      std::get<dsp::ResolvedDownmix>(resolved.composition.stages[2]);
  const auto channels = split.channels;
  const auto validatingRequest =
      resolutionEvidence != nullptr &&
      resolutionEvidence->diffuser.has_value();
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
  if (validatingRequest &&
      resolutionEvidence->diffuser->expectedStepLengths.size() !=
          resolutionEvidence->diffuser->stepCount) {
    switch (resolutionEvidence->diffuser->sampleBudget.status) {
    case SampleBudgetStatus::valid:
      break;
    case SampleBudgetStatus::nonPositive:
      fail(
          "/composition/stages/1/totalMs",
          "expected value greater than zero");
    case SampleBudgetStatus::tooLarge:
      fail(
          "/composition/stages/1/totalMs",
          "resolved sample budget is too large");
    case SampleBudgetStatus::belowOneSample:
      fail(
          "/composition/stages/1/totalMs",
          "resolved sample budget must be at least one sample");
    case SampleBudgetStatus::tooFewPositions:
      fail(
          "/composition/stages/1/totalMs",
          "delay strategy requires at least one sample position per Channel");
    }
    fail(
        "/composition/stages/1/steps",
        "expected a valid step count and distribution");
  }
  if (diffuser.steps.empty()) {
    fail("/composition/stages/1/steps", "expected at least one step");
  }
  if (diffuser.totalSamples == 0) {
    fail(
        "/composition/stages/1/totalSamples",
        "expected value greater than zero");
  }
  checkDiffuserMemoryBudget(
      channels,
      diffuser.totalSamples,
      diffuser.steps.size(),
      memoryBudgetBytes,
      "/composition/stages/1");
  if (validatingRequest &&
      diffuser.steps.size() != resolutionEvidence->diffuser->stepCount) {
    fail(
        "/composition/stages/1/steps",
        "expected one resolved step per requested step");
  }
  const auto matrixElements = matrixElementCount(channels);
  std::uint64_t stepLengthSum = 0;
  for (std::size_t stepPosition = 0; stepPosition < diffuser.steps.size();
       ++stepPosition) {
    const auto& step = diffuser.steps[stepPosition];
    const auto stepPath =
        "/composition/stages/1/steps/" + std::to_string(stepPosition);
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

    auto sortedDelays = step.delaysSamples;
    std::sort(sortedDelays.begin(), sortedDelays.end());
    // Uniform-random deliberately samples with replacement, so clumping and
    // collisions are a permitted ablation rather than a validation failure.
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
      if (step.bufferSizes[channel] != delay) {
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
    switch (step.mix) {
    case dsp::MixMatrixType::hadamard: {
      const auto canonical = resolveHadamardMatrix(channels);
      const auto tolerance =
          4.0 * std::numeric_limits<double>::epsilon() * canonical.front();
      for (std::size_t element = 0; element < canonical.size(); ++element) {
        const auto actual = step.matrix[element];
        if (!std::isfinite(actual) ||
            std::abs(actual - canonical[element]) > tolerance) {
          fail(
              stepPath + "/matrix",
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
        const auto actual = step.matrix[element];
        if (!std::isfinite(actual) ||
            std::abs(actual - canonical[element]) > tolerance) {
          fail(
              stepPath + "/matrix",
              "expected the normalized canonical Householder matrix");
        }
      }
      break;
    }
    case dsp::MixMatrixType::randomOrthogonal: {
      // Property-checked from the resolved coefficients themselves
      // (MM^T = I) rather than by recomputing and trusting the seeded
      // construction that produced them -- a hand-authored ablation may
      // substitute any valid orthogonal matrix.
      constexpr double kOrthogonalityTolerance = 1e-9;
      for (std::uint32_t rowA = 0; rowA < channels; ++rowA) {
        for (std::uint32_t rowB = 0; rowB < channels; ++rowB) {
          double dot = 0.0;
          for (std::uint32_t column = 0; column < channels; ++column) {
            dot +=
                step.matrix[
                    static_cast<std::size_t>(rowA) * channels + column] *
                step.matrix[
                    static_cast<std::size_t>(rowB) * channels + column];
          }
          const auto expected = rowA == rowB ? 1.0 : 0.0;
          if (!std::isfinite(dot) ||
              std::abs(dot - expected) > kOrthogonalityTolerance) {
            fail(
                stepPath + "/matrix",
                "expected an orthogonal resolved matrix (M M^T = I)");
          }
        }
      }
      break;
    }
    }
  }
  if (stepLengthSum != diffuser.totalSamples) {
    fail(
        "/composition/stages/1/steps",
        "expected step sample budgets to sum to the resolved total");
  }
  if (downmix.inputChannels != channels ||
      downmix.outputChannels != 2) {
    fail(
        "/composition/stages/2",
        "Downmix dimensions must map the internal Channels to stereo");
  }
  if (downmix.strategy != dsp::DownmixStrategy::select) {
    fail(
        "/composition/stages/2/strategy",
        "the first diffusion slice requires select");
  }
  if (!(downmix.compensation > 0.0) ||
      !std::isfinite(downmix.compensation)) {
    fail(
        "/composition/stages/2/compensation",
        "expected finite positive gain");
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
    fail(
        "/composition/stages/2/normalisation",
        "expected energy or none");
  }
  if (downmix.compensation != expectedCompensation) {
    fail(
        "/composition/stages/2/compensation",
        "expected gain derived from Downmix normalisation");
  }
}

} // namespace

} // namespace rvrbotron::config
