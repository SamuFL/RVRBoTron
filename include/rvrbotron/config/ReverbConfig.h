#pragma once

#include "rvrbotron/dsp/ResolvedConfig.h"

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace rvrbotron::config {

struct SplitConfig {
  std::optional<std::uint32_t> channels;
  std::optional<dsp::SplitStrategyType> strategy;
  std::optional<dsp::EnergyNormalisation> normalisation;
};

enum class DiffusionDistribution {
  even,
  doubling,
};

struct DiffusionStepConfig {
  std::optional<dsp::DelayStrategy> delayStrategy;
  std::optional<dsp::MixMatrixType> mix;
  std::optional<bool> shuffle;
  std::optional<dsp::PolarityStrategy> polarity;
};

struct DiffuserConfig {
  std::optional<std::uint32_t> steps;
  std::optional<double> totalMs;
  std::optional<DiffusionDistribution> distribution;
  std::optional<DiffusionStepConfig> step;
};

struct DownmixConfig {
  std::optional<dsp::DownmixStrategy> strategy;
  std::optional<dsp::EnergyNormalisation> normalisation;
};

using StageConfig = std::variant<SplitConfig, DiffuserConfig, DownmixConfig>;

struct CompositionConfig {
  bool stagesSpecified = false;
  std::vector<StageConfig> stages;
};

struct ReverbConfig {
  std::optional<std::uint32_t> formatVersion;
  std::optional<std::uint64_t> seed;
  std::optional<CompositionConfig> composition;
};

} // namespace rvrbotron::config
