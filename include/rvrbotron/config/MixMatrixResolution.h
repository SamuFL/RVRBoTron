#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace rvrbotron::config {

// Overflow-checked element count (channels^2) for a resolved N-by-N mixing
// matrix; nullopt if the count would exceed what a std::vector<double> can
// address.
[[nodiscard]] std::optional<std::size_t> matrixElementCount(
    std::uint32_t channels) noexcept;

// Normalized canonical Sylvester-Hadamard matrix, in row-major signal
// order. Valid only for a power-of-two Channel count; callers must check
// that before calling.
[[nodiscard]] std::vector<double> resolveHadamardMatrix(
    std::uint32_t channels);

// Normalized Householder reflection off the all-ones vector: subtracts
// twice the mean of the Channels from every Channel. Valid for any N >= 1.
[[nodiscard]] std::vector<double> resolveHouseholderMatrix(
    std::uint32_t channels);

// The domain-separated usage tag for Diffusion Step and Feedback Loop
// RandomOrthogonal mixing (see ADR-0002): "MIXORTHO". Every RandomOrthogonal
// dense fill -- mixing or Downmix alike -- shares fillRandomOrthogonalSeed
// and householderQrOrthogonalize; only the usage tag differs per site, so
// callers pass their own tag explicitly rather than one being implied.
constexpr std::uint64_t kMixMatrixRandomOrthogonalUsage =
    0x4d49584f5254484fULL;

// Deterministic dense fill in [-1, 1] from the positional random stream,
// one draw per matrix cell (row, column) under `usage`; the matrix is
// shared across Diffusion Steps rather than per-step, so the derivation
// does not use a step index. Not itself orthogonal -- the input to
// Householder QR orthogonalization.
[[nodiscard]] std::vector<double> fillRandomOrthogonalSeed(
    std::uint32_t channels, std::uint64_t seed, std::uint64_t usage);

// Orthogonalizes an arbitrary dense N-by-N matrix (row-major) via
// Householder QR with a fixed sign convention -- the reflection sign at
// each elimination step is chosen opposite the pivot, the standard choice
// that avoids cancellation -- returning the Q factor. Returns nullopt if
// any elimination step's remaining pivot column is singular or
// numerically near-singular, rather than silently substituting an
// arbitrary reflection to continue.
[[nodiscard]] std::optional<std::vector<double>> householderQrOrthogonalize(
    std::vector<double> matrix, std::uint32_t channels);

// fillRandomOrthogonalSeed(channels, seed, usage) followed by
// householderQrOrthogonalize(...).
[[nodiscard]] std::optional<std::vector<double>>
resolveRandomOrthogonalMatrix(
    std::uint32_t channels, std::uint64_t seed, std::uint64_t usage);

} // namespace rvrbotron::config
