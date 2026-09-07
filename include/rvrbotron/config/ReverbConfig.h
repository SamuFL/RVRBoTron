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

// Reference-configuration defaults for an omitted Feedback Loop stage (see
// docs/design/reverb/stages/04-feedback-loop.md): 100-200 ms delays,
// 2.4 s RT60, Householder mixing.
constexpr double kDefaultFeedbackLoopDelayMinMs = 100.0;
constexpr double kDefaultFeedbackLoopDelayMaxMs = 200.0;
constexpr double kDefaultFeedbackLoopRt60Sec = 2.4;
// Tail budget headroom above RT60: 1.5x places the drain's end near -90 dB,
// comfortable margin for a T30 Schroeder fit. Settled during Milestone 3
// design (see docs/adr and issue #51); sweepable per render.
constexpr double kDefaultFeedbackLoopDecayMargin = 1.5;

// Research-baseline defaults for an included-but-empty Damping object (see
// docs/design/reverb/stages/05-damping.md): half decay time above 4 kHz,
// unchanged decay below 200 Hz.
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

// Research-baseline defaults for an included-but-empty Modulation object
// on the Feedback Loop (see docs/design/reverb/stages/06-modulation.md
// and issue #89): a meaningful research baseline rather than a neutral
// product default -- explicit zero depth remains available for identity
// experiments.
constexpr double kDefaultModulationDepthMs = 0.4;
constexpr double kDefaultModulationRateHz = 0.7;

// Issue #89 shipped `lagrange3` interpolation with every Channel always
// modulated; issue #90 adds `shape` (all three waveforms) and
// `channelFraction` (partial-Channel modulation). `interpolation` still
// only accepts `lagrange3` -- `linear` and `allpass` are added by later
// tickets that extend dsp::ModulationInterpolation and this parser's
// accepted values without moving the field itself.
struct ModulationConfig {
  std::optional<double> depthMs;
  std::optional<double> rateHz;
  std::optional<dsp::ModulationShape> shape;
  std::optional<double> channelFraction;
  std::optional<dsp::ModulationInterpolation> interpolation;
};

struct FeedbackLoopConfig {
  std::optional<double> delayMinMs;
  std::optional<double> delayMaxMs;
  std::optional<dsp::DelayStrategy> delayStrategy;
  std::optional<double> rt60Sec;
  std::optional<double> decayMargin;
  std::optional<dsp::MixMatrixType> mix;
  std::optional<dsp::GainMode> gainMode;
  // Runtime silence-floor seam; omitted/nullopt resolves to disabled (see
  // dsp::ResolvedFeedbackLoop::silenceFloorDb and issue #54).
  std::optional<double> silenceFloorDb;
  // Omitted (nullopt) disables Damping and preserves existing undamped
  // output; an included empty object resolves to the research baseline
  // (see DampingConfig and issue #75).
  std::optional<DampingConfig> damping;
  // Omitted (nullopt) disables Modulation and preserves existing
  // rendered output; an included empty object resolves to the research
  // baseline above (see ModulationConfig and issue #89).
  std::optional<ModulationConfig> modulation;
};

using StageConfig = std::
    variant<SplitConfig, DiffuserConfig, FeedbackLoopConfig, DownmixConfig>;

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
