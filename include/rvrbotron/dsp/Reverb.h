#pragma once

#include "rvrbotron/dsp/ResolvedConfig.h"
#include "rvrbotron/dsp/Sample.h"

#include <cstddef>

namespace rvrbotron::dsp {

class Reverb {
public:
  explicit Reverb(const ResolvedConfig& config) noexcept;

  void process(Sample* const* channels,
               std::size_t channelCount,
               std::size_t frameCount) noexcept;
};

} // namespace rvrbotron::dsp
