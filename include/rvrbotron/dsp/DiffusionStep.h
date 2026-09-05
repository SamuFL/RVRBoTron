#pragma once

#include "rvrbotron/dsp/DelayLine.h"
#include "rvrbotron/dsp/ResolvedConfig.h"
#include "rvrbotron/dsp/Sample.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace rvrbotron::dsp {

class MixMatrix;

class DiffusionStep {
public:
  explicit DiffusionStep(const ResolvedDiffusionStep& config);
  ~DiffusionStep();

  void processFrame(const Sample* inputs, Sample* outputs) noexcept;

  [[nodiscard]] std::size_t channelCount() const noexcept;
  [[nodiscard]] std::size_t ownedBytes() const noexcept;

private:
  std::size_t channels_;

  DelayLine delayLine_;

  std::vector<std::uint32_t> permutation_;
  std::vector<Sample> polarity_;
  std::unique_ptr<MixMatrix> mix_;

  std::vector<Sample> delayedValues_;
};

} // namespace rvrbotron::dsp
