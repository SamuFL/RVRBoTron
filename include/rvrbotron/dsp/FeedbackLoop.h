#pragma once

#include "rvrbotron/dsp/ResolvedConfig.h"
#include "rvrbotron/dsp/Sample.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace rvrbotron::dsp {

class MixMatrix;

// The circulating delay network that gives the reverb tail its size and
// decay (see docs/design/reverb/stages/04-feedback-loop.md). Per sample:
// read each Channel's delay line, mix the gained readings, write input plus
// the mixed feedback. Read happens before write -- the one place the
// signal flow in this codebase runs backwards -- so this stays a single
// explicit loop rather than an abstraction that hides the ordering.
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

  std::vector<std::uint64_t> delays_;
  std::vector<std::size_t> delayOffsets_;
  std::vector<std::size_t> delayPositions_;
  std::vector<Sample> delayStorage_;

  std::vector<Sample> gains_;
  std::unique_ptr<MixMatrix> mix_;

  std::vector<Sample> fedBack_;

  // The first-audible Damping tracer (see docs/design/reverb/stages/
  // 05-damping.md and issue #75): a per-Channel high shelf applied after
  // decay gain and before mixing, on every circulation. Disabled when the
  // Resolved Configuration carries no Damping. `highShelfBypassed_` skips
  // the filter's arithmetic and state entirely at a unity high ratio, so
  // output stays bit-identical to Damping disabled rather than merely
  // relying on the coefficients simplifying to identity under rounding.
  bool dampingEnabled_ = false;
  bool highShelfBypassed_ = true;
  std::vector<Sample> highShelfB0_;
  std::vector<Sample> highShelfB1_;
  std::vector<Sample> highShelfA1_;
  std::vector<Sample> highShelfPrevInput_;
  std::vector<Sample> highShelfPrevOutput_;
};

} // namespace rvrbotron::dsp
