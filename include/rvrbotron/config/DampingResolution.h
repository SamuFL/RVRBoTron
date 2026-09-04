#pragma once

#include <cstdint>

namespace rvrbotron::config {

// Canonical prewarped first-order digital shelf coefficients (see
// docs/design/reverb/stages/05-damping.md's "Filter choice"):
// y[n] = b0*x[n] + b1*x[n-1] - a1*y[n-1].
struct ShelfCoefficients {
  double b0 = 1.0;
  double b1 = 0.0;
  double a1 = 0.0;
};

// A Channel's resolved shelf plateau gain (linear), solved from that
// Channel's own resolved decay gain and a requested ratio (see
// docs/design/reverb/stages/05-damping.md's "Parameterise by ratio, not
// shelf gain"): the additional per-loop dB loss is |g_dB| * (1/ratio - 1),
// applied negatively, where g_dB is the Channel's decay gain expressed in
// dB. Shared by both shelves -- only the ratio (highRatio or lowRatio)
// differs. `channelGain` must be strictly between 0 and 1, and `ratio`
// finite and greater than zero.
[[nodiscard]] double resolveShelfGain(
    double channelGain, double ratio) noexcept;

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

// The mirror-image half-gain-in-dB first-order low shelf: unity at high
// frequencies, `gain` at DC, and |H(j*wc)| = sqrt(gain) at the prewarped
// corner. The analog prototype H(s) = (s + gain*p) / (s + p), with
// p = wc/sqrt(gain), gives the same half-gain corner property as the high
// shelf's p = wc*sqrt(gain), mirrored around the corner. Same argument
// contract as resolveHighShelfCoefficients.
[[nodiscard]] ShelfCoefficients resolveLowShelfCoefficients(
    double gain, double cornerHz, double sampleRateHz) noexcept;

// |H(e^(j*2*pi*frequencyHz/sampleRateHz))| for a resolved one-pole shelf
// section: the section's actual digital magnitude response at an arbitrary
// frequency, not just its asymptotic plateau. Used to evaluate the
// Reference band (1 kHz) response of a shelf whose corner may sit close
// enough to shift it away from unity (see docs/design/reverb/stages/
// 05-damping.md's "One-pole transitions are gradual"). `frequencyHz` and
// `sampleRateHz` must be finite and greater than zero.
[[nodiscard]] double shelfMagnitudeAtFrequency(
    const ShelfCoefficients& coefficients,
    double frequencyHz,
    double sampleRateHz) noexcept;

// Conservative bound on the operator (spectral) norm of an N-by-N
// orthogonal mixing matrix realized at floating-point precision `epsilon`
// (see docs/design/reverb/stages/05-damping.md's "Stability"): each
// element's relative rounding error is at most `epsilon`, so the realized
// matrix's Frobenius-norm deviation from the exact matrix is at most
// `epsilon * sqrt(channels)` (the exact orthogonal matrix's own Frobenius
// norm), and operator norm is bounded by Frobenius norm. Triangle
// inequality then gives ||realized||_2 <= 1 + epsilon*sqrt(channels), a
// closed-form bound continuous in `channels` and `epsilon` -- no frequency
// grid or matrix decomposition. `channels` must be greater than zero.
[[nodiscard]] double resolveMatrixContractionBound(
    std::uint32_t channels, double epsilon) noexcept;

// The conservative one-circulation contraction bound for a single Channel,
// at one realized sample precision (see docs/design/reverb/stages/
// 05-damping.md's "Stability"): decay gain times each monotonic shelf's
// maximum magnitude -- unity or its own plateau, whichever is larger,
// since a monotonic one-pole response never exceeds either endpoint --
// times `matrixBound` (see resolveMatrixContractionBound). A value
// strictly below 1.0 for every Channel, at both float32 and float64
// `matrixBound`, certifies eventual contraction. All arguments must be
// finite and greater than zero.
[[nodiscard]] double resolveChannelContractionBound(
    double channelGain,
    double lowShelfGain,
    double highShelfGain,
    double matrixBound) noexcept;

// Samples for a stable one-pole shelf section's own state to decay by
// 60 dB, from its resolved `a1` coefficient alone (see FeedbackLoop's
// y[n] = b0*x[n] + b1*x[n-1] - a1*y[n-1]): with no further input, the
// section's own state decays as |a1|^n, so n solves |a1|^n = 10^-3.
// Zero when `a1` is exactly zero (no memory). `a1` must be finite and
// strictly less than 1.0 in magnitude (always true for this stage's
// canonical shelf coefficients, see resolveHighShelfCoefficients and
// resolveLowShelfCoefficients); `sampleRateHz` greater than zero.
[[nodiscard]] double resolveShelfSettlingTimeSec(
    double a1, double sampleRateHz) noexcept;

} // namespace rvrbotron::config
