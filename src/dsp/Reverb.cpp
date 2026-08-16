#include "rvrbotron/dsp/Reverb.h"

namespace rvrbotron::dsp {

Reverb::Reverb(const ResolvedConfig&) noexcept {}

void Reverb::process(Sample* const*,
                     const std::size_t channelCount,
                     const std::size_t frameCount) noexcept {
  static_cast<void>(channelCount);
  static_cast<void>(frameCount);
}

} // namespace rvrbotron::dsp
