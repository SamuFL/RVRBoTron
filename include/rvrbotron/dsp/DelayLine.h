#pragma once

#include "rvrbotron/dsp/Sample.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rvrbotron::dsp {

// A per-Channel circular delay buffer shared by every stage that circulates
// audio through a resolved delay: the Feedback Loop and Diffusion Steps
// (see docs/design/reverb/stages/06-modulation.md, issue #88). One
// instance holds every Channel's delay line in a single contiguous
// allocation, exactly mirroring the layout each stage previously
// duplicated on its own.
//
// DelayLine owns storage, wrap-around indexing, and the integer read/write
// path. It does not own read-before-write ordering: a stage that must read
// every Channel before writing any of them (the Feedback Loop) keeps that
// sequencing explicit in its own frame loop -- calling read() across every
// Channel, and only afterward calling write() across every Channel --
// rather than this class hiding it behind one combined operation.
//
// Constructed with an already-resolved length; it never derives its own
// sizing. Resolution is the only place that knows why a buffer needs to be
// as large as it is (recorded evidence, and a rejection reason at load),
// so this class only enforces the invariant a buffer must satisfy to be
// used safely: never shorter than the delay it serves.
class DelayLine {
public:
  DelayLine(
      std::vector<std::uint64_t> delaysSamples,
      std::vector<std::uint64_t> bufferSizes);

  // The sample currently at this Channel's read/write position; does not
  // advance it. Only valid when delaySamples(channel) > 0.
  [[nodiscard]] Sample read(std::size_t channel) const noexcept;

  // Writes into the slot the last read() returned for this Channel, then
  // advances that Channel's position by one, wrapping at its delay length.
  // Only valid when delaySamples(channel) > 0.
  void write(std::size_t channel, Sample value) noexcept;

  [[nodiscard]] std::uint64_t delaySamples(
      std::size_t channel) const noexcept;

  [[nodiscard]] std::size_t ownedBytes() const noexcept;

private:
  std::vector<std::uint64_t> delays_;
  std::vector<std::size_t> offsets_;
  std::vector<std::size_t> positions_;
  std::vector<Sample> storage_;
};

} // namespace rvrbotron::dsp
