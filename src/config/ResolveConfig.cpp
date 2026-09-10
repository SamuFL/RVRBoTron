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
// Feedback Loop delays are derived from (seed, Channel index) alone -- there
// is one loop, not a chain of indexed steps -- so this usage tag's itemIndex
// argument is always the fixed constant 0 (see resolveFeedbackLoop).
constexpr std::uint64_t kFeedbackLoopDelayUsage = 0x464c4f4f5044454cULL;
// Per-Channel Modulation trajectory seeds are derived from (seed,
// itemIndex, Channel index): itemIndex is the fixed constant 0 for the
// Feedback Loop, mirroring kFeedbackLoopDelayUsage's own convention above
// (see resolveFeedbackLoop). Unchanged from issue #89/#90 -- existing
// resolved.json files and their rendered audio must not shift.
constexpr std::uint64_t kFeedbackLoopModulationSeedUsage =
    0x4d4f44554c534544ULL;
// The Modulation channel-selection permutation (issue #90's
// channelFraction) gets its own usage tag, per the design doc's "Phase,
// rate spread and Channel selection each get their own usage tag" --
// distinct from every other Modulation draw above and from delay
// derivation, so selection never correlates with delay ordering.
// Unchanged from issue #90, for the same reason as the seed tag above.
constexpr std::uint64_t kFeedbackLoopModulationChannelSelectionUsage =
    0x4d4f44434853454cULL;
// A Diffusion Step's own Modulation seed and channel-selection tags
// (issue #91): distinct constants from the Feedback Loop's own above --
// not the same tag with a repurposed itemIndex -- mirroring how
// kDiffusionDelayUsage is already a separate tag from
// kFeedbackLoopDelayUsage for delay derivation. This is what lets a
// Diffusion Step's itemIndex be its own plain step index: a Diffusion
// Step and the Feedback Loop can both resolve itemIndex 0 without ever
// drawing the same value, since they read from different usage-tag
// domains entirely (see rvrbotron::config::ModulationOwner in
// ModulationResolution.h, which selects between these two domains for
// every other per-Channel Modulation draw too).
constexpr std::uint64_t kDiffusionModulationSeedUsage =
    0x44535445504d5344ULL;
constexpr std::uint64_t kDiffusionModulationChannelSelectionUsage =
    0x44535445504d4353ULL;
// The Main Downmix's own domain-separated RandomOrthogonal usage tag
// ("MAINDNMX", ADR-0002, issue #108) -- distinct from
// kMixMatrixRandomOrthogonalUsage ("MIXORTHO"), which the Diffuser and
// Feedback Loop's own `random-orthogonal` mix continues to use unchanged,
// so the two domains never draw the same value for the same (seed,
// row/column). The future Early Reflections Downmix gets its own
// "EARLDNMX" tag rather than reusing this one.
constexpr std::uint64_t kMainDownmixRandomOrthogonalUsage =
    0x4d41494e444e4d58ULL;
// The Early Downmix's own domain-separated RandomOrthogonal usage tag
// ("EARLDNMX", issue #111, ADR-0002): distinct from both
// kMainDownmixRandomOrthogonalUsage and kMixMatrixRandomOrthogonalUsage, so
// Early's Downmix never draws the same value as Main's for the same
// (seed, row/column) -- see docs/design/reverb/stages/09-composition.md's
// seeding table.
constexpr std::uint64_t kEarlyDownmixRandomOrthogonalUsage =
    0x4541524c444e4d58ULL;

[[noreturn]] void fail(const std::string_view path,
                       const std::string_view reason) {
  throw HarnessError(
      ErrorCategory::invalidConfiguration,
      std::string(reason),
      std::string(path));
}

