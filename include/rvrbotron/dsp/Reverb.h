#pragma once

#include "rvrbotron/dsp/ResolvedConfig.h"
#include "rvrbotron/dsp/Sample.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace rvrbotron::dsp {

enum class StageCaptureBoundary {
  split,
  diffusionStep,
};

class StageCaptureSink {
public:
  virtual ~StageCaptureSink() = default;

  virtual void captureFrame(StageCaptureBoundary boundary,
                            std::uint32_t index,
                            const Sample* channels,
                            std::size_t channelCount) noexcept = 0;
};

class Reverb {
public:
  explicit Reverb(const ResolvedConfig& config,
                  StageCaptureSink* captureSink = nullptr);
  ~Reverb();

  Reverb(const Reverb&) = delete;
  Reverb& operator=(const Reverb&) = delete;
  Reverb(Reverb&&) noexcept;
  Reverb& operator=(Reverb&&) noexcept;

  void process(const Sample* const* inputs,
               std::size_t inputChannelCount,
               Sample* const* outputs,
               std::size_t outputChannelCount,
               std::size_t frameCount) noexcept;

  [[nodiscard]] std::size_t inputChannelCount() const noexcept;
  [[nodiscard]] std::size_t outputChannelCount() const noexcept;
  [[nodiscard]] std::uint64_t finiteTailFrames() const noexcept;
  // Exact DSP-owned bytes: the heap-allocated pimpl's object storage, its
  // owned-container capacities, and every owned Split/Diffuser/Downmix
  // sub-object reachable from it.
  [[nodiscard]] std::size_t ownedBytes() const noexcept;

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

} // namespace rvrbotron::dsp
