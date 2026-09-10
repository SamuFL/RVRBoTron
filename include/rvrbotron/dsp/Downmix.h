#pragma once

#include "rvrbotron/dsp/ResolvedConfig.h"
#include "rvrbotron/dsp/Sample.h"

#include <array>
#include <cstddef>
#include <vector>

namespace rvrbotron::dsp {

class Downmix {
public:
  explicit Downmix(const ResolvedDownmix& config);

  void processFrame(const Sample* channels,
                    Sample* const* outputs,
                    std::size_t frame) const noexcept;

  [[nodiscard]] std::size_t inputChannelCount() const noexcept;
  [[nodiscard]] std::size_t outputChannelCount() const noexcept;
  [[nodiscard]] std::size_t ownedBytes() const noexcept;
  // The backing-vector allocations only, mirroring DelayLine::
  // ownedStorageBytes()/Modulation::ownedBytes(): a containing object
  // that embeds a Downmix by value (rather than through a heap pointer,
  // like Reverb's own unique_ptr<Downmix>) already counts this object's
  // in-place storage through its own sizeof(*this), so adding
  // ownedBytes()'s sizeof(*this) there too would double-count it -- see
  // EarlyReflections::ownedBytes() (issue #111).
  [[nodiscard]] std::size_t ownedStorageBytes() const noexcept;

private:
  DownmixStrategy strategy_;
  std::size_t inputChannels_;
  std::size_t outputChannels_;
  // `select`'s O(1) fast path: its rows are one-hot, so processFrame reads
  // a single indexed Channel per output rather than paying for a dense
  // N-wide dot product over mostly-zero coefficients (issue #108).
  std::size_t leftChannel_;
  std::size_t rightChannel_;
  Sample compensation_;
  // Every other strategy's dense rows (empty for `select`, which never
  // reads them). Allocated once at construction, never resized in
  // processFrame.
  std::vector<Sample> effectiveLeftRow_;
  std::vector<Sample> effectiveRightRow_;
  // The resolved 2x2 Width matrix (docs/design/reverb/stages/
  // 08-downmix.md's "Width as a constant-power mid/side law"), applied to
  // the pre-Width [left, right] vector as row-major
  // [[m00, m01], [m10, m11]] -- resolved once before construction (issue
  // #109) so processFrame never computes trigonometry.
  std::array<Sample, 4> widthMatrix_;
  // True at the default 90 degrees: processFrame assigns the pre-Width
  // samples directly rather than multiplying through widthMatrix_ (an
  // algebraic identity there), so the bypass is exact for every input,
  // including signed zero -- `1*(-0.0) + 0*(+0.0)` rounds to `+0.0` under
  // IEEE 754, not `-0.0`, which the matrix form alone would not catch.
  bool identityWidth_;
};

} // namespace rvrbotron::dsp