// A Diffuser and a Feedback Loop are each positionally independent: neither
// hardcodes its own stage index, since the Feedback Loop lands at index 1
// when it is the sole middle stage but index 2 when it follows a Diffuser
// (see docs/design/reverb/stages/09-composition.md's valid Composition
// shapes).
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
// Channel sized to `maxBufferSamples`, plus a full NxN matrix and small
// per-Channel metadata for every step, plus `modulationBytes` for every
// active Diffusion Step Modulation's own owned per-Channel vectors
// (channelSeeds/channelTargetsPerSample/channelPhases/channelModulated;
// see issue #91) -- nullopt (from estimateDiffuserModulationBytes's own
// overflow) propagates as an unrepresentable, and therefore rejected,
// estimate, exactly like every other overflow below. `maxBufferSamples`
// is the nominal shared sample budget before any step is resolved (the
// caller has nothing better yet), or the largest actually-resolved
// per-Channel buffer size across every step once resolved -- Diffusion
// Step Modulation's Excursion and Interpolation margin can grow a
// Channel's buffer past the nominal budget (see docs/design/reverb/
// stages/06-modulation.md's "Delay buffers need headroom"), so a
// post-resolution caller must pass whichever of the two is larger to
// keep this estimate conservative (mirrors
// estimateFeedbackLoopMemoryBytes's own `max(delayMaxSamples,
// maxResolvedBufferSamples)` pattern).
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

// Conservative per-Channel byte estimate for one active Modulation
// object's own owned vectors (see dsp::Modulation::ownedBytes(), which
// this deliberately over-approximates rather than imports exactly, to
// keep this estimator's own arithmetic simple and self-contained):
// channelSeeds (uint64_t), channelTargetsPerSample and channelPhases
// (double each), and channelModulated (rounded up to a full byte per
// Channel even though std::vector<bool> packs bits).
constexpr std::uint64_t kModulationBytesPerChannel = 25;

// Sums `kModulationBytesPerChannel * channels` for every Diffusion Step
// whose own Modulation actually moved at least one Channel (empty
// `channelModulated` is the resolved bypass -- no vectors allocated; see
// docs/design/reverb/stages/06-modulation.md's "Identity is guaranteed by
// construction, not by arithmetic"). Returns nullopt on overflow, treated
// by `checkDiffuserMemoryBudget`'s caller the same as any other
// unrepresentable estimate -- conservatively rejected rather than
// silently underestimated.
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

// Conservative estimate of the DSP-owned bytes a resolved Feedback Loop
// will occupy: one delay line per Channel sized to the longest resolved
// delay, plus a full NxN mixing matrix and small per-Channel metadata.
// There is no step multiplier -- one loop, not a chain of steps.
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
// value -- for example an unreasonably large doubling step count, or
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

// The Modulation per-Channel bypass mask (issue #90's channelFraction,
// see docs/design/reverb/stages/06-modulation.md's "Trajectories are
// seeded positionally"): the first ceil(channelFraction * channels)
// entries of a fixed permutation, seeded independently of delay
// derivation and of `channelFraction` itself, so raising
// `channelFraction` only lengthens the prefix taken from the *same*
// permutation -- never reshuffling a Channel that was already
// modulating -- and selection never correlates with delay ordering.
// `channelFraction` must already be finite and in [0, 1]; `channels`
// must be > 0. `owner` selects between the Feedback Loop's and a
// Diffusion Step's own separate usage-tag domain (see ModulationOwner in
// ModulationResolution.h); `itemIndex` is 0 for the Feedback Loop (one
// loop, not a chain of steps) and the step index for a Diffusion Step
// (issue #91), so two modulated steps never select the same Channels
// from the same permutation.
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
  // ceil(channelFraction * channels), with a small absolute tolerance
  // subtracted first: multiplying by an integer can round the exact
  // product a few ULPs above the intended integer (0.14 * 100 ==
  // 14.000000000000002 in binary64), which would otherwise ceil to one
  // Channel more than documented (PR #98 review). The tolerance is far
  // too small to swallow any fractional intent a human would actually
  // type. `std::max(1, ...)` keeps any non-zero fraction modulating at
  // least one Channel even for a channelFraction small enough that the
  // tolerance would otherwise round it below one; channelFraction ==
  // 1.0 still modulates exactly `channels`.
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

