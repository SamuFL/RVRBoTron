#pragma once

#include "rvrbotron/dsp/Diffuser.h"
#include "rvrbotron/dsp/Downmix.h"
#include "rvrbotron/dsp/ResolvedConfig.h"
#include "rvrbotron/dsp/Sample.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rvrbotron::dsp {

class EarlyReflections {
public:
  explicit EarlyReflections(const ResolvedEarlyReflections& config);

  EarlyReflections(const EarlyReflections&) = delete;
  EarlyReflections& operator=(const EarlyReflections&) = delete;
  EarlyReflections(EarlyReflections&&) = delete;
  EarlyReflections& operator=(EarlyReflections&&) = delete;

  // Zeroes this frame's N-Channel accumulator.
  void beginFrame() noexcept;
  // Zeroes a planar accumulator chunk of `frames` frames, the layout the
  // taps then accumulate into. frames <= kMaxChunkFrames.
  void beginBlock(std::size_t frames) noexcept;

  [[nodiscard]] const DiffuserEarlyTap* taps() const noexcept;
  [[nodiscard]] std::size_t tapCount() const noexcept;

  void processFrame(Sample* left, Sample* right) const noexcept;
  void processBlock(Sample* left, Sample* right, std::size_t frames) const noexcept;

  [[nodiscard]] bool enabled() const noexcept;
  [[nodiscard]] std::size_t ownedBytes() const noexcept;

private:
  bool enabled_;
  Sample gain_;
  Downmix downmix_;
  std::vector<Sample> accumulator_;
  std::vector<DiffuserEarlyTap> taps_;
};

} // namespace rvrbotron::dsp
