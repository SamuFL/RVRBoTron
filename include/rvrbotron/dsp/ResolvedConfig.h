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
  orthogonalRows,
  halves,
  alternating,
  sumAll,
};

enum class DownmixAlignment {
  aligned,
  unaligned,
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

enum class ModulationInterpolation {
  lagrange3,
  linear,
  allpass,
};

enum class ModulationShape {
  smoothedRandom,
  sine,
  triangle,
};

struct ResolvedModulation {
  double depthMs = 0.4;
  double rateHz = 0.7;
  ModulationShape shape = ModulationShape::smoothedRandom;
  double channelFraction = 1.0;
  ModulationInterpolation interpolation = ModulationInterpolation::lagrange3;
  double excursionSamples = 0.0;
  std::uint64_t interpolationMarginSamples = 0;
  std::vector<std::uint64_t> channelSeeds;
  std::vector<double> channelTargetsPerSample;
  std::vector<double> channelPhases;
  std::vector<bool> channelModulated;
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
  std::optional<ResolvedModulation> modulation;
};

struct ResolvedDiffuser {
  std::uint64_t totalSamples = 0;
  std::vector<ResolvedDiffusionStep> steps;
};

struct ResolvedDamping {
  double highRatio = 0.5;
  double highHz = 4000.0;
  double lowRatio = 1.0;
  double lowHz = 200.0;
  std::vector<double> highShelfGains;
  std::vector<double> highShelfB0;
  std::vector<double> highShelfB1;
  std::vector<double> highShelfA1;
  std::vector<double> lowShelfGains;
  std::vector<double> lowShelfB0;
  std::vector<double> lowShelfB1;
  std::vector<double> lowShelfA1;
  std::vector<double> expectedLowRt60Sec;
  std::vector<double> expectedReferenceRt60Sec;
  std::vector<double> expectedHighRt60Sec;
  std::vector<double> contractionBoundFloat32;
  std::vector<double> contractionMarginFloat32;
  std::vector<double> contractionBoundFloat64;
  std::vector<double> contractionMarginFloat64;
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
  GainMode gainMode = GainMode::perChannel;
  std::vector<double> gains;
  MixMatrixType mix = MixMatrixType::householder;
  std::vector<double> matrix;
  double decayMargin = 0.0;
  std::uint64_t tailBudgetSamples = 0;
  std::uint64_t blockSizeBoundSamples = 0;
  std::optional<double> silenceFloorDb;
  std::optional<ResolvedDamping> damping;
  std::optional<ResolvedModulation> modulation;
};

struct ResolvedDownmix {
  std::uint32_t inputChannels = 0;
  std::uint32_t outputChannels = 2;
  DownmixStrategy strategy = DownmixStrategy::select;
  std::optional<std::uint32_t> leftChannel;
  std::optional<std::uint32_t> rightChannel;
  EnergyNormalisation normalisation = EnergyNormalisation::energy;
  double compensation = 0.0;
  std::vector<double> leftRow;
  std::vector<double> rightRow;
  std::vector<double> effectiveLeftRow;
  std::vector<double> effectiveRightRow;
  DownmixAlignment alignment = DownmixAlignment::aligned;
  double widthDeg = 90.0;
  std::vector<double> widthMatrix;
  bool coherentDownmixAblation = false;
};

using ResolvedStage = std::variant<
    ResolvedSplit,
    ResolvedDiffuser,
    ResolvedFeedbackLoop,
    ResolvedDownmix>;

struct ResolvedEarlyTap {
  std::uint32_t stepIndex = 0;
  double gainDb = 0.0;
  std::uint64_t nominalSupportMinSamples = 0;
  std::uint64_t nominalSupportMaxSamples = 0;
  double nominalSupportMinMs = 0.0;
  double nominalSupportMaxMs = 0.0;
  std::uint64_t conservativeSupportMinSamples = 0;
  std::uint64_t conservativeSupportMaxSamples = 0;
  double conservativeSupportMinMs = 0.0;
  double conservativeSupportMaxMs = 0.0;
  double shapingGainDb = 0.0;
  double gain = 1.0;
};

struct ResolvedEarlyReflections {
  bool enabled = true;
  double levelDb = 0.0;
  double gain = 1.0;
  double decayDbPerSec = 0.0;
  std::vector<ResolvedEarlyTap> taps;
  ResolvedDownmix downmix;
};

struct ResolvedComposition {
  std::vector<ResolvedStage> stages;
  bool mainEnabled = true;
  double mainLevelDb = 0.0;
  double mainGain = 1.0;
  std::optional<ResolvedEarlyReflections> early;
  double dryDb = 0.0;
  double dryGain = 1.0;
  double wetDb = 0.0;
  double wetGain = 1.0;
  bool wetOnly = true;
  double preDelayMs = 0.0;
  std::uint64_t preDelaySamples = 0;
};

inline const ResolvedDiffuser* findResolvedDiffuser(
    const ResolvedComposition& composition) noexcept {
  for (const auto& stage : composition.stages) {
    if (const auto* const diffuser = std::get_if<ResolvedDiffuser>(&stage)) {
      return diffuser;
    }
  }
  return nullptr;
}

struct ResolvedConfig {
  std::uint32_t formatVersion = 2;
  std::uint64_t seed = 0;
  std::uint32_t sampleRate = 0;
  ResolvedComposition composition;
};

} // namespace rvrbotron::dsp