// Resolves one Modulation object: shared by the Feedback Loop and each
// Diffusion Step (see docs/design/reverb/stages/06-modulation.md, issues
// #89 and #91) so seeds, rates, phases, channel selection, the Excursion
// rejection rule, and buffer headroom can never drift between the two
// modulated stages. `owner` selects the caller's own usage-tag domain
// (see ModulationOwner in ModulationResolution.h); `itemIndex` is the
// positional-seeding item index for every per-Channel draw below -- 0 for
// the Feedback Loop (one loop, not a chain of steps) and the step index
// for a Diffusion Step, so two modulated steps never share a trajectory.
// `delaysSamples` is that stage's own already-resolved per-Channel delay;
// `bufferSizes` is mutated in place, gaining headroom only for the
// Channels actually modulated -- every other Channel's buffer size is
// left exactly as the caller passed it in. `rejectionPath` names the
// responsible parameter (that stage's own `.../modulation/depthMs`) if
// the Excursion rejection rule fires.
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

  // depthMs of 0 and channelFraction of 0 are both resolved bypasses (see
  // docs/design/reverb/stages/06-modulation.md's "Identity is guaranteed
  // by construction, not by arithmetic"): no seeds, no rates, no phases,
  // no bypass mask, no buffer headroom, so an explicit zero of either one
  // and an omitted Modulation object leave every other field identical.
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

  // The Excursion rejection rule applies to modulated Channels only: a
  // Channel channelFraction excludes never moves, so it can never
  // overrun regardless of how short its delay is.
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

  // Delay buffers reserve Excursion plus the fixed Interpolation margin,
  // added only to the Channels channelFraction actually selected -- an
  // unmodulated Channel, or every Channel when Modulation is absent or
  // fully bypassed, keeps its buffer size exactly equal to its delay, as
  // today.
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

// The additional finite-response reach Diffusion Step Modulation adds to
// the Diffuser's own resolved totalSamples (PR review on #112): beyond a
// step's own nominal length, a modulated Channel's read can still
// reference live content up to that step's own resolved Excursion
// (rounded up) plus the fixed Interpolation margin further out --
// resolveModulationHeadroomSamples, the same reach already used to size
// that Channel's own delay-line buffer (issue #91's "Delay buffers need
// headroom"), applied here to the Diffuser's own overall drain length
// instead. Summed once per actively modulated step, regardless of
// Channel count, so the Diffuser's own drain -- and therefore Early
// Reflections' conservative Tap support, which is bounded only by that
// same drain -- never truncates real energy. Shared by resolveDiffuser
// and validateDiffuserStage so the two never drift on the formula.
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
    // Modulation follows the same override-wins-over-defaults precedence
    // as every field above, but -- unlike them -- presence itself is the
    // decision: omitted from both the override and the shared defaults
    // means Modulation stays disabled for this step (see docs/design/
    // reverb/stages/06-modulation.md's "Placement" and issue #91).
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

// The Tail budget: an RT60 (either rt60Sec alone, or -- with boosted
// Damping, #77 -- the slower of rt60Sec, each boosted shelf's own
// conservative feedback-decay estimate, and its state-settling time)
// multiplied by decayMargin, rounded up to frames. Zero when any input is
// non-finite/non-positive, or the exact product doesn't fit a uint64_t.
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
  // 2^64, not uint64_t::max(), as the exclusive upper bound: uint64_t::max()
  // (2^64 - 1) is not exactly representable in a `long double` narrower than
  // 64 mantissa bits -- as on this codebase's macOS/ARM64 build, where
  // `long double` is `double` -- so comparing against it directly rejects
  // (or, with the wrong relational operator, admits) the wrong boundary
  // depending on platform. 2^64 is an exact power of two and therefore
  // exactly representable at any precision, making this comparison correct
  // on every supported platform: ceiledSamples strictly below it always
  // fits in a uint64_t.
  if (ceiledSamples >= 0x1p64L) {
    return 0;
  }
  return static_cast<std::uint64_t>(ceiledSamples);
}

