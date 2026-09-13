#pragma once

#include "rvrbotron/dsp/ResolvedConfig.h"
#include "rvrbotron/dsp/Sample.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace rvrbotron::dsp {

class MixMatrix {
public:
  virtual ~MixMatrix() = default;

  virtual void mix(Sample* channels) const noexcept = 0;
  [[nodiscard]] virtual std::size_t channelCount() const noexcept = 0;
  // Object storage plus any owned-container capacities on the heap block
  // this matrix occupies, in bytes.
  [[nodiscard]] virtual std::size_t ownedBytes() const noexcept = 0;
};

class HadamardMixMatrix final : public MixMatrix {
public:
  HadamardMixMatrix(std::size_t channels,
                    const std::vector<double>& resolvedCoefficients);

  void mix(Sample* channels) const noexcept override;
  [[nodiscard]] std::size_t channelCount() const noexcept override;
  [[nodiscard]] std::size_t ownedBytes() const noexcept override;

private:
  std::size_t channels_;
  Sample scale_;
};

// Normalized Householder reflection off the all-ones vector: subtracts
// twice the mean of the Channels from every Channel. Valid for any N >= 1;
// mixed in O(N) rather than the dense O(N^2) multiply, matching its
// "cheap" character.
class HouseholderMixMatrix final : public MixMatrix {
public:
  HouseholderMixMatrix(std::size_t channels,
                       const std::vector<double>& resolvedCoefficients);

  void mix(Sample* channels) const noexcept override;
  [[nodiscard]] std::size_t channelCount() const noexcept override;
  [[nodiscard]] std::size_t ownedBytes() const noexcept override;

private:
  std::size_t channels_;
  Sample twoOverChannels_;
};

// Seeded dense orthogonal matrix with no Haar-uniformity claim. Unlike
// Hadamard and Householder, it has no fast structured transform, so it is
// mixed by a dense multiply against the resolved coefficients, using a
// scratch buffer allocated once at construction.
class RandomOrthogonalMixMatrix final : public MixMatrix {
public:
  RandomOrthogonalMixMatrix(std::size_t channels,
                            const std::vector<double>& resolvedCoefficients);

  void mix(Sample* channels) const noexcept override;
  [[nodiscard]] std::size_t channelCount() const noexcept override;
  [[nodiscard]] std::size_t ownedBytes() const noexcept override;

private:
  std::size_t channels_;
  std::vector<Sample> matrix_;
  mutable std::vector<Sample> scratch_;
};

std::unique_ptr<MixMatrix> makeMixMatrix(
    MixMatrixType type,
    std::size_t channels,
    const std::vector<double>& resolvedCoefficients);

} // namespace rvrbotron::dsp
