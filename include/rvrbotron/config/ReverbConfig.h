#pragma once

#include "rvrbotron/dsp/ResolvedConfig.h"

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace rvrbotron::config {

// Reference-configuration defaults for an omitted Diffuser stage (see
// docs/design/reverb/stages/03-diffuser.md): four Diffusion Steps spanning
// 300 ms with a doubling distribution.
constexpr std::uint32_t kDefaultDiffuserStepCount = 4;
constexpr double kDefaultDiffuserTotalMs = 300.0;

// Default DSP-owned memory budget for a resolved Diffuser, configurable at
// the renderer interface (see the `--memory-budget-mib` CLI flag).
constexpr std::uint64_t kDefaultDiffuserMemoryBudgetBytes =
    512ULL * 1024ULL * 1024ULL;

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

struct DiffusionStepOverride {
  std::uint32_t index = 0;
  DiffusionStepConfig step;
};

struct DiffuserConfig {
  std::optional<std::uint32_t> steps;
  std::optional<double> totalMs;
  std::optional<DiffusionDistribution> distribution;
  std::optional<std::vector<double>> lengthsMs;
  std::optional<DiffusionStepConfig> step;
  std::optional<std::vector<DiffusionStepOverride>> stepOverrides;
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
