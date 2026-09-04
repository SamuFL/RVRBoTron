#pragma once

namespace rvrbotron::config {

// Canonical prewarped first-order digital shelf coefficients (see
// docs/design/reverb/stages/05-damping.md's "Filter choice"):
// y[n] = b0*x[n] + b1*x[n-1] - a1*y[n-1].
struct ShelfCoefficients {
  double b0 = 1.0;
  double b1 = 0.0;
  double a1 = 0.0;
};

// A Channel's resolved high-shelf plateau gain (linear), solved from that
// Channel's own resolved decay gain and the requested high ratio (see
// docs/design/reverb/stages/05-damping.md's "Parameterise by ratio, not
// shelf gain"): the additional per-loop dB loss is
// |g_dB| * (1/highRatio - 1), applied negatively, where g_dB is the
// Channel's decay gain expressed in dB. `channelGain` must be strictly
// between 0 and 1, and `highRatio` finite and greater than zero.
[[nodiscard]] double resolveHighShelfGain(
    double channelGain, double highRatio) noexcept;

// The half-gain-in-dB first-order high shelf (see docs/design/reverb/
// stages/05-damping.md's "half-gain frequencies"): the analog prototype
// H(s) = (gain*s + p) / (s + p), with p = wc*sqrt(gain), places the
// response at the prewarped corner wc exactly at sqrt(gain) in magnitude --
// half of the plateau gain's dB value -- for any positive `gain`. Bilinear-
// transformed with prewarping so the digital corner lands exactly at
// `cornerHz`. `gain` must be finite and greater than zero; `cornerHz` finite
// and strictly between 0 and `sampleRateHz` / 2; `sampleRateHz` greater than
// zero.
[[nodiscard]] ShelfCoefficients resolveHighShelfCoefficients(
    double gain, double cornerHz, double sampleRateHz) noexcept;

} // namespace rvrbotron::config
