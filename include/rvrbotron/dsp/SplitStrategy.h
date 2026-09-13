#pragma once

#include "rvrbotron/dsp/ResolvedConfig.h"
#include "rvrbotron/dsp/Sample.h"

#include <cstddef>
#include <memory>

namespace rvrbotron::dsp {

class SplitStrategy {
public:
  virtual ~SplitStrategy() = default;

  virtual void processFrame(const Sample* const* inputs,
                            std::size_t frame,
                            Sample* channels) const noexcept = 0;
  [[nodiscard]] virtual std::size_t inputChannelCount() const noexcept = 0;
  [[nodiscard]] virtual std::size_t channelCount() const noexcept = 0;
  [[nodiscard]] virtual std::size_t ownedBytes() const noexcept = 0;
};

class DuplicateSplitStrategy final : public SplitStrategy {
public:
  explicit DuplicateSplitStrategy(const ResolvedSplit& config);

  void processFrame(const Sample* const* inputs,
                    std::size_t frame,
                    Sample* channels) const noexcept override;
  [[nodiscard]] std::size_t inputChannelCount() const noexcept override;
  [[nodiscard]] std::size_t channelCount() const noexcept override;
  [[nodiscard]] std::size_t ownedBytes() const noexcept override;

private:
  std::size_t inputChannels_;
  std::size_t channels_;
  Sample sourceGain_;
  Sample channelGain_;
};

class StereoHalvesSplitStrategy final : public SplitStrategy {
public:
  explicit StereoHalvesSplitStrategy(const ResolvedSplit& config);

  void processFrame(const Sample* const* inputs,
                    std::size_t frame,
                    Sample* channels) const noexcept override;
  [[nodiscard]] std::size_t inputChannelCount() const noexcept override;
  [[nodiscard]] std::size_t channelCount() const noexcept override;
  [[nodiscard]] std::size_t ownedBytes() const noexcept override;

private:
  std::size_t channels_;
  std::size_t half_;
  Sample channelGain_;
};

class StereoInterleaveSplitStrategy final : public SplitStrategy {
public:
  explicit StereoInterleaveSplitStrategy(const ResolvedSplit& config);

  void processFrame(const Sample* const* inputs,
                    std::size_t frame,
                    Sample* channels) const noexcept override;
  [[nodiscard]] std::size_t inputChannelCount() const noexcept override;
  [[nodiscard]] std::size_t channelCount() const noexcept override;
  [[nodiscard]] std::size_t ownedBytes() const noexcept override;

private:
  std::size_t channels_;
  Sample channelGain_;
};

std::unique_ptr<SplitStrategy> makeSplitStrategy(
    const ResolvedSplit& config);

} // namespace rvrbotron::dsp
