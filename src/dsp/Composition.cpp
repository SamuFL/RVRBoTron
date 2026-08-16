#include "rvrbotron/dsp/Composition.h"

namespace rvrbotron::dsp {

Composition::Composition(const ResolvedConfig&) noexcept {}

void Composition::process(Sample* const*,
                          const std::size_t channelCount,
                          const std::size_t frameCount) noexcept {
  static_cast<void>(channelCount);
  static_cast<void>(frameCount);
}

} // namespace rvrbotron::dsp
