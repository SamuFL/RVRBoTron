#include "rvrbotron/dsp/Modulation.h"

#include "rvrbotron/dsp/MathConstants.h"
#include "rvrbotron/dsp/OwnedBytes.h"
#include "rvrbotron/dsp/PositionalRandom.h"

#include <algorithm>
#include <cmath>

namespace rvrbotron::dsp {
namespace {

constexpr std::uint64_t kModulationTrajectoryUsage = 0x4d4f44544152475fULL;

double targetValue(
    const std::uint64_t channelSeed, const std::int64_t targetIndex) {
  const auto drawIndex = static_cast<std::uint64_t>(targetIndex + 1);
  const auto unit = positionalUnitDoubleV1(
      channelSeed, kModulationTrajectoryUsage, 0, 0, drawIndex);
  return 2.0 * unit - 1.0;
}

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
  constexpr double kMaxTargetIndex = 9.0e18;
  const auto index = static_cast<std::int64_t>(
      std::clamp(base, -kMaxTargetIndex, kMaxTargetIndex));
  const auto p0 = targetValue(channelSeed, index - 1);
  const auto p1 = targetValue(channelSeed, index);
  const auto p2 = targetValue(channelSeed, index + 1);
  const auto p3 = targetValue(channelSeed, index + 2);
  return std::clamp(catmullRom(p0, p1, p2, p3, frac), -1.0, 1.0);
}

double sineTrajectory(const double t) noexcept {
  const auto phase = t - std::floor(t);
  return std::sin(2.0 * kPi * phase);
}

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
  return ownedVectorBytes(channelSeeds_) +
      ownedVectorBytes(channelTargetsPerSample_) +
      ownedVectorBytes(channelPhases_) +
      ownedVectorBytes(channelModulated_);
}

} // namespace rvrbotron::dsp