// A Feedback Loop has one delay/gain/mix per Channel rather than a chain of
// indexed steps: delays derive positionally from (seed, Channel index) per
// ADR-0002, decay gain is solved per `gainMode` (#56) -- `perChannel` from
// that Channel's own loop time so every Channel decays at the requested
// rate regardless of delay spread, `uniform` from one shared gain solved
// from the mean loop time instead (see docs/design/reverb/stages/
// 04-feedback-loop.md) -- and the resolved Tail budget is an upper bound
// derived from rt60Sec and decayMargin -- never zero once
// rt60Sec/decayMargin/sampleRate are all valid, independent of whether
// per-Channel delay/gain/matrix derivation itself succeeds.
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

  // Checked only after delayMinSamples/delayMaxSamples are already set
  // above -- consistent with how the Diffuser always resolves
  // lengthSamples before its own mix-validity check ever runs -- so a
  // Hadamard request at a non-power-of-two Channel count fails validation
  // on its own specific check rather than on an unrelated
  // "delayMinSamples/delayMaxSamples derived from ms" mismatch.
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
    // One shared gain solved from the mean loop time across Channels
    // (docs/design/reverb/stages/04-feedback-loop.md's "Solving RT60 into
    // gain"): the mean is deliberate rather than, say, the extremes, so
    // the resulting per-Channel RT60 error is symmetric around the
    // requested value rather than biased toward one end of the spread.
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

    // Per-Channel shelf gains, coefficients, and expected decay are only
    // solvable once every Channel's own decay gain is known (see
    // resolveShelfGain); finiteness/range validation of the four requested
    // fields runs regardless, in validateFeedbackLoopStage below.
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
      // Only a boosted shelf (ratio > 1.0) can push decay slower than
      // rt60Sec; an attenuating or bypassed shelf never lengthens it, so
      // undamped and attenuation-only Tail budgets stay exactly rt60Sec *
      // decayMargin (see ResolvedDamping's declaration).
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

        // Each shelf's isolated asymptotic prediction: ratio times this
        // Channel's own implied undamped RT60 (see ResolvedDamping's
        // declaration) -- not a coupled solve, matching the design's
        // documented decision not to compensate for shelf interaction.
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

    // The Block-size bound becomes modulation-aware: the shortest
    // *instantaneous* per-Channel delay across every Channel, moved or
    // not (see "What this forces on the architecture") -- only once
    // Modulation actually moved at least one Channel (a Diffusion Step
    // has no equivalent bound; see resolveModulation's own bypass).
    if (!loop.modulation->channelModulated.empty()) {
      loop.blockSizeBoundSamples = resolveModulationBlockSizeBoundSamples(
          loop.delaysSamples,
          loop.modulation->channelModulated,
          loop.modulation->excursionSamples);
    }
  }

  return loop;
}

// Named for error messages naming the actual requested strategy (issue
// #110) rather than a name hardcoded from when `select` had only one
// alternative.
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
  }
  fail("/composition", "unsupported Downmix strategy");
}

// A `select` Downmix's unit-norm intrinsic row: a single 1.0 at `channel`
// over `channels` total positions. Left with no 1.0 entry (norm zero, not
// unit) when `channel` is out of range or `channels` is zero -- resolution
// runs before validateResolvedConfig can reject either, and this helper
// must not throw or index out of bounds on the way there.
std::vector<double> selectRow(
    const std::uint32_t channel, const std::uint32_t channels) {
  std::vector<double> row(channels, 0.0);
  if (channel < channels) {
    row[channel] = 1.0;
  }
  return row;
}

// `halves`' intrinsic row (issue #110): equal `1/sqrt(groupSize)`
// coefficients over the first `ceil(N/2)` Channels (`leftGroup`) or the
// remainder. Both groups are non-empty for every `channels >= 2`, but this
// defensively returns an all-zero row rather than dividing by zero below
// that floor -- resolution runs before validateResolvedConfig can reject a
// shorter N, mirroring `orthogonal-rows`' own defensive placeholder.
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

// `alternating`'s intrinsic row (issue #110): equal `1/sqrt(groupSize)`
// coefficients over even Channel indices (`leftGroup`) or odd indices.
// Same N>=2 defensive placeholder as `halvesRow` above.
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

// `orthogonal-rows`' intrinsic rows: rows 0 and 1 of a resolved dense
// N-by-N matrix (ADR-0002). Shared by resolveDownmix and
// validateResolvedConfig so the two never drift on how a row pair is cut
// from the flat row-major matrix.
DownmixRowPair extractDownmixRows(
    const std::vector<double>& matrix, const std::uint32_t channels) {
  return {
      std::vector<double>(matrix.begin(), matrix.begin() + channels),
      std::vector<double>(
          matrix.begin() + channels, matrix.begin() + 2 * channels),
  };
}

// Conservative estimate of the transient N-by-N matrix that
// `orthogonal-rows` resolution allocates (ADR-0002) before any row is cut
// from it -- the same shape of estimate checkDiffuserMemoryBudget and
// checkFeedbackLoopMemoryBudget make for their own mix matrices, applied
// here so a large Channel count fails as a structured configuration error
// instead of an uncontrolled allocation.
std::optional<std::uint64_t> estimateDownmixOrthogonalMatrixBytes(
    const std::uint32_t channels) noexcept {
  constexpr std::uint64_t kSampleBytes = 8;
  const auto matrixElements = checkedMul(channels, channels);
  return matrixElements ? checkedMul(*matrixElements, kSampleBytes)
                        : std::nullopt;
}

