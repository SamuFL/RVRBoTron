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
  // #109) so processFrame never computes trigonometry. At the default
  // 90 degrees this is an exact identity, so every existing strategy's
  // pre-Width output is unchanged bit-for-bit.
  std::array<Sample, 4> widthMatrix_;
};

} // namespace rvrbotron::dsp
