#pragma once

#include "rvrbotron/dsp/ResolvedConfig.h"
#include "rvrbotron/dsp/Sample.h"

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
};

} // namespace rvrbotron::dsp
