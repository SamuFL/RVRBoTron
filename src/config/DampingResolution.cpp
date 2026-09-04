#include "rvrbotron/config/DampingResolution.h"

#include <cmath>

namespace rvrbotron::config {
namespace {

constexpr double kPi = 3.14159265358979323846;

} // namespace

double resolveHighShelfGain(
    const double channelGain, const double highRatio) noexcept {
  const auto lossDb = 20.0 * std::log10(channelGain);
  const auto shelfDb = lossDb * (1.0 / highRatio - 1.0);
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

} // namespace rvrbotron::config
