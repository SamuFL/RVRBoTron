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

  // Not const: smoothed-random trajectories cache their Catmull-Rom
  // targets per Channel.
  [[nodiscard]] double lookbackSamples(
      std::size_t channel, std::uint64_t nominalDelaySamples) noexcept;

  void advanceFrame() noexcept;

  [[nodiscard]] std::size_t ownedBytes() const noexcept;

private:
  // The four Catmull-Rom targets around one target index. A trajectory
  // only crosses to the next index every 1 / channelTargetsPerSample
  // frames (seconds, at typical rates), so caching them removes almost all
  // per-frame hashing without changing a single value (issue #163).
  struct SmoothedRandomTargets {
    std::int64_t index = 0;
    bool valid = false;
    double points[4] = {};
  };

  [[nodiscard]] double smoothedRandomTrajectory(
      std::size_t channel, double t) noexcept;

  std::vector<std::uint64_t> channelSeeds_;
  std::vector<double> channelTargetsPerSample_;
  std::vector<double> channelPhases_;
  std::vector<bool> channelModulated_;
  ModulationShape shape_;
  double excursionSamples_;
  std::uint64_t frameIndex_ = 0;
  std::vector<SmoothedRandomTargets> smoothedRandomTargets_;
};

} // namespace rvrbotron::dsp
