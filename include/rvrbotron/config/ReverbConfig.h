#pragma once

#include "rvrbotron/dsp/ResolvedConfig.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace rvrbotron::config {

constexpr std::uint32_t kReverbConfigFormatVersion = 2;

constexpr std::string_view kFormatVersion1RecoveryReason =
    "reverb configuration format 1 is unsupported by this build; use tag "
    "format-v1-final (commit 8a4e718) to render or analyze format-1 "
    "configurations";

inline std::optional<std::string> formatVersionRejectionReason(
    const std::uint32_t version) {
  if (version == kReverbConfigFormatVersion) {
    return std::nullopt;
  }
  if (version == 1) {
    return std::string(kFormatVersion1RecoveryReason);
  }
  return "expected integer " + std::to_string(kReverbConfigFormatVersion);
}

constexpr std::uint32_t kDefaultDiffuserStepCount = 4;
constexpr double kDefaultDiffuserTotalMs = 300.0;

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

struct ModulationConfig {
  std::optional<double> depthMs;
  std::optional<double> rateHz;
  std::optional<dsp::ModulationShape> shape;
  std::optional<double> channelFraction;
  std::optional<dsp::ModulationInterpolation> interpolation;
};

struct DiffusionStepConfig {
  std::optional<dsp::DelayStrategy> delayStrategy;
  std::optional<dsp::MixMatrixType> mix;
  std::optional<bool> shuffle;
  std::optional<dsp::PolarityStrategy> polarity;
  std::optional<ModulationConfig> modulation;
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
  std::optional<std::uint32_t> leftChannel;
  std::optional<std::uint32_t> rightChannel;
  std::optional<dsp::EnergyNormalisation> normalisation;
  std::optional<double> widthDeg;
};

constexpr double kDefaultFeedbackLoopDelayMinMs = 100.0;
constexpr double kDefaultFeedbackLoopDelayMaxMs = 200.0;
constexpr double kDefaultFeedbackLoopRt60Sec = 2.4;
constexpr double kDefaultFeedbackLoopDecayMargin = 1.5;

constexpr double kDefaultDampingHighRatio = 0.5;
constexpr double kDefaultDampingHighHz = 4000.0;
constexpr double kDefaultDampingLowRatio = 1.0;
constexpr double kDefaultDampingLowHz = 200.0;

struct DampingConfig {
  std::optional<double> highRatio;
  std::optional<double> highHz;
  std::optional<double> lowRatio;
  std::optional<double> lowHz;
};

constexpr double kDefaultModulationDepthMs = 0.4;
constexpr double kDefaultModulationRateHz = 0.7;

struct FeedbackLoopConfig {
  std::optional<double> delayMinMs;
  std::optional<double> delayMaxMs;
  std::optional<dsp::DelayStrategy> delayStrategy;
  std::optional<double> rt60Sec;
  std::optional<double> decayMargin;
  std::optional<dsp::MixMatrixType> mix;
  std::optional<dsp::GainMode> gainMode;
  std::optional<double> silenceFloorDb;
  std::optional<DampingConfig> damping;
  std::optional<ModulationConfig> modulation;
};

using StageConfig = std::
    variant<SplitConfig, DiffuserConfig, FeedbackLoopConfig, DownmixConfig>;

struct EarlyTapConfig {
  std::uint32_t stepIndex = 0;
  std::optional<double> gainDb;
};

struct EarlyConfig {
  std::optional<bool> enabled;
  std::optional<double> levelDb;
  std::optional<double> decayDbPerSec;
  std::optional<std::vector<EarlyTapConfig>> taps;
  std::optional<DownmixConfig> downmix;
};

struct CompositionConfig {
  bool stagesSpecified = false;
  std::vector<StageConfig> stages;
  std::optional<bool> mainEnabled;
  std::optional<double> mainLevelDb;
  std::optional<EarlyConfig> early;
  std::optional<double> dryDb;
  std::optional<double> wetDb;
  std::optional<bool> wetOnly;
  std::optional<double> preDelayMs;
};

struct ReverbConfig {
  std::optional<std::uint32_t> formatVersion;
  std::optional<std::uint64_t> seed;
  std::optional<CompositionConfig> composition;
};

} // namespace rvrbotron::config
