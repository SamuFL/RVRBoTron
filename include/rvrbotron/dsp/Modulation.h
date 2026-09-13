#pragma once

#include "rvrbotron/dsp/ResolvedConfig.h"
#include "rvrbotron/dsp/Sample.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rvrbotron::dsp {

class Modulation {
public:
  explicit Modulation(const ResolvedModulation& config);

  [[nodiscard]] bool isModulated(std::size_t channel) const noexcept;

  [[nodiscard]] double lookbackSamples(
      std::size_t channel, std::uint64_t nominalDelaySamples) const noexcept;

  void advanceFrame() noexcept;

  [[nodiscard]] std::size_t ownedBytes() const noexcept;

private:
  std::vector<std::uint64_t> channelSeeds_;
  std::vector<double> channelTargetsPerSample_;
  std::vector<double> channelPhases_;
  std::vector<bool> channelModulated_;
  ModulationShape shape_;
  double excursionSamples_;
  std::uint64_t frameIndex_ = 0;
};

} // namespace rvrbotron::dsp
