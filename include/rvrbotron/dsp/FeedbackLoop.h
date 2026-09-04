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

  // Two-shelf Damping (see docs/design/reverb/stages/05-damping.md and
  // issues #75/#76): independent per-Channel high and low shelves applied,
  // in that order, after decay gain and before mixing, on every
  // circulation. Disabled when the Resolved Configuration carries no
  // Damping. Each `*Bypassed_` flag skips its own section's arithmetic and
  // state entirely at a unity ratio, so output stays bit-identical to
  // Damping disabled (or to the other section alone) rather than merely
  // relying on the coefficients simplifying to identity under rounding.
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
};

} // namespace rvrbotron::dsp
