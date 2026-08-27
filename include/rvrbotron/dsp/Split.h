#pragma once

#include "rvrbotron/dsp/ResolvedConfig.h"
#include "rvrbotron/dsp/Sample.h"

#include <cstddef>
#include <memory>

namespace rvrbotron::dsp {

class SplitStrategy;

class Split {
public:
  explicit Split(const ResolvedSplit& config);
  ~Split();

  void processFrame(const Sample* const* inputs,
                    std::size_t frame,
                    Sample* channels) const noexcept;

  [[nodiscard]] std::size_t inputChannelCount() const noexcept;
  [[nodiscard]] std::size_t channelCount() const noexcept;
  [[nodiscard]] std::size_t ownedBytes() const noexcept;

private:
  std::unique_ptr<SplitStrategy> strategy_;
};

} // namespace rvrbotron::dsp
