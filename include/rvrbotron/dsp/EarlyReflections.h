#pragma once

#include "rvrbotron/dsp/Diffuser.h"
#include "rvrbotron/dsp/Downmix.h"
#include "rvrbotron/dsp/ResolvedConfig.h"
#include "rvrbotron/dsp/Sample.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rvrbotron::dsp {

// The parallel Early Reflections branch (issue #111, docs/design/reverb/
// stages/07-early-reflections.md): owns the canonical tap set's one
// N-Channel accumulator and this branch's own Downmix, summed into the
// same stereo output as the Main wet path -- never fed into a Feedback
// Loop. Two-phase like every other stage: construction takes only already
// Resolved values.
class EarlyReflections {
public:
  explicit EarlyReflections(const ResolvedEarlyReflections& config);

  // Zeroes this frame's N-Channel accumulator and returns a tap descriptor
  // for the caller to pass into Diffuser::processFrame, which accumulates
  // the configured Diffusion Step's completed post-step frame into it.
  [[nodiscard]] DiffuserEarlyTap beginFrame() noexcept;

  // Downmixes the accumulated frame (populated by the Diffuser call this
  // frame's beginFrame() tap fed), applies branch level, and adds (+=) the
  // resulting stereo pair into outputs[0][frame]/outputs[1][frame] --
  // superposition with whatever the Main wet path already wrote there.
  void processFrame(Sample* const* outputs, std::size_t frame) const noexcept;

  [[nodiscard]] bool enabled() const noexcept;
  [[nodiscard]] std::size_t ownedBytes() const noexcept;

private:
  bool enabled_;
  std::uint32_t tapStepIndex_;
  Sample gain_;
  Downmix downmix_;
  std::vector<Sample> accumulator_;
};

} // namespace rvrbotron::dsp
