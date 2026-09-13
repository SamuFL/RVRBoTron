#include "rvrbotron/config/DampingResolution.h"

#include "rvrbotron/dsp/MathConstants.h"

#include <algorithm>
#include <cmath>

namespace rvrbotron::config {
namespace {

using dsp::kPi;

} // namespace

double resolveShelfGain(
    const double channelGain, const double ratio) noexcept {
  const auto lossDb = 20.0 * std::log10(channelGain);
  const auto shelfDb = lossDb * (1.0 / ratio - 1.0);
  return std::pow(10.0, shelfDb / 20.0);
}

ShelfCoefficients resolveHighShelfCoefficients(
    const double gain,
    const double cornerHz,
    const double sampleRateHz) noexcept {
  // Prewarp the requested corner so its digital-domain response lands
  // exactly at cornerHz after the bilinear transform below.
  const auto warpedCorner =
      2.0 * sampleRateHz * std::tan(kPi * cornerHz / sampleRateHz);
  const auto bilinearScale = 2.0 * sampleRateHz;
  // p = wc*sqrt(gain): the pole/zero placement that makes |H(j*wc)| =
  // sqrt(gain) exactly (see resolveHighShelfCoefficients's declaration).
  const auto pole = warpedCorner * std::sqrt(gain);
  const auto denominator = bilinearScale + pole;
  return {
      (gain * bilinearScale + pole) / denominator,
      (pole - gain * bilinearScale) / denominator,
      (pole - bilinearScale) / denominator,
  };
}

ShelfCoefficients resolveLowShelfCoefficients(
    const double gain,
    const double cornerHz,
    const double sampleRateHz) noexcept {
  const auto warpedCorner =
      2.0 * sampleRateHz * std::tan(kPi * cornerHz / sampleRateHz);
  const auto bilinearScale = 2.0 * sampleRateHz;
  // p = wc/sqrt(gain): the mirror image of the high shelf's pole placement
  // (see resolveLowShelfCoefficients's declaration), giving the same
  // |H(j*wc)| = sqrt(gain) at the corner.
  const auto pole = warpedCorner / std::sqrt(gain);
  const auto denominator = bilinearScale + pole;
  return {
      (bilinearScale + gain * pole) / denominator,
      (gain * pole - bilinearScale) / denominator,
      (pole - bilinearScale) / denominator,
  };
}

double shelfMagnitudeAtFrequency(
    const ShelfCoefficients& coefficients,
    const double frequencyHz,
    const double sampleRateHz) noexcept {
  const auto omega = 2.0 * kPi * frequencyHz / sampleRateHz;
  const auto cosOmega = std::cos(omega);
  const auto sinOmega = std::sin(omega);
  // H(e^{j*omega}) = (b0 + b1*e^{-j*omega}) / (1 + a1*e^{-j*omega}).
  const auto numeratorReal = coefficients.b0 + coefficients.b1 * cosOmega;
  const auto numeratorImag = -coefficients.b1 * sinOmega;
  const auto denominatorReal = 1.0 + coefficients.a1 * cosOmega;
  const auto denominatorImag = -coefficients.a1 * sinOmega;
  const auto numeratorMagnitudeSquared =
      numeratorReal * numeratorReal + numeratorImag * numeratorImag;
  const auto denominatorMagnitudeSquared =
      denominatorReal * denominatorReal + denominatorImag * denominatorImag;
  return std::sqrt(numeratorMagnitudeSquared / denominatorMagnitudeSquared);
}

double resolveMatrixContractionBound(
    const std::uint32_t channels, const double epsilon) noexcept {
  return 1.0 + std::sqrt(static_cast<double>(channels)) * epsilon;
}

double resolveChannelContractionBound(
    const double channelGain,
    const double lowShelfGain,
    const double highShelfGain,
    const double matrixBound) noexcept {
  return channelGain * std::max(1.0, lowShelfGain) *
      std::max(1.0, highShelfGain) * matrixBound;
}

double resolveShelfSettlingTimeSec(
    const double a1, const double sampleRateHz) noexcept {
  if (a1 == 0.0) {
    return 0.0;
  }
  const auto samples = 60.0 / (-20.0 * std::log10(std::abs(a1)));
  return samples / sampleRateHz;
}

} // namespace rvrbotron::config
