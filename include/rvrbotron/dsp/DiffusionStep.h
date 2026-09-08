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

  // One-shot, non-compounding delay-time movement scoped to this step
  // alone (see docs/design/reverb/stages/06-modulation.md's "Placement"
  // and issue #91) -- as distinct from the Feedback Loop's own
  // compounding Modulation. Resolution's own bypass (an omitted
  // Modulation object, an explicit zero depth, or a zero
  // channelFraction) leaves `modulation_` unconstructed, exactly
  // mirroring FeedbackLoop's own `modulation_`.
  std::optional<Modulation> modulation_;
  // Which DelayLine fractional-read method modulated Channels use (issue
  // #92): meaningful only while `modulation_` holds a value. Interpolation
  // is a property of the read, not of the trajectory Modulation generates,
  // so it lives here rather than inside Modulation itself (mirrors
  // FeedbackLoop's own `interpolation_`).
  ModulationInterpolation interpolation_ = ModulationInterpolation::lagrange3;
  // Per-Channel allpass interpolator state (issue #93): mirrors
  // FeedbackLoop's own `allpassState_`/`allpassStateIndex_` -- one
  // persistent output sample per Channel actually modulated, populated
  // only while `interpolation_` is `allpass`, so a bypassed step or a
  // Channel `channelFraction` excludes allocates none of it at all.
  std::vector<Sample> allpassState_;
  std::vector<std::size_t> allpassStateIndex_;
};

} // namespace rvrbotron::dsp
