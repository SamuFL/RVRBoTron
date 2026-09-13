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
  [[nodiscard]] std::size_t ownedStorageBytes() const noexcept;

private:
  DownmixStrategy strategy_;
  std::size_t inputChannels_;
  std::size_t outputChannels_;
  std::size_t leftChannel_;
  std::size_t rightChannel_;
  Sample compensation_;
  std::vector<Sample> effectiveLeftRow_;
  std::vector<Sample> effectiveRightRow_;
  std::array<Sample, 4> widthMatrix_;
  bool identityWidth_;
};

} // namespace rvrbotron::dsp
