#include "rvrbotron/dsp/MixMatrix.h"

#include "rvrbotron/dsp/OwnedBytes.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace rvrbotron::dsp {
namespace {

bool hasOddParity(std::size_t value) noexcept {
  bool odd = false;
  while (value != 0) {
    odd = !odd;
    value &= value - 1;
  }
  return odd;
}

} // namespace

HadamardMixMatrix::HadamardMixMatrix(
    const std::size_t channels,
    const std::vector<double>& resolvedCoefficients)
    : channels_(channels),
      scale_(0) {
  if (channels_ == 0 || (channels_ & (channels_ - 1)) != 0) {
    throw std::invalid_argument(
        "Hadamard MixMatrix requires a power-of-two Channel count");
  }
  if (channels_ > std::numeric_limits<std::size_t>::max() / channels_ ||
      resolvedCoefficients.size() != channels_ * channels_) {
    throw std::invalid_argument(
        "Hadamard MixMatrix requires an N by N coefficient matrix");
  }

  const auto resolvedScale = resolvedCoefficients.front();
  const auto expectedScale =
      1.0 / std::sqrt(static_cast<double>(channels_));
  const auto scaleTolerance =
      4.0 * std::numeric_limits<double>::epsilon() * expectedScale;
  if (!std::isfinite(resolvedScale) || resolvedScale <= 0.0 ||
      std::abs(resolvedScale - expectedScale) > scaleTolerance) {
    throw std::invalid_argument(
        "Hadamard MixMatrix requires normalized resolved coefficients");
  }
  for (std::size_t row = 0; row < channels_; ++row) {
    for (std::size_t column = 0; column < channels_; ++column) {
      const auto expected =
          hasOddParity(row & column) ? -resolvedScale : resolvedScale;
      if (resolvedCoefficients[row * channels_ + column] != expected) {
        throw std::invalid_argument(
            "Hadamard MixMatrix requires canonical resolved coefficients");
      }
    }
  }
  scale_ = static_cast<Sample>(resolvedScale);
}

void HadamardMixMatrix::mix(Sample* const channels) const noexcept {
  for (std::size_t width = 1; width < channels_; width *= 2) {
    const auto stride = width * 2;
    for (std::size_t offset = 0; offset < channels_; offset += stride) {
      for (std::size_t index = 0; index < width; ++index) {
        const auto left = channels[offset + index];
        const auto right = channels[offset + width + index];
        channels[offset + index] = left + right;
        channels[offset + width + index] = left - right;
      }
    }
  }
  for (std::size_t channel = 0; channel < channels_; ++channel) {
    channels[channel] *= scale_;
  }
}

std::size_t HadamardMixMatrix::channelCount() const noexcept {
  return channels_;
}

std::size_t HadamardMixMatrix::ownedBytes() const noexcept {
  return sizeof(*this);
}

HouseholderMixMatrix::HouseholderMixMatrix(
    const std::size_t channels,
    const std::vector<double>& resolvedCoefficients)
    : channels_(channels),
      twoOverChannels_(0) {
  if (channels_ == 0) {
    throw std::invalid_argument(
        "Householder MixMatrix requires at least one Channel");
  }
  if (resolvedCoefficients.size() != channels_ * channels_) {
    throw std::invalid_argument(
        "Householder MixMatrix requires an N by N coefficient matrix");
  }

  const auto expectedOffDiagonal = -2.0 / static_cast<double>(channels_);
  const auto expectedDiagonal = 1.0 + expectedOffDiagonal;
  const auto tolerance =
      4.0 * std::numeric_limits<double>::epsilon() *
      std::max(1.0, std::abs(expectedOffDiagonal));
  for (std::size_t row = 0; row < channels_; ++row) {
    for (std::size_t column = 0; column < channels_; ++column) {
      const auto expected =
          row == column ? expectedDiagonal : expectedOffDiagonal;
      const auto actual = resolvedCoefficients[row * channels_ + column];
      if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        throw std::invalid_argument(
            "Householder MixMatrix requires canonical resolved "
            "coefficients");
      }
    }
  }
  twoOverChannels_ = static_cast<Sample>(1.0 - resolvedCoefficients.front());
}

