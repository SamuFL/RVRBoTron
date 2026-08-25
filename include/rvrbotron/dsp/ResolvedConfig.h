#pragma once

#include <cstdint>
#include <variant>
#include <vector>

namespace rvrbotron::dsp {

enum class SplitStrategyType {
  duplicate,
};

enum class EnergyNormalisation {
  energy,
  none,
};

enum class DelayStrategy {
  segmentedRandom,
  even,
};

enum class MixMatrixType {
  hadamard,
};

enum class PolarityStrategy {
  seededRandom,
  none,
};

enum class DownmixStrategy {
  select,
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

struct ResolvedDownmix {
  std::uint32_t inputChannels = 0;
  std::uint32_t outputChannels = 2;
  DownmixStrategy strategy = DownmixStrategy::select;
  EnergyNormalisation normalisation = EnergyNormalisation::energy;
  double compensation = 0.0;
};

using ResolvedStage =
    std::variant<ResolvedSplit, ResolvedDiffuser, ResolvedDownmix>;

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
