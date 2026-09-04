#pragma once

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace rvrbotron::dsp {

enum class SplitStrategyType {
  duplicate,
  stereoHalves,
  stereoInterleave,
};

enum class EnergyNormalisation {
  energy,
  none,
};

enum class DelayStrategy {
  segmentedRandom,
  uniformRandom,
  even,
};

enum class MixMatrixType {
  hadamard,
  householder,
  randomOrthogonal,
};

enum class PolarityStrategy {
  seededRandom,
  none,
};

enum class DownmixStrategy {
  select,
};

enum class GainMode {
  perChannel,
  uniform,
};

struct ResolvedSplit {
  std::uint32_t inputChannels = 0;
  std::uint32_t channels = 0;
  SplitStrategyType strategy = SplitStrategyType::duplicate;
  EnergyNormalisation normalisation = EnergyNormalisation::energy;
  double sourceGain = 0.0;
  double channelGain = 0.0;
};

struct ResolvedDiffusionStep {
  std::uint32_t index = 0;
  std::uint64_t lengthSamples = 0;
  double lengthMs = 0.0;
  DelayStrategy delayStrategy = DelayStrategy::segmentedRandom;
  std::vector<std::uint64_t> delaysSamples;
  std::vector<double> delaysMs;
  std::vector<std::uint64_t> bufferSizes;
  bool shuffle = true;
  std::vector<std::uint32_t> permutation;
  PolarityStrategy polarity = PolarityStrategy::seededRandom;
  std::vector<int> polaritySigns;
  MixMatrixType mix = MixMatrixType::hadamard;
  std::vector<double> matrix;
};

struct ResolvedDiffuser {
  std::uint64_t totalSamples = 0;
  std::vector<ResolvedDiffusionStep> steps;
};

// Resolved Two-shelf Damping (see docs/design/reverb/stages/05-damping.md):
// requested low/high decay ratios and shelf corners, plus each Channel's
// resolved low- and high-shelf plateau gains, canonical prewarped one-pole
// coefficients, and expected frequency-dependent decay. Both shelves accept
// ratios from zero exclusive through 1.0 inclusive unconditionally, and
// above 1.0 (safe boosting, #77) when the conservative contraction
// certificate below passes.
struct ResolvedDamping {
  double highRatio = 0.5;
  double highHz = 4000.0;
  double lowRatio = 1.0;
  double lowHz = 200.0;
  // Per-Channel resolved shelf plateau gain (linear) and canonical
  // prewarped first-order digital coefficients:
  // y[n] = b0*x[n] + b1*x[n-1] - a1*y[n-1].
  std::vector<double> highShelfGains;
  std::vector<double> highShelfB0;
  std::vector<double> highShelfB1;
  std::vector<double> highShelfA1;
  std::vector<double> lowShelfGains;
  std::vector<double> lowShelfB0;
  std::vector<double> lowShelfB1;
  std::vector<double> lowShelfA1;
  // Per-Channel expected decay (seconds), derived from that Channel's own
  // loop time and resolved decay gain rather than measured: the low- and
  // high-band values are each shelf's isolated asymptotic prediction
  // (ratio times that Channel's own implied undamped RT60); the reference
  // value is the RT60 implied by both shelves' actual combined response at
  // 1 kHz, which is also what Reference-band validation checks against
  // `rt60Sec`. Under `perChannel` gain mode these converge to one narrow
  // common target per band; under `uniform` they range across Channels
  // (see docs/design/reverb/stages/04-feedback-loop.md's gain modes).
  std::vector<double> expectedLowRt60Sec;
  std::vector<double> expectedReferenceRt60Sec;
  std::vector<double> expectedHighRt60Sec;
  // Two-shelf boost safety (#77): the conservative one-circulation
  // contraction bound per Channel, at both realized sample precisions
  // (see docs/design/reverb/stages/05-damping.md's "Stability" and
  // rvrbotron::config::resolveChannelContractionBound). Strictly below
  // 1.0 for every Channel, at both precisions, certifies eventual
  // contraction; `contractionMargin*` is `1.0 - contractionBound*`.
  std::vector<double> contractionBoundFloat32;
  std::vector<double> contractionMarginFloat32;
  std::vector<double> contractionBoundFloat64;
  std::vector<double> contractionMarginFloat64;
  // The slower of `rt60Sec`, each boosted shelf's own conservative
  // feedback-decay estimate (expectedLow/HighRt60Sec, for whichever ratio
  // exceeds 1.0), and each boosted shelf's own state-settling time (see
  // rvrbotron::config::resolveShelfSettlingTimeSec); feeds the Tail budget
  // in place of `rt60Sec` alone. Equals `rt60Sec` exactly when neither
  // ratio exceeds 1.0, preserving undamped and attenuation-only Tail
  // budgets unchanged.
  double slowestResolvedRt60Sec = 0.0;
};

struct ResolvedFeedbackLoop {
  std::uint32_t channels = 0;
  std::uint64_t delayMinSamples = 0;
  std::uint64_t delayMaxSamples = 0;
  double delayMinMs = 0.0;
  double delayMaxMs = 0.0;
  DelayStrategy delayStrategy = DelayStrategy::segmentedRandom;
  std::vector<std::uint64_t> delaysSamples;
  std::vector<double> delaysMs;
  std::vector<std::uint64_t> bufferSizes;
  double rt60Sec = 0.0;
  // How `gains` below was solved (see docs/design/reverb/stages/
  // 04-feedback-loop.md's "Solving RT60 into gain"): `perChannel` derives
  // each Channel's gain from that Channel's own loop time, so every
  // Channel decays at the requested rate regardless of delay spread.
  // `uniform` derives one shared gain from the mean loop time across
  // Channels instead -- the reference design's approach, kept available
  // for comparison and deliberately less accurate at a wide delay spread.
  GainMode gainMode = GainMode::perChannel;
  std::vector<double> gains;
  MixMatrixType mix = MixMatrixType::householder;
  std::vector<double> matrix;
  double decayMargin = 0.0;
  // Resolved upper bound on frames rendered past input EOF, derived from
  // rt60Sec and decayMargin (see CONTEXT.md's Tail budget entry).
  std::uint64_t tailBudgetSamples = 0;
  // Resolved upper bound on the legal block size: the shortest resolved
  // per-Channel delay, in samples (see CONTEXT.md's Block-size bound
  // entry).
  std::uint64_t blockSizeBoundSamples = 0;
  // The runtime silence-floor seam for the eventual plugin's idle behaviour
  // (see docs/design/reverb/stages/04-feedback-loop.md and issue #54).
  // Disabled (nullopt) by default: the feedback write path's denormal
  // flush stays at a purely numerical threshold, so output is bit-identical
  // to a build without this field. Enabling it -- deferred past this
  // milestone -- would raise the flush to an audible threshold and let the
  // drain terminate early, which changes rendered samples and is therefore
  // a versioned Resolved Configuration value rather than an implementation
  // detail.
  std::optional<double> silenceFloorDb;
  // The first-audible Damping tracer (see docs/design/reverb/stages/
  // 05-damping.md and issue #75). Disabled (nullopt) by default: omission
  // preserves undamped output, and existing format-version-1 Resolved
  // Configurations without this field load as disabled.
  std::optional<ResolvedDamping> damping;
};

struct ResolvedDownmix {
  std::uint32_t inputChannels = 0;
  std::uint32_t outputChannels = 2;
  DownmixStrategy strategy = DownmixStrategy::select;
  EnergyNormalisation normalisation = EnergyNormalisation::energy;
  double compensation = 0.0;
};

using ResolvedStage = std::variant<
    ResolvedSplit,
    ResolvedDiffuser,
    ResolvedFeedbackLoop,
    ResolvedDownmix>;

struct ResolvedComposition {
  std::vector<ResolvedStage> stages;
};

struct ResolvedConfig {
  std::uint32_t formatVersion = 1;
  std::uint64_t seed = 0;
  std::uint32_t sampleRate = 0;
  ResolvedComposition composition;
};

} // namespace rvrbotron::dsp
