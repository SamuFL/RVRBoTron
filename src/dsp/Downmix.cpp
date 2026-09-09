#include "rvrbotron/dsp/Downmix.h"

#include "rvrbotron/dsp/OwnedBytes.h"

namespace rvrbotron::dsp {

namespace {

std::vector<Sample> toSampleRow(const std::vector<double>& row) {
  std::vector<Sample> converted(row.size());
  for (std::size_t index = 0; index < row.size(); ++index) {
    converted[index] = static_cast<Sample>(row[index]);
  }
  return converted;
}

} // namespace

Downmix::Downmix(const ResolvedDownmix& config)
    : strategy_(config.strategy),
      inputChannels_(config.inputChannels),
      outputChannels_(config.outputChannels),
      leftChannel_(config.leftChannel.value_or(0)),
      rightChannel_(
          config.rightChannel.value_or(config.leftChannel.value_or(0))),
      compensation_(static_cast<Sample>(config.compensation)),
      effectiveLeftRow_(
          config.strategy == DownmixStrategy::select
              ? std::vector<Sample>()
              : toSampleRow(config.effectiveLeftRow)),
      effectiveRightRow_(
          config.strategy == DownmixStrategy::select
              ? std::vector<Sample>()
              : toSampleRow(config.effectiveRightRow)) {}

void Downmix::processFrame(const Sample* const channels,
                           Sample* const* const outputs,
                           const std::size_t frame) const noexcept {
  if (strategy_ == DownmixStrategy::select) {
    outputs[0][frame] = channels[leftChannel_] * compensation_;
    outputs[1][frame] = channels[rightChannel_] * compensation_;
    return;
  }
  Sample left{0};
  Sample right{0};
  for (std::size_t channel = 0; channel < inputChannels_; ++channel) {
    left += channels[channel] * effectiveLeftRow_[channel];
    right += channels[channel] * effectiveRightRow_[channel];
  }
  outputs[0][frame] = left;
  outputs[1][frame] = right;
}

std::size_t Downmix::inputChannelCount() const noexcept {
  return inputChannels_;
}

std::size_t Downmix::outputChannelCount() const noexcept {
  return outputChannels_;
}

std::size_t Downmix::ownedBytes() const noexcept {
  return sizeof(*this) + ownedVectorBytes(effectiveLeftRow_) +
         ownedVectorBytes(effectiveRightRow_);
}

} // namespace rvrbotron::dsp
