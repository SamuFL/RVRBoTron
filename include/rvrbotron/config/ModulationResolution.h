#pragma once

#include <cstdint>
#include <vector>

namespace rvrbotron::config {

// The fixed worst-case Interpolation margin, in samples (see docs/design/
// reverb/stages/06-modulation.md's "What this forces on the
// architecture"): third-order Lagrange's four-point stencil reaches 2
// samples beyond the nominal Excursion bound on the far (longer-lookback)
// side, plus 1 sample of floor()-rounding slack applied symmetrically on
// both sides. Fixed regardless of the configured interpolation method,
// so DSP-owned memory does not change when the method changes (`linear`
// and `allpass` arrive in later tickets, both needing no more headroom
// than this).
constexpr std::uint64_t kModulationInterpolationMarginSamples = 3;

// Which stage a Modulation draw belongs to (see docs/design/reverb/
// stages/06-modulation.md's "Placement" and issue #91): the Feedback Loop
// and a Diffusion Step draw structurally identical per-Channel values
// (rate spread, phase, and -- in rvrbotron::config::resolveModulation --
// trajectory seed and channel-selection permutation too) from the same
// functions, but need genuinely separate positional-random usage-tag
// domains, exactly like kDiffusionDelayUsage and kFeedbackLoopDelayUsage
// are already separate tags for delay derivation (see ADR-0002): a
// Diffusion Step's itemIndex 0 must never collide with the Feedback
// Loop's own fixed itemIndex 0.
enum class ModulationOwner {
  feedbackLoop,
  diffusionStep,
};

// Peak per-Channel Excursion, in samples, for a requested `depthMs` at
// this Composition's sample rate (see "Depth and rate multiply").
// `depthMs` must be finite and >= 0; `sampleRateHz` finite and > 0.
[[nodiscard]] double resolveExcursionSamples(
    double depthMs, double sampleRateHz) noexcept;

// The fixed +-10% per-Channel rate spread (see "Decorrelation and
// shape"): a documented constant, not a parameter, seeded positionally
// from this Composition's own seed, `owner`'s own usage tag, `itemIndex`,
// and the Channel index so it never reshuffles when unrelated
// configuration changes. `itemIndex` is 0 for the Feedback Loop (one
// loop, not a chain of steps) and the step index for a Diffusion Step
// (issue #91), so two modulated steps never share a trajectory.
[[nodiscard]] double resolveModulationRateSpread(
    std::uint64_t seed,
    ModulationOwner owner,
    std::uint64_t itemIndex,
    std::uint32_t channel) noexcept;

// This Channel's resolved phase: a positionally seeded offset in
// [0, 1), added to that Channel's target-grid position before the rate
// spread above ever separates the grids further (see "Decorrelation and
// shape"'s "Phase, rate spread and Channel selection each get their own
// usage tag"). Seeded independently of the rate spread and of the
// trajectory seed itself, from this Composition's own seed, `owner`'s
// own usage tag, `itemIndex`, and the Channel index -- see
// resolveModulationRateSpread's `owner`/`itemIndex`.
[[nodiscard]] double resolveModulationPhase(
    std::uint64_t seed,
    ModulationOwner owner,
    std::uint64_t itemIndex,
    std::uint32_t channel) noexcept;

// Whether a resolved per-Channel delay safely serves the requested
// Excursion plus the fixed Interpolation margin above -- the rejection
// gate for "The Block-size bound becomes modulation-aware": false means
// the configuration must be rejected before construction rather than
// risking an overrun at the modulation peak.
[[nodiscard]] bool modulationFitsDelay(
    std::uint64_t delaySamples, double excursionSamples) noexcept;

// The per-Channel buffer headroom an active Modulation reserves beyond
// its nominal resolved delay -- Excursion, rounded up, plus the fixed
// Interpolation margin (see "Delay buffers need headroom"). Shared by
// resolution and its own validation so the two formulas can never drift
// apart.
[[nodiscard]] std::uint64_t resolveModulationHeadroomSamples(
    double excursionSamples) noexcept;

// The modulation-aware Block-size bound: the shortest *instantaneous*
// per-Channel delay across every Channel (see "The Block-size bound
// becomes modulation-aware") -- a modulated Channel's own delay less the
// Excursion applied to it, an unmodulated Channel's own delay unchanged
// (issue #90's partial-Channel modulation) -- rounded down so the bound
// never overstates what is safely servable. `delaysSamples` and
// `channelModulated` must be the same size.
[[nodiscard]] std::uint64_t resolveModulationBlockSizeBoundSamples(
    const std::vector<std::uint64_t>& delaysSamples,
    const std::vector<bool>& channelModulated,
    double excursionSamples) noexcept;

} // namespace rvrbotron::config
