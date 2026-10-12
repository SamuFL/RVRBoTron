#include "rvrbotron/dsp/Downmix.h"

#include "rvrbotron/dsp/OwnedBytes.h"

#include <algorithm>

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
  processBlock(channels, 1, outputs[0] + frame, outputs[1] + frame);
}

void Downmix::processBlock(const Sample* const channels,
                           const std::size_t frames,
                           Sample* const left,
                           Sample* const right) const noexcept {
  if (strategy_ == DownmixStrategy::select) {
    const auto* const leftValues = channels + leftChannel_ * frames;
    const auto* const rightValues = channels + rightChannel_ * frames;
    for (std::size_t frame = 0; frame < frames; ++frame) {
      left[frame] = leftValues[frame] * compensation_;
      right[frame] = rightValues[frame] * compensation_;
    }
  } else {
    // Accumulates each frame in Channel order. The product is its own
    // statement so no compiler fuses it into the sum: v1.0.0 summed
    // unfused products wherever its vectorised loop covered the Channels,
    // and a fused or unfused choice that varied with the chunk length would
    // break block-size independence (issue #163).
    std::fill_n(left, frames, Sample{0});
    std::fill_n(right, frames, Sample{0});
    for (std::size_t channel = 0; channel < inputChannels_; ++channel) {
      const auto* const values = channels + channel * frames;
      const auto leftWeight = effectiveLeftRow_[channel];
      const auto rightWeight = effectiveRightRow_[channel];
      for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto leftProduct = values[frame] * leftWeight;
        const auto rightProduct = values[frame] * rightWeight;
        left[frame] += leftProduct;
        right[frame] += rightProduct;
      }
    }
  }
  if (identityWidth_) {
    return;
  }
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const auto preWidthLeft = left[frame];
    const auto preWidthRight = right[frame];
    left[frame] =
        widthMatrix_[0] * preWidthLeft + widthMatrix_[1] * preWidthRight;
    right[frame] =
        widthMatrix_[2] * preWidthLeft + widthMatrix_[3] * preWidthRight;
  }
}

std::size_t Downmix::inputChannelCount() const noexcept {
  return inputChannels_;
}

std::size_t Downmix::outputChannelCount() const noexcept {
  return outputChannels_;
}

std::size_t Downmix::ownedBytes() const noexcept {
  return sizeof(*this) + ownedStorageBytes();
}

std::size_t Downmix::ownedStorageBytes() const noexcept {
  return ownedVectorBytes(effectiveLeftRow_) +
         ownedVectorBytes(effectiveRightRow_);
}

} // namespace rvrbotron::dsp