void HouseholderMixMatrix::mix(Sample* const channels) const noexcept {
  Sample sum = 0;
  for (std::size_t channel = 0; channel < channels_; ++channel) {
    sum += channels[channel];
  }
  const auto term = twoOverChannels_ * sum;
  for (std::size_t channel = 0; channel < channels_; ++channel) {
    channels[channel] -= term;
  }
}

std::size_t HouseholderMixMatrix::channelCount() const noexcept {
  return channels_;
}

std::size_t HouseholderMixMatrix::ownedBytes() const noexcept {
  return sizeof(*this);
}

namespace {

constexpr double kRandomOrthogonalTolerance = 1e-9;

} // namespace

RandomOrthogonalMixMatrix::RandomOrthogonalMixMatrix(
    const std::size_t channels,
    const std::vector<double>& resolvedCoefficients)
    : channels_(channels), scratch_(channels, Sample{0}) {
  if (channels_ == 0) {
    throw std::invalid_argument(
        "RandomOrthogonal MixMatrix requires at least one Channel");
  }
  if (resolvedCoefficients.size() != channels_ * channels_) {
    throw std::invalid_argument(
        "RandomOrthogonal MixMatrix requires an N by N coefficient matrix");
  }

  for (std::size_t rowA = 0; rowA < channels_; ++rowA) {
    for (std::size_t rowB = 0; rowB < channels_; ++rowB) {
      double dot = 0.0;
      for (std::size_t column = 0; column < channels_; ++column) {
        dot += resolvedCoefficients[rowA * channels_ + column] *
               resolvedCoefficients[rowB * channels_ + column];
      }
      const auto expected = rowA == rowB ? 1.0 : 0.0;
      if (!std::isfinite(dot) ||
          std::abs(dot - expected) > kRandomOrthogonalTolerance) {
        throw std::invalid_argument(
            "RandomOrthogonal MixMatrix requires orthogonal resolved "
            "coefficients");
      }
    }
  }

  matrix_.reserve(resolvedCoefficients.size());
  for (const auto coefficient : resolvedCoefficients) {
    matrix_.push_back(static_cast<Sample>(coefficient));
  }
}

void RandomOrthogonalMixMatrix::mix(Sample* const channels) const noexcept {
  for (std::size_t row = 0; row < channels_; ++row) {
    Sample sum = 0;
    const auto rowOffset = row * channels_;
    for (std::size_t column = 0; column < channels_; ++column) {
      sum += matrix_[rowOffset + column] * channels[column];
    }
    scratch_[row] = sum;
  }
  for (std::size_t row = 0; row < channels_; ++row) {
    channels[row] = scratch_[row];
  }
}

std::size_t RandomOrthogonalMixMatrix::channelCount() const noexcept {
  return channels_;
}

std::size_t RandomOrthogonalMixMatrix::ownedBytes() const noexcept {
  return sizeof(*this) + ownedVectorBytes(matrix_) +
         ownedVectorBytes(scratch_);
}

std::unique_ptr<MixMatrix> makeMixMatrix(
    const MixMatrixType type,
    const std::size_t channels,
    const std::vector<double>& resolvedCoefficients) {
  switch (type) {
  case MixMatrixType::hadamard:
    return std::make_unique<HadamardMixMatrix>(
        channels, resolvedCoefficients);
  case MixMatrixType::householder:
    return std::make_unique<HouseholderMixMatrix>(
        channels, resolvedCoefficients);
  case MixMatrixType::randomOrthogonal:
    return std::make_unique<RandomOrthogonalMixMatrix>(
        channels, resolvedCoefficients);
  }
  throw std::invalid_argument("unsupported MixMatrix type");
}

} // namespace rvrbotron::dsp
