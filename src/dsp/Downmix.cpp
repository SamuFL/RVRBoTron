#include "rvrbotron/dsp/Downmix.h"

namespace rvrbotron::dsp {

Downmix::Downmix(const ResolvedDownmix& config)
    : strategy_(config.strategy),
      inputChannels_(config.inputChannels),
      outputChannels_(config.outputChannels),
      compensation_(static_cast<Sample>(config.compensation)) {}

void Downmix::processFrame(const Sample* const channels,
                           Sample* const* const outputs,
                           const std::size_t frame) const noexcept {
  if (inputChannels_ == 1) {
    const auto value = channels[0] * compensation_;
    outputs[0][frame] = value;
    outputs[1][frame] = value;
    return;
  }
  outputs[0][frame] = channels[0] * compensation_;
  outputs[1][frame] = channels[1] * compensation_;
}

std::size_t Downmix::inputChannelCount() const noexcept {
  return inputChannels_;
}

std::size_t Downmix::outputChannelCount() const noexcept {
  return outputChannels_;
}

std::size_t Downmix::ownedBytes() const noexcept {
  return sizeof(*this);
}

} // namespace rvrbotron::dsp
