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

// The delay line's fractional-read method for a Modulation-bearing stage
// (see docs/design/reverb/stages/06-modulation.md's "Fractional delay
// becomes mandatory"): third-order Lagrange is the default; `linear`
// (issue #92) and `allpass` (issue #93) are deliberate ablations, made
// available to be heard and measured rather than hidden. `linear`
// darkens a modulated tail as depth rises, an unintended lowpass;
// `allpass` has flat magnitude at any *fixed* fractional delay but
// carries persistent per-Channel state a *moving* delay repeatedly
// invalidates, producing transient artefacts -- see the design doc's own
// recorded finding on whether it is viable inside a compounding Feedback
// Loop. Shared by the Feedback Loop and a Diffusion Step alike, and
// identically sized: the fixed Interpolation margin is sized for the
// worst case across all three methods, so a Channel's *buffer* size
// never moves when this choice changes -- `allpass` alone also carries
// its own small per-Channel filter state, owned by whichever stage
// (Feedback Loop or Diffusion Step) actually uses it, allocated only for
// a Channel actually modulated with `allpass` chosen (see
// FeedbackLoop/DiffusionStep's own `allpassState_`).
enum class ModulationInterpolation {
  lagrange3,
  linear,
  allpass,
};

// A Modulation's per-Channel trajectory waveform (see docs/design/
// reverb/stages/06-modulation.md's "Decorrelation and shape"):
// `smoothedRandom` is Catmull-Rom interpolation between random targets;
// `sine` and `triangle` are periodic. `rateHz` means the same thing for
// all three -- one full target-grid cycle per 1/rateHz seconds -- so
// comparing shapes at a fixed rate compares only the shapes.
enum class ModulationShape {
  smoothedRandom,
  sine,
  triangle,
};

// Resolved Modulation (see docs/design/reverb/stages/06-modulation.md,
// issues #89/#91): seeded per-Channel delay-time movement, read through a
// fractional DelayLine interpolator, on either the Feedback Loop (where
// movement compounds every circulation) or a Diffusion Step (where the
// signal passes once, so movement is one-shot and does not compound).
// Disabled (nullopt) on the owning stage by default: omission preserves
// existing rendered output and leaves the Resolved Configuration
// byte-identical to one written without it.
//
// There is no `target` field (see the design doc's "Placement"): which
// stage a resolved Modulation belongs to is already recorded by where
// this struct is nested (on the Feedback Loop, or on a Diffusion Step),
// not by a second field naming the same decision. Likewise, "nominal and
// maximum buffer bounds" -- called for in issue #89's acceptance criteria
// -- are the owning stage's own `delaysSamples` and `bufferSizes`, not
// duplicated here.
struct ResolvedModulation {
  // Peak Excursion, in milliseconds, and the LFO rate, in Hz, that
  // together govern the Detune product (see "Depth and rate multiply").
  // 0.4ms / 0.7Hz is the documented research baseline for an included but
  // otherwise empty Modulation object -- not a neutral default.
  double depthMs = 0.4;
  double rateHz = 0.7;
  ModulationShape shape = ModulationShape::smoothedRandom;
  // Proportion of Channels modulated, rounded up to the nearest Channel
  // (see "Placement"'s "channelFraction is what tests it at the
  // output"). 1.0 modulates every Channel; 0 disables Modulation for
  // this stage, exactly like an explicit depthMs of 0.
  double channelFraction = 1.0;
  ModulationInterpolation interpolation = ModulationInterpolation::lagrange3;
  // depthMs resolved to samples at this Composition's sample rate; 0 when
  // depthMs is 0.
  double excursionSamples = 0.0;
  // The fixed worst-case Interpolation margin (see "What this forces on
  // the architecture"), in samples, sized for the worst of all three
  // eventual interpolation methods so DSP-owned memory does not move when
  // the method changes.
  std::uint64_t interpolationMarginSamples = 0;
  // Per-Channel derived trajectory seed (a pure function of this
  // Composition's own seed, the owning stage's own positional itemIndex
  // -- 0 for the Feedback Loop, this step's own index for a Diffusion
  // Step -- and the Channel index), per-Channel resolved trajectory rate
  // -- rateHz times that Channel's own fixed +-10% seeded spread, already
  // divided by the sample rate so the DSP layer works in per-sample units
  // like every other resolved rate in this codebase -- and per-Channel
  // resolved phase, a positionally seeded offset in [0, 1) added to that
  // Channel's target-grid position (see "Decorrelation and shape"'s
  // "Phase, rate spread and Channel selection each get their own usage
  // tag"). All three empty when depthMs is 0 (the resolved bypass; see
  // "Identity is guaranteed by construction, not by arithmetic").
  std::vector<std::uint64_t> channelSeeds;
  std::vector<double> channelTargetsPerSample;
  std::vector<double> channelPhases;
  // The per-Channel bypass mask (see "What this forces on the
  // architecture"'s "Identity is guaranteed by construction, not by
  // arithmetic"): true for a Channel actually modulated -- the first
  // ceil(channelFraction * N) entries of a positionally seeded fixed
  // permutation, independent of delay ordering -- false for a Channel
  // that keeps the cheaper integer read path. Empty exactly when the
  // other three per-Channel vectors are (the resolved bypass).
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
  // Seeded delay-time movement scoped to this step alone -- one-shot,
  // non-compounding, as distinct from the Feedback Loop's own compounding
  // Modulation (see docs/design/reverb/stages/06-modulation.md's
  // "Placement" and issue #91). Disabled (nullopt) by default: omission
  // preserves this step's existing rendered output and resolved bytes.
  // Seeded per step (this step's own `index` is the positional
  // itemIndex, see ResolvedModulation) and per Channel, so two modulated
  // steps never share a trajectory.
  std::optional<ResolvedModulation> modulation;
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
  // Seeded, compounding delay-time movement (see docs/design/reverb/
  // stages/06-modulation.md and issue #89) -- as distinct from a
  // Diffusion Step's own one-shot Modulation (issue #91). Disabled
  // (nullopt) by default: omission preserves existing rendered output,
  // and existing format-version-1 Resolved Configurations without this
  // field load as disabled.
  std::optional<ResolvedModulation> modulation;
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
  std::uint32_t formatVersion = 2;
  std::uint64_t seed = 0;
  std::uint32_t sampleRate = 0;
  ResolvedComposition composition;
};

} // namespace rvrbotron::dsp
