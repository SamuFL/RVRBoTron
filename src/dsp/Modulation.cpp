#include "rvrbotron/dsp/Modulation.h"

#include "rvrbotron/dsp/MathConstants.h"
#include "rvrbotron/dsp/OwnedBytes.h"
#include "rvrbotron/dsp/PositionalRandom.h"

#include <algorithm>
#include <cmath>

namespace rvrbotron::dsp {
namespace {

// This Channel's smoothed-random target sequence is a pure function of
// its own derived seed and an integer target counter, never accumulated
// state (see docs/design/reverb/stages/06-modulation.md's "Decorrelation
// and shape"). `targetIndex` is always >= -1 for a non-negative frame
// index and rate (see Modulation::lookbackSamples), so biasing it by one
// keeps the draw index representable as an unsigned counter. This is a
// distinct usage tag from resolution's own per-Channel seed derivation
// (config::kModulationSeedUsage): that one selects *which* seed a
// Channel gets, this one selects *which point along that Channel's own
// trajectory* -- two different axes drawn from the same per-Channel
// seed, unrelated to any notion of "which stage this Modulation
// belongs to" (see ResolvedModulation's declaration -- there is no such
// field).
constexpr std::uint64_t kModulationTrajectoryUsage = 0x4d4f44544152475fULL;

double targetValue(
    const std::uint64_t channelSeed, const std::int64_t targetIndex) {
  const auto drawIndex = static_cast<std::uint64_t>(targetIndex + 1);
  const auto unit = positionalUnitDoubleV1(
      channelSeed, kModulationTrajectoryUsage, 0, 0, drawIndex);
  return 2.0 * unit - 1.0;
}

// Cubic Catmull-Rom interpolation through 4 targets at relative
// positions -1, 0, 1, 2, evaluated at `t` in [0, 1] between p1 and p2.
double catmullRom(
    const double p0,
    const double p1,
    const double p2,
    const double p3,
    const double t) noexcept {
  const auto t2 = t * t;
  const auto t3 = t2 * t;
  return 0.5 *
      ((2.0 * p1) + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
       (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
}

double smoothedRandomTrajectory(
    const std::uint64_t channelSeed, const double t) noexcept {
  const auto base = std::floor(t);
  const auto frac = t - base;
  // `rateHz` is only bounded below (>= 0); resolution places no upper
  // bound on it, so `base` could in principle exceed what an int64_t can
  // represent. Clamping keeps the cast (and the +1/+2 stencil offsets
  // below) well-defined for every resolved value rather than relying on
  // a `rateHz` no reasonable render would ever reach staying in range by
  // chance (see PR #97 review) -- comfortably inside int64_t's range on
  // both sides, with headroom for the stencil's own +2.
  constexpr double kMaxTargetIndex = 9.0e18;
  const auto index = static_cast<std::int64_t>(
      std::clamp(base, -kMaxTargetIndex, kMaxTargetIndex));
  const auto p0 = targetValue(channelSeed, index - 1);
  const auto p1 = targetValue(channelSeed, index);
  const auto p2 = targetValue(channelSeed, index + 1);
  const auto p3 = targetValue(channelSeed, index + 2);
  // Catmull-Rom can overshoot the [-1, 1] range of its control points for
  // non-monotonic data, which uniform random targets certainly are;
  // clamping keeps the trajectory within the Excursion the buffer was
  // actually sized for (see resolution's "Delay buffers need headroom"),
  // rather than requiring a larger, harder-to-reason-about margin.
  return std::clamp(catmullRom(p0, p1, p2, p3, frac), -1.0, 1.0);
}

// A period-1 sine, zero and rising at every integer `t` -- one full
// cycle per target-grid step, so `rateHz` means the same thing here as
// for `smoothedRandom` (see "Decorrelation and shape"). Reducing to a
// fractional phase before calling sin() keeps the argument bounded
// regardless of how large `t` has grown over a long render, rather than
// losing precision to a huge, unreduced angle.
double sineTrajectory(const double t) noexcept {
  const auto phase = t - std::floor(t);
  return std::sin(2.0 * kPi * phase);
}

// A period-1 triangle wave sharing sine's zero crossings and peak
// locations (zero and rising at integer `t`, +1 at t + 0.25, -1 at
// t + 0.75), so a `sine`-versus-`triangle` comparison at a fixed rate
// differs only in shape, never in timing.
double triangleTrajectory(const double t) noexcept {
  const auto shifted = t + 0.25;
  const auto shiftedPhase = shifted - std::floor(shifted);
  return 1.0 - 4.0 * std::abs(shiftedPhase - 0.5);
}

} // namespace

Modulation::Modulation(const ResolvedModulation& config)
    : channelSeeds_(config.channelSeeds),
      channelTargetsPerSample_(config.channelTargetsPerSample),
      channelPhases_(config.channelPhases),
      channelModulated_(config.channelModulated),
      shape_(config.shape),
      excursionSamples_(config.excursionSamples) {}

bool Modulation::isModulated(const std::size_t channel) const noexcept {
  return channelModulated_[channel];
}

double Modulation::lookbackSamples(
    const std::size_t channel, const std::uint64_t nominalDelaySamples) const noexcept {
  // Each Channel's target grid is offset by its own positionally seeded
  // phase (see resolution's kModulationPhaseUsage) before the +-10% rate
  // spread ever separates the grids further, so no two Channels ever
  // start reading the same point of their own (independently seeded)
  // trajectory -- the third of the design doc's "Phase, rate spread and
  // Channel selection each get their own usage tag" axes. `rateHz`
  // means the same thing for every shape: one full target-grid cycle
  // per 1/rateHz seconds, computed identically here regardless of which
  // waveform is sampled from it below.
  //
  // rateHz >= 0 and frameIndex_ >= 0, so the rate term is never negative;
  // adding a phase in [0, 1) cannot make `t` negative either.
  const auto t =
      static_cast<double>(frameIndex_) * channelTargetsPerSample_[channel] +
      channelPhases_[channel];
  double trajectory;
  switch (shape_) {
  case ModulationShape::smoothedRandom:
    trajectory = smoothedRandomTrajectory(channelSeeds_[channel], t);
    break;
  case ModulationShape::sine:
    trajectory = sineTrajectory(t);
    break;
  case ModulationShape::triangle:
    trajectory = triangleTrajectory(t);
    break;
  }
  return static_cast<double>(nominalDelaySamples) +
      trajectory * excursionSamples_;
}

void Modulation::advanceFrame() noexcept {
  ++frameIndex_;
}

std::size_t Modulation::ownedBytes() const noexcept {
  // Backing-vector allocations only, mirroring DelayLine::
  // ownedStorageBytes() -- a containing FeedbackLoop owns this object's
  // own in-place storage (it holds Modulation by value inside an
  // std::optional, not by pointer) through its own sizeof(*this), so
  // adding sizeof(*this) here as well would double-count it.
  return ownedVectorBytes(channelSeeds_) +
      ownedVectorBytes(channelTargetsPerSample_) +
      ownedVectorBytes(channelPhases_) +
      ownedVectorBytes(channelModulated_);
}

} // namespace rvrbotron::dsp
