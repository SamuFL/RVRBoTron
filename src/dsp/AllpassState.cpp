#include "rvrbotron/dsp/AllpassState.h"

namespace rvrbotron::dsp {

void buildAllpassState(
    const std::vector<bool>& channelModulated,
    std::vector<Sample>& state,
    std::vector<std::size_t>& index) {
  index.assign(channelModulated.size(), 0);
  std::size_t modulatedCount = 0;
  for (std::size_t channel = 0; channel < channelModulated.size(); ++channel) {
    if (channelModulated[channel]) {
      index[channel] = modulatedCount;
      ++modulatedCount;
    }
  }
  state.assign(modulatedCount, Sample{0});
}

} // namespace rvrbotron::dsp
