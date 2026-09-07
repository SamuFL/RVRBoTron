#pragma once

#include "rvrbotron/dsp/ResolvedConfig.h"
#include "rvrbotron/dsp/Sample.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rvrbotron::dsp {

// Seeded, per-Channel delay-time movement inside the Feedback Loop (see
// docs/design/reverb/stages/06-modulation.md, issue #89): a smoothed-
// random trajectory per Channel -- Catmull-Rom interpolation between
// targets drawn uniformly in [-1, +1], a new target every 1/rateHz --
// reproducible from an integer target counter with no accumulated state.
// Decorrelation across Channels is structural: each Channel draws from
// its own seed (`channelSeeds`) and moves at its own resolved rate
// (`channelTargetsPerSample`, already carrying the fixed +-10% seeded
// spread), so trajectories are never pinned to a shared start value.
//
// Constructed only for a stage whose Modulation is actually active
// (depthMs > 0); the owning stage keeps the cheaper unmodulated path
// entirely separate rather than asking this class to collapse to
// identity at zero depth (see the design doc's "Identity is guaranteed
// by construction, not by arithmetic").
class Modulation {
public:
  // `config.channelSeeds` and `config.channelTargetsPerSample` must each
  // carry one entry per Channel; construct this only for a stage whose
  // Modulation is actually active (config.depthMs > 0), which is exactly
  // when resolution populates both.
  explicit Modulation(const ResolvedModulation& config);

  // This Channel's current fractional lookback -- `nominalDelaySamples`
  // plus this Channel's trajectory value (clamped to [-1, 1]; see
  // Modulation.cpp) times the resolved Excursion. Uses the frame index
  // last set by advanceFrame(); call this for every Channel before
  // advancing.
  [[nodiscard]] double lookbackSamples(
      std::size_t channel, std::uint64_t nominalDelaySamples) const noexcept;

  // Advances the shared frame counter by one sample. Call exactly once
  // per processed frame, after every Channel's lookbackSamples() for
  // that frame.
  void advanceFrame() noexcept;

  [[nodiscard]] std::size_t ownedBytes() const noexcept;

private:
  std::vector<std::uint64_t> channelSeeds_;
  std::vector<double> channelTargetsPerSample_;
  double excursionSamples_;
  std::uint64_t frameIndex_ = 0;
};

} // namespace rvrbotron::dsp
