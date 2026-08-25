#include "rvrbotron/config/ResolveConfig.h"

#include "rvrbotron/HarnessError.h"
#include "rvrbotron/config/PositionalRandom.h"

#include <algorithm>
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
  std::uint32_t stepCount = 1;
  double totalMs = 40.0;
  SampleBudget sampleBudget;
  bool deriveChannelValues = true;
};

struct ResolutionEvidence {
  std::optional<DiffuserDerivation> diffuser;
};

SampleBudget deriveSampleBudget(const double totalMs,
                                const std::uint32_t sampleRate,
                                const std::uint32_t channels) noexcept {
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
  if (samples + 1 < channels) {
    return {SampleBudgetStatus::tooFewPositions, samples};
  }
  return {SampleBudgetStatus::valid, samples};
}

std::optional<std::size_t> hadamardElementCount(
    const std::uint32_t channels) noexcept {
  if (channels == 0) {
    return std::nullopt;
  }
  const auto maxElements = std::vector<double>{}.max_size();
  if (channels > maxElements / channels) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(channels) * channels;
}

bool hasOddParity(std::uint32_t value) noexcept {
  bool odd = false;
  while (value != 0) {
    odd = !odd;
    value &= value - 1U;
  }
  return odd;
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

std::vector<double> makeHadamard(const std::uint32_t channels,
                                 const std::size_t elementCount) {
  const auto scale = 1.0 / std::sqrt(static_cast<double>(channels));
  std::vector<double> matrix(elementCount);
  for (std::uint32_t row = 0; row < channels; ++row) {
    for (std::uint32_t column = 0; column < channels; ++column) {
      matrix[static_cast<std::size_t>(row) * channels + column] =
          hasOddParity(row & column) ? -scale : scale;
    }
  }
  return matrix;
}

dsp::ResolvedSplit resolveSplit(const SplitConfig& requested,
                                const std::uint32_t inputChannels) {
  const auto channels = requested.channels.value_or(8);
  const auto strategy =
      requested.strategy.value_or(dsp::SplitStrategyType::duplicate);
  const auto normalisation = requested.normalisation.value_or(
      dsp::EnergyNormalisation::energy);
  const auto sourceGain =
      inputChannels == 1
          ? 1.0
          : inputChannels == 2
                ? 1.0 / std::sqrt(2.0)
                : 0.0;
  const auto channelGain =
      normalisation == dsp::EnergyNormalisation::energy
          ? channels == 0
                ? 0.0
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
  const auto stepSettings =
      requested.step.value_or(DiffusionStepConfig{});
  const auto delayStrategy = stepSettings.delayStrategy.value_or(
      dsp::DelayStrategy::segmentedRandom);
  const auto mix =
      stepSettings.mix.value_or(dsp::MixMatrixType::hadamard);
  const auto shuffle = stepSettings.shuffle.value_or(true);
  const auto polarity = stepSettings.polarity.value_or(
      dsp::PolarityStrategy::seededRandom);

  dsp::ResolvedDiffuser diffuser;
  diffuser.totalSamples = derivation.sampleBudget.samples;
  if (derivation.stepCount != 1) {
    return diffuser;
  }

  dsp::ResolvedDiffusionStep step;
  step.index = 0;
  step.lengthSamples = derivation.sampleBudget.samples;
  step.lengthMs =
      sampleRate == 0
          ? 0.0
          : static_cast<double>(step.lengthSamples) * 1000.0 /
                sampleRate;
  step.delayStrategy = delayStrategy;
  step.shuffle = shuffle;
  step.polarity = polarity;
  step.mix = mix;

  const auto matrixElements = hadamardElementCount(channels);
  const auto supportedDelayStrategy =
      delayStrategy == dsp::DelayStrategy::segmentedRandom ||
      delayStrategy == dsp::DelayStrategy::even;
  const auto supportedPolarity =
      polarity == dsp::PolarityStrategy::seededRandom ||
      polarity == dsp::PolarityStrategy::none;
  const auto canDeriveChannelValues =
      derivation.deriveChannelValues &&
      derivation.sampleBudget.status == SampleBudgetStatus::valid &&
      isPowerOfTwo(channels) &&
      mix == dsp::MixMatrixType::hadamard &&
      supportedDelayStrategy &&
      supportedPolarity &&
      matrixElements.has_value();
  if (!canDeriveChannelValues) {
    diffuser.steps.push_back(std::move(step));
    return diffuser;
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
    } else {
      delay = evenDelay(step.lengthSamples, channels, channel);
    }
    step.delaysSamples.push_back(delay);
    step.delaysMs.push_back(
        static_cast<double>(delay) * 1000.0 / sampleRate);
    step.bufferSizes.push_back(delay);
  }

  step.permutation.resize(channels);
  std::iota(step.permutation.begin(), step.permutation.end(), 0U);
  if (shuffle) {
    for (std::uint32_t index = channels - 1U; index > 0; --index) {
      const auto selected = static_cast<std::uint32_t>(boundedRandom(
          seed,
          kDiffusionShuffleUsage,
          step.index,
          index,
          static_cast<std::uint64_t>(index) + 1));
      std::swap(step.permutation[index], step.permutation[selected]);
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
  step.matrix = makeHadamard(channels, *matrixElements);

  diffuser.steps.push_back(std::move(step));
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
    const ResolutionEvidence* resolutionEvidence);

} // namespace

dsp::ResolvedConfig resolveConfig(const ReverbConfig& requested,
                                  const std::uint32_t sampleRate,
                                  const std::uint32_t inputChannels) {
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
              derivation.stepCount = stageConfig.steps.value_or(1);
              derivation.totalMs = stageConfig.totalMs.value_or(40.0);
              derivation.sampleBudget = deriveSampleBudget(
                  derivation.totalMs, sampleRate, channels);
              derivation.deriveChannelValues = deriveChannelValues;
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

  validateResolvedConfig(resolved, &resolutionEvidence);
  return resolved;
}

void validateResolvedConfig(const dsp::ResolvedConfig& resolved) {
  validateResolvedConfig(resolved, nullptr);
}

namespace {

void validateResolvedConfig(
    const dsp::ResolvedConfig& resolved,
    const ResolutionEvidence* const resolutionEvidence) {
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
  const auto mixPath =
      validatingRequest
          ? "/composition/stages/1/step/mix"
          : "/composition/stages/1/steps/0/mix";
  if (split.inputChannels == 0 || split.inputChannels > 2) {
    fail(
        "/composition/stages/0/inputChannels",
        "expected one or two input Channels");
  }
  if (channels == 0) {
    fail("/composition/stages/0/channels", "expected value greater than zero");
  }
  if (split.strategy != dsp::SplitStrategyType::duplicate) {
    fail(
        "/composition/stages/0/strategy",
        "the first diffusion slice requires duplicate");
  }
  if (!(split.sourceGain > 0.0) || !std::isfinite(split.sourceGain)) {
    fail(
        "/composition/stages/0/sourceGain",
        "expected finite positive gain");
  }
  const auto expectedSourceGain =
      split.inputChannels == 1 ? 1.0 : 1.0 / std::sqrt(2.0);
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
        1.0 / std::sqrt(static_cast<double>(channels));
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
      resolutionEvidence->diffuser->stepCount != 1) {
    fail(
        "/composition/stages/1/steps",
        "the first diffusion slice requires exactly one step");
  }
  if (diffuser.steps.size() != 1) {
    fail(
        "/composition/stages/1/steps",
        "the first diffusion slice requires exactly one step");
  }
  const auto& step = diffuser.steps.front();
  if (step.index != 0) {
    fail("/composition/stages/1/steps/0/index", "expected zero");
  }
  if (validatingRequest) {
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
  }
  if (step.lengthSamples != diffuser.totalSamples ||
      step.lengthSamples == 0) {
    fail(
        "/composition/stages/1/steps/0/lengthSamples",
        "expected the complete nonzero Diffuser sample budget");
  }
  if (step.lengthSamples == std::numeric_limits<std::uint64_t>::max() ||
      step.lengthSamples + 1 < channels) {
    fail(
        "/composition/stages/1/steps/0/lengthSamples",
        "delay strategy requires at least one sample position per Channel");
  }
  const auto expectedLengthMs =
      static_cast<double>(step.lengthSamples) * 1000.0 /
      resolved.sampleRate;
  if (step.lengthMs != expectedLengthMs) {
    fail(
        "/composition/stages/1/steps/0/lengthMs",
        "expected milliseconds derived from the integer sample budget");
  }
  if (step.delayStrategy != dsp::DelayStrategy::segmentedRandom &&
      step.delayStrategy != dsp::DelayStrategy::even) {
    fail(
        "/composition/stages/1/steps/0/delayStrategy",
        "expected segmented-random or even");
  }
  if (step.polarity != dsp::PolarityStrategy::seededRandom &&
      step.polarity != dsp::PolarityStrategy::none) {
    fail(
        "/composition/stages/1/steps/0/polarity",
        "expected seeded-random or none");
  }
  if (step.mix != dsp::MixMatrixType::hadamard) {
    fail(
        mixPath,
        "the first diffusion slice requires hadamard");
  }
  if (!isPowerOfTwo(channels)) {
    fail(
        mixPath,
        "hadamard requires a power-of-two Channel count");
  }
  const auto matrixElements = hadamardElementCount(channels);
  if (!matrixElements.has_value()) {
    fail(mixPath, "Hadamard matrix is too large");
  }
  const auto requireChannelValues =
      [channels](const std::size_t size,
                 const std::string_view path) {
        if (size != channels) {
          fail(path, "expected one value per Channel");
        }
      };
  requireChannelValues(
      step.delaysSamples.size(),
      "/composition/stages/1/steps/0/delaysSamples");
  requireChannelValues(
      step.delaysMs.size(),
      "/composition/stages/1/steps/0/delaysMs");
  requireChannelValues(
      step.bufferSizes.size(),
      "/composition/stages/1/steps/0/bufferSizes");
  requireChannelValues(
      step.permutation.size(),
      "/composition/stages/1/steps/0/permutation");
  requireChannelValues(
      step.polaritySigns.size(),
      "/composition/stages/1/steps/0/polaritySigns");

  auto sortedDelays = step.delaysSamples;
  std::sort(sortedDelays.begin(), sortedDelays.end());
  if (std::adjacent_find(sortedDelays.begin(), sortedDelays.end()) !=
      sortedDelays.end()) {
    fail(
        "/composition/stages/1/steps/0/delaysSamples",
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
            "/composition/stages/1/steps/0/delaysSamples",
            "segmented-random requires one delay in every segment");
      }
    }
  }
  std::vector<bool> seenPermutation(channels, false);
  for (std::uint32_t channel = 0; channel < channels; ++channel) {
    const auto delay = step.delaysSamples[channel];
    if (delay > step.lengthSamples) {
      fail(
          "/composition/stages/1/steps/0/delaysSamples",
          "expected distinct delays within the step sample budget");
    }
    const auto expectedDelayMs =
        static_cast<double>(delay) * 1000.0 / resolved.sampleRate;
    if (step.delaysMs[channel] != expectedDelayMs) {
      fail(
          "/composition/stages/1/steps/0/delaysMs",
          "expected milliseconds derived from integer delays");
    }
    if (step.bufferSizes[channel] != delay) {
      fail(
          "/composition/stages/1/steps/0/bufferSizes",
          "expected each buffer size to equal its integer delay");
    }
    const auto source = step.permutation[channel];
    if (source >= channels || seenPermutation[source]) {
      fail(
          "/composition/stages/1/steps/0/permutation",
          "expected a permutation of Channel indices");
    }
    if (!step.shuffle && source != channel) {
      fail(
          "/composition/stages/1/steps/0/permutation",
          "shuffle false requires the identity permutation");
    }
    seenPermutation[source] = true;
    if (step.polaritySigns[channel] != -1 &&
        step.polaritySigns[channel] != 1) {
      fail(
          "/composition/stages/1/steps/0/polaritySigns",
          "expected -1 or 1");
    }
    if (step.polarity == dsp::PolarityStrategy::none &&
        step.polaritySigns[channel] != 1) {
      fail(
          "/composition/stages/1/steps/0/polaritySigns",
          "polarity none requires all +1 signs");
    }
    if (step.delayStrategy == dsp::DelayStrategy::even &&
        delay != evenDelay(step.lengthSamples, channels, channel)) {
      fail(
          "/composition/stages/1/steps/0/delaysSamples",
          "even requires delays distributed over the available positions");
    }
  }
  if (step.matrix.size() != *matrixElements) {
    fail(
        "/composition/stages/1/steps/0/matrix",
        "expected an N by N matrix");
  }
  const auto expectedScale =
      1.0 / std::sqrt(static_cast<double>(channels));
  const auto resolvedScale = step.matrix.front();
  const auto scaleTolerance =
      4.0 * std::numeric_limits<double>::epsilon() * expectedScale;
  if (!std::isfinite(resolvedScale) || resolvedScale <= 0.0 ||
      std::abs(resolvedScale - expectedScale) > scaleTolerance) {
    fail(
        "/composition/stages/1/steps/0/matrix",
        "expected the normalized canonical Sylvester-Hadamard matrix");
  }
  for (std::uint32_t row = 0; row < channels; ++row) {
    for (std::uint32_t column = 0; column < channels; ++column) {
      const auto expected =
          hasOddParity(row & column) ? -resolvedScale : resolvedScale;
      if (step.matrix[
              static_cast<std::size_t>(row) * channels + column] !=
          expected) {
        fail(
            "/composition/stages/1/steps/0/matrix",
            "expected the normalized canonical Sylvester-Hadamard matrix");
      }
    }
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
