#pragma once

#include "rvrbotron/dsp/ResolvedConfig.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace rvrbotron::config {

// The only reverb configuration format version this build parses (see
// ADR-0006 and issue #106). Requested and Resolved reverb configuration
// both require this exact value at `/formatVersion`.
constexpr std::uint32_t kReverbConfigFormatVersion = 2;

// The archived build that still renders and analyzes format-version-1
// configuration (see ADR-0006). Named here, once, so every `/formatVersion`
// rejection message quotes the same tag and commit.
constexpr std::string_view kFormatVersion1RecoveryReason =
    "reverb configuration format 1 is unsupported by this build; use tag "
    "format-v1-final (commit 8a4e718) to render or analyze format-1 "
    "configurations";

// The reason to report at `/formatVersion` for an unsupported version, or
// nullopt when `version` is the one supported format. Shared by Requested
// and Resolved configuration parsing so both name the same archived build
// for version 1 and the same wording -- derived from
// kReverbConfigFormatVersion rather than a second hardcoded literal -- for
// every other unsupported version.
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

// Research-baseline defaults for an included-but-empty Modulation object
// on a Diffusion Step (see docs/design/reverb/stages/06-modulation.md and
// issue #91): the same research baseline as the Feedback Loop's own
// Modulation, so the two stages differ only in the values a caller
// actually sets, not in what an empty object means.
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
  // Omitted (nullopt) disables Modulation for this step and preserves its
  // existing rendered output and resolved bytes; an included empty object
  // resolves to the research baseline (see ModulationConfig and issue
  // #91). Accepted in both the shared step defaults and per-step
  // overrides, exactly like every other field above: an override's own
  // `modulation` (when present) wins over the shared default's, which
  // wins over Modulation being absent entirely (see docs/design/reverb/
  // stages/06-modulation.md's "Placement").
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
  // Required for `select`, unlike the archived format-version-1
  // diagnostic Downmix's implicit Channel 0/1 choice (issue #107): a
  // missing leftChannel is rejected both at the JSON boundary
  // (ConfigJson.cpp's requireField) and again in resolveDownmix, which
  // has no fallback of its own -- so a direct (non-JSON) caller that
  // omits it is rejected rather than silently resolved to Channel 0.
  // rightChannel omitted duplicates leftChannel to mono.
  std::optional<std::uint32_t> leftChannel;
  std::optional<std::uint32_t> rightChannel;
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

// Research-baseline defaults for an included-but-empty Modulation object,
// on the Feedback Loop or a Diffusion Step alike (see docs/design/reverb/
// stages/06-modulation.md and issues #89/#91): a meaningful research
// baseline rather than a neutral product default -- explicit zero depth
// remains available for identity experiments.
constexpr double kDefaultModulationDepthMs = 0.4;
constexpr double kDefaultModulationRateHz = 0.7;

// Issue #89 shipped `lagrange3` interpolation with every Channel always
// modulated; issue #90 adds `shape` (all three waveforms) and
// `channelFraction` (partial-Channel modulation); issue #91 places the
// same ModulationConfig (defined above, alongside DiffusionStepConfig) on
// a Diffusion Step; issue #92 adds `linear`, and issue #93 adds
// `allpass` -- both deliberate ablations, neither the default --
// alongside `lagrange3`.

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
