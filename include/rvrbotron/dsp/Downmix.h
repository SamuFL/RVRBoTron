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
  // The compensation-scaled row each output reads: a dense dot product
  // over every Channel, uniform across strategies (`select`'s rows are
  // just one-hot) rather than an indexed fast path -- see issue #108.
  // Allocated once at construction, never resized in processFrame.
  std::vector<Sample> effectiveLeftRow_;
  std::vector<Sample> effectiveRightRow_;
};

} // namespace rvrbotron::dsp
