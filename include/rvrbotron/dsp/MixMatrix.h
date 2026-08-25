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
};

class HadamardMixMatrix final : public MixMatrix {
public:
  HadamardMixMatrix(std::size_t channels,
                    const std::vector<double>& resolvedCoefficients);

  void mix(Sample* channels) const noexcept override;
  [[nodiscard]] std::size_t channelCount() const noexcept override;

private:
  std::size_t channels_;
  Sample scale_;
};

std::unique_ptr<MixMatrix> makeMixMatrix(
    MixMatrixType type,
    std::size_t channels,
    const std::vector<double>& resolvedCoefficients);

} // namespace rvrbotron::dsp
