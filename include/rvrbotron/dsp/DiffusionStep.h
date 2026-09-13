#pragma once

#include "rvrbotron/dsp/DelayLine.h"
#include "rvrbotron/dsp/Modulation.h"
#include "rvrbotron/dsp/ResolvedConfig.h"
#include "rvrbotron/dsp/Sample.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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

  std::optional<Modulation> modulation_;
  ModulationInterpolation interpolation_ = ModulationInterpolation::lagrange3;
  std::vector<Sample> allpassState_;
  std::vector<std::size_t> allpassStateIndex_;
};

} // namespace rvrbotron::dsp
