#pragma once

#include "rvrbotron/dsp/ResolvedConfig.h"
#include "rvrbotron/dsp/Sample.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rvrbotron::dsp {

// Seeded, per-Channel delay-time movement inside the Feedback Loop (see
// docs/design/reverb/stages/06-modulation.md, issues #89 and #90): a
// trajectory per Channel in one of three shapes -- `smoothedRandom`
// (Catmull-Rom interpolation between targets drawn uniformly in
// [-1, +1], a new target every 1/rateHz, reproducible from an integer
// target counter with no accumulated state), `sine`, or `triangle`.
// Decorrelation across Channels is structural: each Channel draws from
// its own seed (`channelSeeds`, consulted only by `smoothedRandom`),
// starts at its own resolved phase (`channelPhases`), and moves at its
// own resolved rate (`channelTargetsPerSample`, already carrying the
// fixed +-10% seeded spread), so trajectories are never pinned to a
// shared start value.
//
// Constructed only for a stage whose Modulation actually moves at least
// one Channel (depthMs > 0 and channelFraction > 0); the owning stage
// keeps the cheaper unmodulated path entirely separate for every
// Channel excluded by `channelModulated` -- including every Channel
// when this class isn't constructed at all -- rather than asking this
// class to collapse to identity at zero depth or zero fraction (see the
// design doc's "Identity is guaranteed by construction, not by
// arithmetic").
class Modulation {
public:
  // `config.channelSeeds`, `config.channelTargetsPerSample`,
  // `config.channelPhases` and `config.channelModulated` must each carry
  // one entry per Channel; construct this only for a stage whose
  // Modulation actually moves at least one Channel (config.depthMs > 0
  // and config.channelFraction > 0), which is exactly when resolution
  // populates all four.
  explicit Modulation(const ResolvedModulation& config);

  // Whether this Channel is one of the ones channelFraction selected.
  // A caller must never call lookbackSamples() for a Channel this
  // returns false for -- that Channel keeps the plain integer read path
  // instead, with no Channel reordering.
  [[nodiscard]] bool isModulated(std::size_t channel) const noexcept;

  // This Channel's current fractional lookback -- `nominalDelaySamples`
  // plus this Channel's trajectory value (clamped to [-1, 1] for
  // `smoothedRandom`, exactly within it for `sine`/`triangle`; see
  // Modulation.cpp) times the resolved Excursion. Uses the frame index
  // last set by advanceFrame(); call this for every modulated Channel
  // before advancing.
  [[nodiscard]] double lookbackSamples(
      std::size_t channel, std::uint64_t nominalDelaySamples) const noexcept;

  // Advances the shared frame counter by one sample. Call exactly once
  // per processed frame, after every modulated Channel's
  // lookbackSamples() for that frame.
  void advanceFrame() noexcept;

  // The backing-vector allocations only. A containing FeedbackLoop owns
  // this object's own in-place storage (held by value inside an
  // std::optional, not by pointer) through its own sizeof(*this), the
  // same convention DelayLine::ownedStorageBytes() documents.
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
