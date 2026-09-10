#pragma once

#include "rvrbotron/dsp/Diffuser.h"
#include "rvrbotron/dsp/Downmix.h"
#include "rvrbotron/dsp/ResolvedConfig.h"
#include "rvrbotron/dsp/Sample.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rvrbotron::dsp {

// The parallel Early Reflections branch (issues #111/#112, docs/design/
// reverb/stages/07-early-reflections.md): owns the canonical tap set's one
// shared N-Channel accumulator and this branch's own Downmix, summed into
// the same stereo output as the Main wet path -- never fed into a
// Feedback Loop. Two-phase like every other stage: construction takes
// only already Resolved values, and every tap's own shaping gain is
// already resolved to a linear multiplier by then, so processing never
// computes `pow`.
class EarlyReflections {
public:
  explicit EarlyReflections(const ResolvedEarlyReflections& config);

  // taps_ holds raw pointers into this object's own accumulator_ (see
  // its declaration below), so a copy would leave the copy's own
  // descriptors pointing at the source's accumulator -- writing into
  // the wrong object, or into dangling storage once the source is gone
  // -- and a naive move would need every descriptor rebound to the
  // destination's own (potentially reallocated) storage. Neither copy
  // nor move has a legitimate use here: this object is always
  // constructed in place and owned through a single unique_ptr
  // (see Reverb::Implementation::early), so both are deleted outright
  // rather than implemented to work correctly for a need that does not
  // exist (PR review on #112).
  EarlyReflections(const EarlyReflections&) = delete;
  EarlyReflections& operator=(const EarlyReflections&) = delete;
  EarlyReflections(EarlyReflections&&) = delete;
  EarlyReflections& operator=(EarlyReflections&&) = delete;

  // Zeroes this frame's N-Channel accumulator.
  void beginFrame() noexcept;

  // The canonical tap descriptors to pass into Diffuser::processFrame,
  // sorted ascending by stepIndex with unique indices, and each already
  // pointing at this object's own accumulator -- resolved once at
  // construction, stable for this object's lifetime, so a caller reads
  // them fresh every frame rather than receiving a new array each call.
  [[nodiscard]] const DiffuserEarlyTap* taps() const noexcept;
  [[nodiscard]] std::size_t tapCount() const noexcept;

  // Downmixes the accumulated frame (populated by the Diffuser call this
  // frame's beginFrame() cleared it for), applies branch level, and
  // writes the resulting stereo pair into *left/*right -- this branch's
  // own contribution only, not summed into any output buffer. The
  // caller (Reverb) decides whether to capture it (issue #113's
  // early-stereo boundary) and/or sum it with the Main wet path's own
  // pair into the final stereo output.
  void processFrame(Sample* left, Sample* right) const noexcept;

  [[nodiscard]] bool enabled() const noexcept;
  [[nodiscard]] std::size_t ownedBytes() const noexcept;

private:
  bool enabled_;
  Sample gain_;
  Downmix downmix_;
  std::vector<Sample> accumulator_;
  std::vector<DiffuserEarlyTap> taps_;
};

} // namespace rvrbotron::dsp
