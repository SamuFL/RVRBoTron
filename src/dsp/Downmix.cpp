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
              : toSampleRow(config.effectiveRightRow)),
      widthMatrix_{
          static_cast<Sample>(config.widthMatrix[0]),
          static_cast<Sample>(config.widthMatrix[1]),
          static_cast<Sample>(config.widthMatrix[2]),
          static_cast<Sample>(config.widthMatrix[3])},
      identityWidth_(config.widthDeg == 90.0) {}

void Downmix::processFrame(const Sample* const channels,
                           Sample* const* const outputs,
                           const std::size_t frame) const noexcept {
  Sample preWidthLeft;
  Sample preWidthRight;
  if (strategy_ == DownmixStrategy::select) {
    preWidthLeft = channels[leftChannel_] * compensation_;
    preWidthRight = channels[rightChannel_] * compensation_;
  } else {
    Sample left{0};
    Sample right{0};
    for (std::size_t channel = 0; channel < inputChannels_; ++channel) {
      left += channels[channel] * effectiveLeftRow_[channel];
      right += channels[channel] * effectiveRightRow_[channel];
    }
    preWidthLeft = left;
    preWidthRight = right;
  }
  if (identityWidth_) {
    outputs[0][frame] = preWidthLeft;
    outputs[1][frame] = preWidthRight;
    return;
  }
  outputs[0][frame] =
      widthMatrix_[0] * preWidthLeft + widthMatrix_[1] * preWidthRight;
  outputs[1][frame] =
      widthMatrix_[2] * preWidthLeft + widthMatrix_[3] * preWidthRight;
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
