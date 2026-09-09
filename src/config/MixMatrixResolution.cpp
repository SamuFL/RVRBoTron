#include "rvrbotron/config/MixMatrixResolution.h"

#include "rvrbotron/dsp/PositionalRandom.h"

#include <cmath>
#include <limits>

namespace rvrbotron::config {
namespace {

bool hasOddParity(std::uint32_t value) noexcept {
  bool odd = false;
  while (value != 0) {
    odd = !odd;
    value &= value - 1U;
  }
  return odd;
}

// Numerically negligible pivot/reflection norms indicate the generated
// matrix is singular or near-singular; treated as an explicit construction
// failure rather than silently substituting an arbitrary direction. This
// threshold is deliberately generous relative to the double-precision
// arithmetic involved -- reaching it from an honest random fill is
// practically impossible, so it exists to convert an exceptional situation
// into a loud, explicit failure rather than to trigger routinely.
constexpr double kSingularEpsilon = 1e-9;

} // namespace

std::optional<std::size_t> matrixElementCount(
    const std::uint32_t channels) noexcept {
  if (channels == 0) {
    return std::nullopt;
  }
  const auto maxElements = std::vector<double>{}.max_size();
  if (channels > maxElements / channels) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(channels) * channels;
}

std::vector<double> resolveHadamardMatrix(const std::uint32_t channels) {
  const auto scale = 1.0 / std::sqrt(static_cast<double>(channels));
  std::vector<double> matrix(
      static_cast<std::size_t>(channels) * channels);
  for (std::uint32_t row = 0; row < channels; ++row) {
    for (std::uint32_t column = 0; column < channels; ++column) {
      matrix[static_cast<std::size_t>(row) * channels + column] =
          hasOddParity(row & column) ? -scale : scale;
    }
  }
  return matrix;
}

std::vector<double> resolveHouseholderMatrix(const std::uint32_t channels) {
  const auto offDiagonal = -2.0 / static_cast<double>(channels);
  const auto diagonal = 1.0 + offDiagonal;
  std::vector<double> matrix(
      static_cast<std::size_t>(channels) * channels);
  for (std::uint32_t row = 0; row < channels; ++row) {
    for (std::uint32_t column = 0; column < channels; ++column) {
      matrix[static_cast<std::size_t>(row) * channels + column] =
          row == column ? diagonal : offDiagonal;
    }
  }
  return matrix;
}

std::vector<double> fillRandomOrthogonalSeed(
    const std::uint32_t channels,
    const std::uint64_t seed,
    const std::uint64_t usage) {
  std::vector<double> matrix(
      static_cast<std::size_t>(channels) * channels);
  for (std::uint32_t row = 0; row < channels; ++row) {
    for (std::uint32_t column = 0; column < channels; ++column) {
      const auto unit = dsp::positionalUnitDoubleV1(
          seed, usage, row, column);
      matrix[static_cast<std::size_t>(row) * channels + column] =
          -1.0 + 2.0 * unit;
    }
  }
  return matrix;
}

std::optional<std::vector<double>> householderQrOrthogonalize(
    std::vector<double> matrix, const std::uint32_t channels) {
  const auto n = static_cast<std::size_t>(channels);
  if (n == 0 || matrix.size() != n * n) {
    return std::nullopt;
  }

  std::vector<double> q(n * n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    q[i * n + i] = 1.0;
  }

  std::vector<double> v(n, 0.0);
  for (std::size_t k = 0; k < n; ++k) {
    double normX = 0.0;
    for (std::size_t i = k; i < n; ++i) {
      const auto value = matrix[i * n + k];
      normX += value * value;
    }
    normX = std::sqrt(normX);
    if (!(normX > kSingularEpsilon)) {
      return std::nullopt;
    }

    const auto pivot = matrix[k * n + k];
    // Fixed sign convention: choose the reflection's target opposite the
    // pivot's sign, the standard numerically stable choice that avoids
    // subtracting two nearly equal numbers.
    const auto alpha = pivot >= 0.0 ? -normX : normX;

    double normV = 0.0;
    for (std::size_t i = k; i < n; ++i) {
      const auto value = matrix[i * n + k] - (i == k ? alpha : 0.0);
      v[i] = value;
      normV += value * value;
    }
    normV = std::sqrt(normV);
    if (!(normV > kSingularEpsilon)) {
      return std::nullopt;
    }
    for (std::size_t i = k; i < n; ++i) {
      v[i] /= normV;
    }

    // Apply H_k = I - 2 v v^T to the trailing submatrix (columns k..n-1)
    // and accumulate Q := Q * H_k (columns k..n-1 of Q), so after the full
    // sweep Q holds the orthogonal factor of the original matrix.
    for (std::size_t column = k; column < n; ++column) {
      double dot = 0.0;
      for (std::size_t i = k; i < n; ++i) {
        dot += v[i] * matrix[i * n + column];
      }
      for (std::size_t i = k; i < n; ++i) {
        matrix[i * n + column] -= 2.0 * dot * v[i];
      }
    }
    for (std::size_t row = 0; row < n; ++row) {
      double dot = 0.0;
      for (std::size_t i = k; i < n; ++i) {
        dot += v[i] * q[row * n + i];
      }
      for (std::size_t i = k; i < n; ++i) {
        q[row * n + i] -= 2.0 * dot * v[i];
      }
    }
  }
  return q;
}

std::optional<std::vector<double>> resolveRandomOrthogonalMatrix(
    const std::uint32_t channels,
    const std::uint64_t seed,
    const std::uint64_t usage) {
  return householderQrOrthogonalize(
      fillRandomOrthogonalSeed(channels, seed, usage), channels);
}

} // namespace rvrbotron::config
