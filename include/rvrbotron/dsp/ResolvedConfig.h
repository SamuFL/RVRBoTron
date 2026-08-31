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