// The Width stage's resolved 2x2 mid/side matrix (docs/design/reverb/
// stages/08-downmix.md's "Width as a constant-power mid/side law"),
// applied to the pre-Width [left, right] vector as row-major
// [[m00, m01], [m10, m11]]. Exact endpoint matrices at 0/90/180 degrees
// bypass the general trig formula: 90 degrees genuinely needs its own
// case, since cos(pi/4) and sin(pi/4), though mathematically equal, are
// not guaranteed bit-identical from libm, and the design doc requires
// 90 degrees to be an exact identity bypass. Shared by resolveDownmix and
// validateResolvedConfig so the two never drift on the formula.
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
    // No fallback: `select` has no implicit Channel choice (issue #107),
    // so a caller that reaches this without the JSON boundary's
    // requireField (e.g. a direct C++ ReverbConfig construction) must
    // still be rejected here rather than silently resolving to the
    // archived Channel-0 default.
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
    // leftChannel/rightChannel are `select`-specific (issue #108): a
    // direct C++ construction that sets either alongside another
    // strategy is rejected here too, mirroring the JSON boundary.
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
    // Every non-select strategy requires N >= 2 to have a distinct row 1
    // (issue #108/#110); resolution runs before validateResolvedConfig can
    // reject a shorter N, so this defensively resolves an all-zero
    // placeholder rather than indexing past a too-short row.
    if (channels < 2) {
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
  };
}

// A wet branch's requested level, converted to a linear multiplier (issue
// #109; shared by the Early branch's own `levelDb`, issue #111) --
// resolved once, before construction, so Reverb's audio processing never
// computes `pow`. Shared by resolveConfig and validateResolvedConfig so
// the two never drift on the formula.
double resolveLinearGainFromDb(const double levelDb) noexcept {
  return std::pow(10.0, levelDb / 20.0);
}

// Early's own `select` Downmix default (issue #111, docs/design/reverb/
// stages/09-composition.md's "A present Early Reflections branch defaults
// to ... select Channels 0/1 (or Channel 0 duplicated at N=1)"), unlike
// the Main Downmix's own `select`, which has no implicit Channel choice
// (issue #107) and is rejected outright when omitted. Only fills
// leftChannel/rightChannel when the caller supplied neither -- an
// explicit leftChannel with no rightChannel still means mono duplication,
// exactly like every other `select` Downmix.
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

// The Main wet path's own resolved Diffuser, if any (issue #112): shared
// by resolveConfig's own Early tap-support resolution below, which needs
// it whether or not a Diffuser is actually present (an absent one is a
// resolution-time no-op here -- validateResolvedConfig's own diffuserStage
// check rejects a missing Diffuser authoritatively afterward). There is
// at most one Diffuser per the shapes validateShape admits.
const dsp::ResolvedDiffuser* findResolvedDiffuser(
    const std::vector<dsp::ResolvedStage>& stages) {
  for (const auto& stage : stages) {
    if (const auto* diffuser = std::get_if<dsp::ResolvedDiffuser>(&stage)) {
      return diffuser;
    }
  }
  return nullptr;
}

struct TapSupportBounds {
  std::uint64_t nominalSupportMinSamples = 0;
  std::uint64_t nominalSupportMaxSamples = 0;
  std::uint64_t conservativeSupportMinSamples = 0;
  std::uint64_t conservativeSupportMaxSamples = 0;
};

// Nominal and conservative Tap support bounds through `stepIndex`
// inclusive (issue #112, docs/design/reverb/stages/
// 07-early-reflections.md's "Tap support"): nominal sums each
// contributing step's own minimum/maximum resolved per-Channel delay;
// conservative additionally widens every step whose own Modulation is
// active by that step's resolved Excursion (rounded up) plus the fixed
// Interpolation margin, applied symmetrically as a conservative (never
// underestimating) reach on both sides -- the same reach
// resolveModulationHeadroomSamples already uses to size delay-line
// buffers, applied here to bound arrival time instead. Shared by
// resolveConfig and validateResolvedConfig so the two never drift on the
// formula. Defensive: an out-of-range stepIndex or an unresolved step
// (empty delaysSamples, e.g. an invalid mix/Channel-count combination the
// Diffuser's own validation rejects separately) contributes nothing
// rather than indexing out of bounds -- resolution runs before
// validateResolvedConfig can reject either.
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

