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

class FeedbackLoop {
public:
  explicit FeedbackLoop(const ResolvedFeedbackLoop& config);
  ~FeedbackLoop();

  void processFrame(const Sample* inputs, Sample* outputs) noexcept;

  [[nodiscard]] std::size_t channelCount() const noexcept;
  [[nodiscard]] std::uint64_t tailBudgetSamples() const noexcept;
  [[nodiscard]] std::uint64_t blockSizeBoundSamples() const noexcept;
  [[nodiscard]] std::size_t ownedBytes() const noexcept;

private:
  std::size_t channels_;
  std::uint64_t tailBudgetSamples_;
  std::uint64_t blockSizeBoundSamples_;

  DelayLine delayLine_;

  std::vector<Sample> gains_;
  std::unique_ptr<MixMatrix> mix_;

  std::vector<Sample> fedBack_;

  bool dampingEnabled_ = false;
  bool highShelfBypassed_ = true;
  std::vector<Sample> highShelfB0_;
  std::vector<Sample> highShelfB1_;
  std::vector<Sample> highShelfA1_;
  std::vector<Sample> highShelfPrevInput_;
  std::vector<Sample> highShelfPrevOutput_;
  bool lowShelfBypassed_ = true;
  std::vector<Sample> lowShelfB0_;
  std::vector<Sample> lowShelfB1_;
  std::vector<Sample> lowShelfA1_;
  std::vector<Sample> lowShelfPrevInput_;
  std::vector<Sample> lowShelfPrevOutput_;

  std::optional<Modulation> modulation_;
  ModulationInterpolation interpolation_ = ModulationInterpolation::lagrange3;
  std::vector<Sample> allpassState_;
  std::vector<std::size_t> allpassStateIndex_;
};

} // namespace rvrbotron::dsp
