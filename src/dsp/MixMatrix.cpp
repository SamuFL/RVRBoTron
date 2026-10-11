#include "rvrbotron/dsp/MixMatrix.h"

#include "rvrbotron/dsp/OwnedBytes.h"
#include "rvrbotron/dsp/ProcessChunk.h"

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

void HadamardMixMatrix::mixBlock(Sample* const channels,
                                 const std::size_t frames) const noexcept {
  // Butterfly stages of widths 1, 2, 4, ... in order. Each runs across the
  // whole chunk, so it vectorises over time, and pairs of stages share one
  // pass over memory (radix 4), with the scale folded into the last pass.
  // Every sample still sees the same additions, subtractions, and final
  // scale, in the same order, as a frame-by-frame transform.
  std::size_t width = 1;
  while (width < channels_) {
    const bool radix4 = width * 4 <= channels_;
    const auto span = width * (radix4 ? 4 : 2);
    const bool last = span == channels_;
    for (std::size_t offset = 0; offset < channels_; offset += span) {
      for (std::size_t index = 0; index < width; ++index) {
        auto* const a = channels + (offset + index) * frames;
        auto* const b = channels + (offset + width + index) * frames;
        if (!radix4) {
          for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto left = a[frame];
            const auto right = b[frame];
            a[frame] = last ? (left + right) * scale_ : left + right;
            b[frame] = last ? (left - right) * scale_ : left - right;
          }
          continue;
        }
        auto* const c = channels + (offset + 2 * width + index) * frames;
        auto* const d = channels + (offset + 3 * width + index) * frames;
        for (std::size_t frame = 0; frame < frames; ++frame) {
          // Stage `width` pairs (a, b) and (c, d); stage 2 * width pairs
          // their results (a, c) and (b, d).
          const auto ab0 = a[frame] + b[frame];
          const auto ab1 = a[frame] - b[frame];
          const auto cd0 = c[frame] + d[frame];
          const auto cd1 = c[frame] - d[frame];
          const auto outA = ab0 + cd0;
          const auto outB = ab1 + cd1;
          const auto outC = ab0 - cd0;
          const auto outD = ab1 - cd1;
          a[frame] = last ? outA * scale_ : outA;
          b[frame] = last ? outB * scale_ : outB;
          c[frame] = last ? outC * scale_ : outC;
          d[frame] = last ? outD * scale_ : outD;
        }
      }
    }
    width = span;
  }
  if (channels_ == 1) {
    for (std::size_t frame = 0; frame < frames; ++frame) {
      channels[frame] *= scale_;
    }
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

void HouseholderMixMatrix::mixBlock(Sample* const channels,
                                    const std::size_t frames) const noexcept {
  // Sums the Channels of each frame in Channel order, as the
  // frame-by-frame reflection does, so the result is bit-identical.
  Sample sums[kMaxChunkFrames];
  std::fill_n(sums, frames, Sample{0});
  for (std::size_t channel = 0; channel < channels_; ++channel) {
    const auto* const values = channels + channel * frames;
    for (std::size_t frame = 0; frame < frames; ++frame) {
      sums[frame] += values[frame];
    }
  }
  for (std::size_t frame = 0; frame < frames; ++frame) {
    sums[frame] = twoOverChannels_ * sums[frame];
  }
  for (std::size_t channel = 0; channel < channels_; ++channel) {
    auto* const values = channels + channel * frames;
    for (std::size_t frame = 0; frame < frames; ++frame) {
      values[frame] -= sums[frame];
    }
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
    : channels_(channels), scratch_(channels * kMaxChunkFrames, Sample{0}) {
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

void RandomOrthogonalMixMatrix::mixBlock(
    Sample* const channels, const std::size_t frames) const noexcept {
  // Each row accumulates its frames in column order. As in Downmix, the
  // product is its own statement so it is never fused into the sum.
  for (std::size_t row = 0; row < channels_; ++row) {
    auto* const sums = scratch_.data() + row * frames;
    std::fill_n(sums, frames, Sample{0});
    const auto rowOffset = row * channels_;
    for (std::size_t column = 0; column < channels_; ++column) {
      const auto coefficient = matrix_[rowOffset + column];
      const auto* const values = channels + column * frames;
      for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto product = coefficient * values[frame];
        sums[frame] += product;
      }
    }
  }
  std::copy_n(scratch_.data(), channels_ * frames, channels);
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
