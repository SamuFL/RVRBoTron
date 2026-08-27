#include "rvrbotron/dsp/Split.h"

#include "rvrbotron/dsp/SplitStrategy.h"

namespace rvrbotron::dsp {

Split::Split(const ResolvedSplit& config)
    : strategy_(makeSplitStrategy(config)) {}

Split::~Split() = default;

void Split::processFrame(const Sample* const* inputs,
                         const std::size_t frame,
                         Sample* const channels) const noexcept {
  strategy_->processFrame(inputs, frame, channels);
}

std::size_t Split::inputChannelCount() const noexcept {
  return strategy_->inputChannelCount();
}

std::size_t Split::channelCount() const noexcept {
  return strategy_->channelCount();
}

std::size_t Split::ownedBytes() const noexcept {
  return sizeof(*this) + strategy_->ownedBytes();
}

} // namespace rvrbotron::dsp
