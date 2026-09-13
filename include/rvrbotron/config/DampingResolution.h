#pragma once

#include <cstdint>

namespace rvrbotron::config {

// Canonical prewarped first-order digital shelf coefficients:
// y[n] = b0*x[n] + b1*x[n-1] - a1*y[n-1].
struct ShelfCoefficients {
  double b0 = 1.0;
  double b1 = 0.0;
  double a1 = 0.0;
};

[[nodiscard]] double resolveShelfGain(
    double channelGain, double ratio) noexcept;

[[nodiscard]] ShelfCoefficients resolveHighShelfCoefficients(
    double gain, double cornerHz, double sampleRateHz) noexcept;

[[nodiscard]] ShelfCoefficients resolveLowShelfCoefficients(
    double gain, double cornerHz, double sampleRateHz) noexcept;

[[nodiscard]] double shelfMagnitudeAtFrequency(
    const ShelfCoefficients& coefficients,
    double frequencyHz,
    double sampleRateHz) noexcept;

[[nodiscard]] double resolveMatrixContractionBound(
    std::uint32_t channels, double epsilon) noexcept;

[[nodiscard]] double resolveChannelContractionBound(
    double channelGain,
    double lowShelfGain,
    double highShelfGain,
    double matrixBound) noexcept;

[[nodiscard]] double resolveShelfSettlingTimeSec(
    double a1, double sampleRateHz) noexcept;

} // namespace rvrbotron::config
