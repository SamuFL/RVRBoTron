#pragma once

#include "rvrbotron/dsp/ResolvedConfig.h"
#include "rvrbotron/dsp/Sample.h"

#include <cstddef>

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
  Sample compensation_;
};

} // namespace rvrbotron::dsp
