#include "rvrbotron/dsp/Downmix.h"

namespace rvrbotron::dsp {

Downmix::Downmix(const ResolvedDownmix& config)
    : strategy_(config.strategy),
      inputChannels_(config.inputChannels),
      outputChannels_(config.outputChannels),
      leftChannel_(config.leftChannel),
      rightChannel_(config.rightChannel.value_or(config.leftChannel)),
      compensation_(static_cast<Sample>(config.compensation)) {}

void Downmix::processFrame(const Sample* const channels,
                           Sample* const* const outputs,
                           const std::size_t frame) const noexcept {
  const auto left = channels[leftChannel_] * compensation_;
  outputs[0][frame] = left;
  outputs[1][frame] =
      rightChannel_ == leftChannel_ ? left : channels[rightChannel_] * compensation_;
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
