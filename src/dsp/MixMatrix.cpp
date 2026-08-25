#include "rvrbotron/dsp/MixMatrix.h"

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

std::unique_ptr<MixMatrix> makeMixMatrix(
    const MixMatrixType type,
    const std::size_t channels,
    const std::vector<double>& resolvedCoefficients) {
  switch (type) {
  case MixMatrixType::hadamard:
    return std::make_unique<HadamardMixMatrix>(
        channels, resolvedCoefficients);
  }
  throw std::invalid_argument("unsupported MixMatrix type");
}

} // namespace rvrbotron::dsp
