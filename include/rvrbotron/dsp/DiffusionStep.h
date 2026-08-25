#pragma once

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

private:
  std::size_t channels_;

  std::vector<std::uint64_t> delays_;
  std::vector<std::size_t> delayOffsets_;
  std::vector<std::size_t> delayPositions_;
  std::vector<Sample> delayStorage_;

  std::vector<std::uint32_t> permutation_;
  std::vector<Sample> polarity_;
  std::unique_ptr<MixMatrix> mix_;

  std::vector<Sample> delayedValues_;
};

} // namespace rvrbotron::dsp