// Resolves one Early tap: its own gain offset, nominal/conservative
// support bounds (in samples and milliseconds), and its shaping gain --
// gainDb minus decayDbPerSec times the nominal support end in seconds
// (docs/design/reverb/stages/07-early-reflections.md's "Early envelope"),
// converted to a linear multiplier. Takes already-resolved-shaped values
// (`stepIndex`, `gainDb`) rather than a Requested-layer EarlyTapConfig,
// so validateResolvedConfig can recompute the same expected tap from
// resolved fields alone -- mirroring how validateResolvedDownmixFields
// and validateResolvedModulation each recompute their own expected
// values from resolved data only, never by reassembling a config-layer
// request struct. `decayDbPerSec` is used as given when finite and
// non-negative; otherwise 0.0 keeps this defensively computable,
// mirroring resolveModulation's own bypass on invalid requested values --
// validateResolvedConfig rejects an actually invalid `decayDbPerSec` from
// the caller's own recorded field, not from this function's internal
// fallback.
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
    // resolveConfig is a public non-JSON entry point too (issue #109,
    // mirroring #107/#108's own direct-construction checks): the JSON
    // boundary's own rejection is not the only place this contract has to
    // hold for a caller that skips it (e.g. a direct C++ ReverbConfig
    // construction).
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
  }
  if (requestedComposition != nullptr &&
      !requestedComposition->stages.empty()) {
    resolved.composition.mainEnabled =
        requestedComposition->mainEnabled.value_or(true);
    resolved.composition.mainLevelDb =
        requestedComposition->mainLevelDb.value_or(0.0);
    resolved.composition.mainGain =
        resolveLinearGainFromDb(resolved.composition.mainLevelDb);
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
    // The Main Downmix's Alignment expectation: unaligned when its source
    // includes a Feedback Loop, aligned otherwise (Diffuser-only) -- see
    // docs/design/reverb/stages/08-downmix.md and issue #107. Requested
    // configuration cannot set this directly.
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
                  // Fast, approximate pre-resolution gate: no step is
                  // resolved yet, so no Modulation headroom is known
                  // (modulationBytes = 0) -- the authoritative,
                  // headroom-aware check runs post-resolution in
                  // validateDiffuserStage, which always runs before this
                  // Diffuser reaches DSP construction.
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

    // The parallel Early Reflections branch (issues #111/#112): resolved
    // once every Main wet path stage above is resolved, since its own
    // taps and Downmix both need the Composition's own `channels` --
    // the same N the Main Downmix resolves against -- and each tap's own
    // support bounds need the Main wet path's own resolved Diffuser.
    // Omitted `early`, an included but empty `early`, and
    // `early: {"taps": []}` all mean no branch; a non-empty `taps`
    // resolves one (see EarlyConfig's own declaration).
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

        // Unique stepIndex per tap, sorted ascending (canonical order,
        // issue #112): a Requested tap-list permutation must resolve and
        // render identically, so order is normalized here rather than
        // preserved, and a duplicate index is rejected outright.
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
            findResolvedDiffuser(resolved.composition.stages);
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

// Shared canonical-coefficient / orthogonality check for a resolved mixing
// matrix, used by both a Diffusion Step and a Feedback Loop: every mixing
// matrix satisfies MMT = I (Part IV, standing invariant 4).
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

// Validates one resolved Modulation object: shared by the Feedback Loop
// and each Diffusion Step (see docs/design/reverb/stages/06-modulation.md,
// issues #89/#91) so the property checks -- Excursion rejection, buffer
// headroom, and every per-Channel derived value -- can never drift
// between the two modulated stages. `owner` selects the caller's own
// usage-tag domain (see ModulationOwner in ModulationResolution.h);
// `itemIndex` is the positional-seeding item index resolution used for
// every per-Channel draw -- 0 for the Feedback Loop (one loop, not a
// chain of steps) and the step index for a Diffusion Step.
// `delaysSamples` and `bufferSizes` must already be validated to carry
// one entry per Channel. Does not check a Block-size bound: the Feedback
// Loop's own caller does that afterward, from its own already-computed
// `modulationActive`, since a Diffusion Step has none.
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
  // `modulationActive` (computed here from channelModulated's own
  // emptiness) and "depthMs and channelFraction both positive" must
  // agree -- resolution's own invariant -- so a hand-authored
  // resolved.json that decouples the two is caught explicitly here
  // rather than silently taking whichever branch one of the two
  // predicates happens to select.
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

// Validates one resolved Diffuser stage. `stageIndex` is always 1: a
// Diffuser is either the composition's sole middle stage or the first of
// two, since a Feedback Loop (when present) always follows it.
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
  // Diffusion Step Modulation's Excursion and Interpolation margin can
  // grow a Channel's resolved buffer past the nominal shared sample
  // budget (see docs/design/reverb/stages/06-modulation.md's "Delay
  // buffers need headroom"); take whichever bound is larger so an
  // unmodulated Diffuser's check is unchanged from before Modulation
  // existed (mirrors validateFeedbackLoopStage's own
  // `maxResolvedBufferSamples` pattern).
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

    // Modulation-aware once this step's own Modulation actually moves at
    // least one Channel (see "Delay buffers need headroom"); checked
    // instead, against the Excursion-plus-margin expectation, once
    // Modulation is validated below.
    const auto stepModulationActive = step.modulation.has_value() &&
        !step.modulation->channelModulated.empty();

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
  // Modulation's Excursion and Interpolation margin can grow a Channel's
  // resolved buffer past delayMaxSamples; take whichever bound is larger
  // so an unmodulated Feedback Loop's check is unchanged from before
  // Modulation existed.
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
  // With Damping disabled, this is the exact, authoritative check: the
  // Tail budget is derived from rt60Sec and decayMargin alone. With
  // Damping enabled, the budget instead follows the slowest resolved RT60
  // (rt60Sec, or slower when a shelf boosts) -- checked exactly once every
  // Channel's resolved gain and shelf coefficients are validated, further
  // below.
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

  // Modulation-aware once Modulation actually moves delays (see "The
  // Block-size bound becomes modulation-aware"); checked instead, against
  // the Excursion-adjusted expectation, once Modulation is validated
  // below.
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
    // Modulation-aware once active for this specific Channel (see
    // "Delay buffers need headroom"); checked instead, against the
    // Excursion-plus-margin expectation, once Modulation is validated
    // below. Guards the mask index defensively rather than trusting its
    // size yet -- that size is itself checked in the same place.
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

    // A relative tolerance, not a tight fixed epsilon: the gain
    // (log10/pow), the coefficients (tan/sqrt/pow), and the expected decay
    // (those plus cos/sin) all chain transcendental functions, so a
    // resolved.json produced on another platform (see ADR-0001) may differ
    // by a few ULPs once rerendered here.
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

      // The Reference band's distance from `rt60Sec` is not itself a
      // rejection reason (see ADR-0004, docs/adr/0004-validate-structure-
      // not-acoustics.md): a gentle one-pole shelf's transition band is
      // wide by design, so even the research-baseline default leaks
      // noticeably into 1 kHz -- a real, verified property of the filter,
      // not a bug. `expectedReferenceRt60Sec` above already records the
      // actual implied decay, which is what a later analysis/report layer
      // (not resolution) flags when it lands more than 10% from
      // `rt60Sec`.

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
      // The accept/reject gate for boosting (#77): a conservative
      // structural proof, not a frequency grid or complete FDN pole solve
      // (see docs/design/reverb/stages/05-damping.md's "Stability"). Every
      // Channel's bound must remain strictly below unity at both realized
      // precisions; this may conservatively reject an overlapping
      // boost-and-cut combination that an exact modal analysis could prove
      // safe (see ADR-0004) -- an accepted trade-off for a cheap,
      // deterministic proof.
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
      // The Block-size bound becomes modulation-aware: the shortest
      // *instantaneous* per-Channel delay across every Channel, moved or
      // not (see "What this forces on the architecture"). A Diffusion
      // Step has no equivalent bound, so this check stays here rather
      // than in the shared validateResolvedModulation.
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

// Validates one resolved Downmix's field-level contract: dimensions,
// strategy/Channel selection, compensation, rows, its Alignment
// expectation, and Width -- shared by the Main Downmix stage and the
// Early Downmix (issue #111), which resolve independently (their own
// `path` and domain-separated `orthogonalUsage` tag) but must never drift
// on what a resolved Downmix is allowed to look like.
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
    if (channels < 2) {
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
    // Branch controls are invalid on the empty identity Composition
    // (docs/design/reverb/stages/09-composition.md). mainEnabled/
    // mainLevelDb are inert booleans/doubles Reverb never reads once
    // stages are empty, but this public validator is also a direct,
    // non-JSON entry point (see ResolveConfig.h) -- a populated `early`
    // here would contradict the same invariant already enforced at the
    // Requested/JSON boundaries, so it is rejected here too rather than
    // silently accepted (issue #111).
    if (resolved.composition.early.has_value()) {
      fail(
          "/composition/early",
          "not applicable to the empty identity Composition");
    }
    return;
  }

  // The Main wet path's own enablement and level (issue #109): moot, and
  // never serialized, on the empty identity Composition handled by the
  // early return above.
  if (!std::isfinite(resolved.composition.mainLevelDb)) {
    fail("/composition/mainLevelDb", "expected a finite value");
  }
  // Checked at float precision regardless of this build's own Sample
  // type -- a resolved.json is meant to give float and double DSP the
  // same structure (docs/design/reverb/stages/09-composition.md), and
  // float has the narrower range: an extreme but finite double mainGain
  // (e.g. 1e50 from a +1000 dB mainLevelDb) silently becomes infinity
  // when narrowed to float, and a tiny one silently becomes exact zero,
  // either way diverging from the resolved value DSP construction
  // actually reads (see Downmix's own effectiveLeftRow/effectiveRightRow
  // for the same double-resolved/Sample-applied split).
  const auto mainGainAtFloatPrecision =
      static_cast<float>(resolved.composition.mainGain);
  if (!(resolved.composition.mainGain > 0.0) ||
      !std::isfinite(resolved.composition.mainGain) ||
      !std::isfinite(mainGainAtFloatPrecision) ||
      !(mainGainAtFloatPrecision > 0.0f)) {
    fail(
        "/composition/mainGain",
        "expected finite positive gain representable at float "
        "precision");
  }
  if (resolved.composition.mainGain !=
      resolveLinearGainFromDb(resolved.composition.mainLevelDb)) {
    fail(
        "/composition/mainGain",
        "expected gain derived from mainLevelDb");
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
  // Captured for the Early Reflections branch below (issue #111): the
  // Main wet path's own Diffuser, if any -- there is at most one per the
  // shapes validateShape admits, so this pointer is unambiguous.
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

  // The parallel Early Reflections branch (issues #111/#112): valid only
  // when the Main wet path contains a Diffuser (docs/design/reverb/
  // stages/09-composition.md), every tap's own stepIndex must be unique
  // and sorted ascending (canonical order) and reference an in-range
  // resolved Diffusion Step, each tap's own support bounds and shaping
  // gain must match the shared resolution formula, and its Downmix is
  // validated by the same shared field contract as Main's -- always an
  // aligned Alignment expectation and its own domain-separated
  // RandomOrthogonal usage tag.
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
      const auto tapGainAtFloatPrecision = static_cast<float>(tap.gain);
      if (!(tap.gain > 0.0) || !std::isfinite(tap.gain) ||
          !std::isfinite(tapGainAtFloatPrecision) ||
          !(tapGainAtFloatPrecision > 0.0f)) {
        fail(
            tapPath + "/gain",
            "expected finite positive gain representable at float "
            "precision");
      }
      if (tap.gain != expectedTap.gain) {
        fail(tapPath + "/gain", "expected gain derived from shapingGainDb");
      }
    }
    if (!std::isfinite(early.levelDb)) {
      fail("/composition/early/levelDb", "expected a finite value");
    }
    // Checked at float precision regardless of this build's own Sample
    // type, mirroring mainGain's own check above (issue #109).
    const auto earlyGainAtFloatPrecision = static_cast<float>(early.gain);
    if (!(early.gain > 0.0) || !std::isfinite(early.gain) ||
        !std::isfinite(earlyGainAtFloatPrecision) ||
        !(earlyGainAtFloatPrecision > 0.0f)) {
      fail(
          "/composition/early/gain",
          "expected finite positive gain representable at float "
          "precision");
    }
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
