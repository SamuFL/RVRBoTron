#include "rvrbotron/config/ModulationResolution.h"

#include "rvrbotron/dsp/PositionalRandom.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rvrbotron::config {
namespace {

constexpr std::uint64_t kModulationRateSpreadUsage = 0x4d4f445241544553ULL;
constexpr std::uint64_t kModulationPhaseUsage = 0x4d4f445048415345ULL;

} // namespace

double resolveExcursionSamples(
    const double depthMs, const double sampleRateHz) noexcept {
  return depthMs * sampleRateHz / 1000.0;
}

double resolveModulationRateSpread(
    const std::uint64_t seed, const std::uint32_t channel) noexcept {
  const auto unit =
      dsp::positionalUnitDoubleV1(seed, kModulationRateSpreadUsage, 0, channel);
  return 1.0 + 0.1 * (2.0 * unit - 1.0);
}

double resolveModulationPhase(
    const std::uint64_t seed, const std::uint32_t channel) noexcept {
  return dsp::positionalUnitDoubleV1(seed, kModulationPhaseUsage, 0, channel);
}

bool modulationFitsDelay(
    const std::uint64_t delaySamples, const double excursionSamples) noexcept {
  return static_cast<double>(delaySamples) - excursionSamples >
      static_cast<double>(kModulationInterpolationMarginSamples);
}

std::uint64_t resolveModulationHeadroomSamples(
    const double excursionSamples) noexcept {
  return static_cast<std::uint64_t>(std::ceil(excursionSamples)) +
      kModulationInterpolationMarginSamples;
}

std::uint64_t resolveModulationBlockSizeBoundSamples(
    const std::vector<std::uint64_t>& delaysSamples,
    const std::vector<bool>& channelModulated,
    const double excursionSamples) noexcept {
  auto shortest = std::numeric_limits<double>::infinity();
  for (std::size_t channel = 0; channel < delaysSamples.size(); ++channel) {
    const auto instantaneous = channelModulated[channel]
        ? static_cast<double>(delaysSamples[channel]) - excursionSamples
        : static_cast<double>(delaysSamples[channel]);
    shortest = std::min(shortest, instantaneous);
  }
  return static_cast<std::uint64_t>(std::floor(shortest));
}

} // namespace rvrbotron::config
