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
      excursionSamples_(config.excursionSamples),
      smoothedRandomTargets_(
          config.shape == ModulationShape::smoothedRandom
              ? config.channelSeeds.size()
              : 0) {}

bool Modulation::isModulated(const std::size_t channel) const noexcept {
  return channelModulated_[channel];
}

double Modulation::smoothedRandomTrajectory(
    const std::size_t channel, const double t) noexcept {
  const auto base = std::floor(t);
  const auto frac = t - base;
  constexpr double kMaxTargetIndex = 9.0e18;
  const auto index = static_cast<std::int64_t>(
      std::clamp(base, -kMaxTargetIndex, kMaxTargetIndex));
  auto& targets = smoothedRandomTargets_[channel];
  if (!targets.valid || targets.index != index) {
    for (std::int64_t point = 0; point < 4; ++point) {
      targets.points[point] =
          targetValue(channelSeeds_[channel], index - 1 + point);
    }
    targets.index = index;
    targets.valid = true;
  }
  const auto* const p = targets.points;
  return std::clamp(catmullRom(p[0], p[1], p[2], p[3], frac), -1.0, 1.0);
}

double Modulation::lookbackSamples(
    const std::size_t channel,
    const std::uint64_t nominalDelaySamples,
    const std::uint64_t frameOffset) noexcept {
  const auto t =
      static_cast<double>(frameIndex_ + frameOffset) *
          channelTargetsPerSample_[channel] +
      channelPhases_[channel];
  double trajectory;
  switch (shape_) {
  case ModulationShape::smoothedRandom:
    trajectory = smoothedRandomTrajectory(channel, t);
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

void Modulation::lookbackBlock(
    const std::size_t channel,
    const std::uint64_t nominalDelaySamples,
    const std::size_t frames,
    double* const lookbacks) noexcept {
  const auto rate = channelTargetsPerSample_[channel];
  if (shape_ != ModulationShape::smoothedRandom || frames == 0 ||
      !(rate >= 0.0)) {
    for (std::size_t frame = 0; frame < frames; ++frame) {
      lookbacks[frame] =
          lookbackSamples(channel, nominalDelaySamples, frame);
    }
    return;
  }

  // Each expression below is the one lookbackSamples() and
  // smoothedRandomTrajectory() evaluate, so every lookback is identical;
  // only the target lookup moves out of the loop. A non-negative rate
  // makes t non-decreasing, so equal first and last target indices mean
  // the whole chunk shares one set of targets.
  const auto phase = channelPhases_[channel];
  const auto nominal = static_cast<double>(nominalDelaySamples);
  const auto targetIndex = [](const double t) noexcept {
    constexpr double kMaxTargetIndex = 9.0e18;
    return static_cast<std::int64_t>(
        std::clamp(std::floor(t), -kMaxTargetIndex, kMaxTargetIndex));
  };
  const auto at = [this, rate, phase](const std::size_t frame) noexcept {
    return static_cast<double>(frameIndex_ + frame) * rate + phase;
  };
  const auto firstIndex = targetIndex(at(0));
  if (firstIndex != targetIndex(at(frames - 1))) {
    for (std::size_t frame = 0; frame < frames; ++frame) {
      lookbacks[frame] =
          lookbackSamples(channel, nominalDelaySamples, frame);
    }
    return;
  }
  const auto t0 = at(0);
  (void)smoothedRandomTrajectory(channel, t0);
  const auto* const p = smoothedRandomTargets_[channel].points;
  const auto p0 = p[0];
  const auto p1 = p[1];
  const auto p2 = p[2];
  const auto p3 = p[3];
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const auto t = at(frame);
    const auto frac = t - std::floor(t);
    const auto trajectory =
        std::clamp(catmullRom(p0, p1, p2, p3, frac), -1.0, 1.0);
    lookbacks[frame] = nominal + trajectory * excursionSamples_;
  }
}

void Modulation::advanceFrame() noexcept {
  ++frameIndex_;
}

void Modulation::advanceFrames(const std::uint64_t frames) noexcept {
  frameIndex_ += frames;
}

std::size_t Modulation::ownedBytes() const noexcept {
  return ownedVectorBytes(channelSeeds_) +
      ownedVectorBytes(channelTargetsPerSample_) +
      ownedVectorBytes(channelPhases_) +
      ownedVectorBytes(channelModulated_);
}

} // namespace rvrbotron::dsp
