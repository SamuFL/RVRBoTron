#include "rvrbotron/config/MixMatrixResolution.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

bool close(const double actual, const double expected,
           const double tolerance) noexcept {
  return std::abs(actual - expected) <= tolerance;
}

// Q Q^T = I within `tolerance`, computed directly from the coefficients --
// the property test the production construction must satisfy, not a
// reimplementation of Householder QR trusted to agree with it.
bool isOrthogonal(const std::vector<double>& matrix,
                  const std::uint32_t channels,
                  const double tolerance) noexcept {
  for (std::uint32_t rowA = 0; rowA < channels; ++rowA) {
    for (std::uint32_t rowB = 0; rowB < channels; ++rowB) {
      double dot = 0.0;
      for (std::uint32_t column = 0; column < channels; ++column) {
        dot +=
            matrix[static_cast<std::size_t>(rowA) * channels + column] *
            matrix[static_cast<std::size_t>(rowB) * channels + column];
      }
      const auto expected = rowA == rowB ? 1.0 : 0.0;
      if (!close(dot, expected, tolerance)) {
        return false;
      }
    }
  }
  return true;
}

} // namespace

int main() {
  using namespace rvrbotron::config;

  if (matrixElementCount(0).has_value()) {
    std::cerr << "matrixElementCount accepted zero Channels\n";
    return 1;
  }
  if (matrixElementCount(4) != std::optional<std::size_t>(16)) {
    std::cerr << "matrixElementCount(4) did not return 16\n";
    return 1;
  }

  // Classic 2x2 Sylvester-Hadamard matrix, hand-derived: scale = 1/sqrt(2).
  constexpr double kHadamardScale = 0.7071067811865475;
  const std::vector<double> expectedHadamard2{
      kHadamardScale, kHadamardScale, kHadamardScale, -kHadamardScale};
  if (resolveHadamardMatrix(2) != expectedHadamard2) {
    std::cerr << "resolveHadamardMatrix(2) did not match the hand-derived "
                 "canonical matrix\n";
    return 1;
  }

  // Householder reflection off the all-ones vector: diagonal 1 - 2/N,
  // off-diagonal -2/N. N=3: diagonal 1/3, off-diagonal -2/3. Compared with
  // a tight tolerance rather than exact equality, since `1.0/3.0` and the
  // production formula's `1.0 + (-2.0/3.0)` round to double differently in
  // their last bit despite being mathematically identical.
  constexpr double kHouseholderDiagonal3 = 1.0 / 3.0;
  constexpr double kHouseholderOffDiagonal3 = -2.0 / 3.0;
  const std::vector<double> expectedHouseholder3{
      kHouseholderDiagonal3,    kHouseholderOffDiagonal3,
      kHouseholderOffDiagonal3, kHouseholderOffDiagonal3,
      kHouseholderDiagonal3,    kHouseholderOffDiagonal3,
      kHouseholderOffDiagonal3, kHouseholderOffDiagonal3,
      kHouseholderDiagonal3,
  };
  const auto actualHouseholder3 = resolveHouseholderMatrix(3);
  if (actualHouseholder3.size() != expectedHouseholder3.size()) {
    std::cerr << "resolveHouseholderMatrix(3) returned the wrong size\n";
    return 1;
  }
  for (std::size_t index = 0; index < expectedHouseholder3.size(); ++index) {
    if (!close(actualHouseholder3[index], expectedHouseholder3[index],
              1e-15)) {
      std::cerr << "resolveHouseholderMatrix(3) did not match the "
                   "hand-derived canonical matrix\n";
      return 1;
    }
  }
  // N=1 is a trivial reflection: a single-Channel polarity flip.
  if (resolveHouseholderMatrix(1) != std::vector<double>{-1.0}) {
    std::cerr << "resolveHouseholderMatrix(1) did not match the trivial "
                 "single-Channel reflection\n";
    return 1;
  }
  if (!isOrthogonal(resolveHouseholderMatrix(5), 5, 1e-12)) {
    std::cerr << "resolveHouseholderMatrix(5) is not orthogonal\n";
    return 1;
  }

  // Independently derived from the documented positional derivation (ADR
  // 0002): splitMix64-based positionalUnitDoubleV1 with usage "MIXORTHO",
  // itemIndex = row, valueIndex = column, mapped from [0,1) to [-1,1).
  const std::vector<double> expectedFill{
      0.2933942688714306,
      -0.09475804783441255,
      -0.5852900294878902,
      -0.2744846012848514,
  };
  const auto actualFill =
      fillRandomOrthogonalSeed(2, 7, kMixMatrixRandomOrthogonalUsage);
  if (actualFill.size() != expectedFill.size()) {
    std::cerr << "fillRandomOrthogonalSeed(2, 7) returned the wrong size\n";
    return 1;
  }
  for (std::size_t index = 0; index < expectedFill.size(); ++index) {
    if (!close(actualFill[index], expectedFill[index], 1e-15)) {
      std::cerr << "fillRandomOrthogonalSeed(2, 7) did not match the "
                   "independently derived fixed vector\n";
      return 1;
    }
  }

  // The Main Downmix's own domain-separated usage tag ("MAINDNMX", ADR-0002,
  // issue #108) fills a genuinely different matrix from "MIXORTHO" at the
  // same (channels, seed) -- branch separation -- independently derived the
  // same way as expectedFill above, just with the other tag.
  constexpr std::uint64_t kMainDownmixUsage = 0x4d41494e444e4d58ULL;
  const std::vector<double> expectedMainDownmixFill{
      -0.05678428623707443,
      -0.49216371819247473,
      0.5601412288335468,
      0.9342651142760141,
  };
  const auto actualMainDownmixFill =
      fillRandomOrthogonalSeed(2, 7, kMainDownmixUsage);
  if (actualMainDownmixFill.size() != expectedMainDownmixFill.size()) {
    std::cerr << "fillRandomOrthogonalSeed(2, 7, MAINDNMX) returned the "
                 "wrong size\n";
    return 1;
  }
  for (std::size_t index = 0; index < expectedMainDownmixFill.size();
       ++index) {
    if (!close(
            actualMainDownmixFill[index], expectedMainDownmixFill[index],
            1e-15)) {
      std::cerr << "fillRandomOrthogonalSeed(2, 7, MAINDNMX) did not match "
                   "the independently derived fixed vector\n";
      return 1;
    }
  }
  if (actualMainDownmixFill == actualFill) {
    std::cerr << "MAINDNMX and MIXORTHO produced the same fill at the same "
                 "(channels, seed) -- usage tags are not domain-separated\n";
    return 1;
  }

  // Hand-derived QR: A = [[0,1],[1,0]] (already orthogonal) resolves via
  // the documented sign convention (reflection sign opposite the pivot) to
  // Q = [[0,1],[-1,0]], verified by hand: Q R with R = [[-1,0],[0,1]]
  // reproduces A. Tolerance-based because Householder QR takes a sqrt.
  const auto handQr =
      householderQrOrthogonalize(std::vector<double>{0.0, 1.0, 1.0, 0.0}, 2);
  if (!handQr.has_value()) {
    std::cerr << "householderQrOrthogonalize rejected a nonsingular matrix\n";
    return 1;
  }
  const std::vector<double> expectedHandQr{0.0, 1.0, -1.0, 0.0};
  for (std::size_t index = 0; index < expectedHandQr.size(); ++index) {
    if (!close((*handQr)[index], expectedHandQr[index], 1e-9)) {
      std::cerr << "householderQrOrthogonalize did not match the "
                   "hand-derived sign convention\n";
      return 1;
    }
  }

  // Singular: an exactly-zero first column has no reflection direction.
  if (householderQrOrthogonalize(std::vector<double>{0.0, 1.0, 0.0, 1.0}, 2)
          .has_value()) {
    std::cerr << "householderQrOrthogonalize accepted a singular column\n";
    return 1;
  }
  if (householderQrOrthogonalize(std::vector<double>{0.0, 0.0, 0.0, 0.0}, 2)
          .has_value()) {
    std::cerr << "householderQrOrthogonalize accepted an all-zero matrix\n";
    return 1;
  }

  // The end-to-end RandomOrthogonal construction is orthogonal for several
  // Channel counts, including non-powers-of-two, and is repeat-deterministic.
  for (const auto channels : {2U, 3U, 5U, 9U}) {
    const auto resolved = resolveRandomOrthogonalMatrix(
        channels, 7, kMixMatrixRandomOrthogonalUsage);
    if (!resolved.has_value()) {
      std::cerr << "resolveRandomOrthogonalMatrix(" << channels
                << ", 7) unexpectedly reported a singular construction\n";
      return 1;
    }
    if (!isOrthogonal(*resolved, channels, 1e-9)) {
      std::cerr << "resolveRandomOrthogonalMatrix(" << channels
                << ", 7) is not orthogonal\n";
      return 1;
    }
    if (resolveRandomOrthogonalMatrix(
            channels, 7, kMixMatrixRandomOrthogonalUsage) != resolved) {
      std::cerr << "resolveRandomOrthogonalMatrix(" << channels
                << ", 7) was not repeat-deterministic\n";
      return 1;
    }
  }

  // The Main Downmix's own usage tag resolves a genuinely different, still
  // orthonormal matrix at the same (channels, seed) -- branch separation
  // survives the full fill-then-QR construction, not just the dense fill
  // (already checked above). Rows 0 and 1 match the independently derived
  // Q from expectedMainDownmixFill's own fill (hand-verified Householder
  // QR, same sign convention).
  const auto mainDownmixResolved =
      resolveRandomOrthogonalMatrix(2, 7, kMainDownmixUsage);
  if (!mainDownmixResolved.has_value()) {
    std::cerr << "resolveRandomOrthogonalMatrix(2, 7, MAINDNMX) "
                 "unexpectedly reported a singular construction\n";
    return 1;
  }
  if (!isOrthogonal(*mainDownmixResolved, 2, 1e-9)) {
    std::cerr << "resolveRandomOrthogonalMatrix(2, 7, MAINDNMX) is not "
                 "orthogonal\n";
    return 1;
  }
  const std::vector<double> expectedMainDownmixQ{
      -0.10085801681611817,
      -0.9949008294518201,
      0.9949008294518201,
      -0.10085801681611806,
  };
  for (std::size_t index = 0; index < expectedMainDownmixQ.size(); ++index) {
    if (!close(
            (*mainDownmixResolved)[index], expectedMainDownmixQ[index],
            1e-9)) {
      std::cerr << "resolveRandomOrthogonalMatrix(2, 7, MAINDNMX) did not "
                   "match the independently derived fixed vector\n";
      return 1;
    }
  }
  if (*mainDownmixResolved ==
      *resolveRandomOrthogonalMatrix(2, 7, kMixMatrixRandomOrthogonalUsage)) {
    std::cerr << "MAINDNMX and MIXORTHO resolved the same orthogonal "
                 "matrix at the same (channels, seed)\n";
    return 1;
  }

  return 0;
}
