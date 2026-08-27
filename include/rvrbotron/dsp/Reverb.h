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
  // Resolved upper bound on frames to render past input EOF: a Diffuser's
  // genuinely finite response length, a Feedback Loop's Tail budget, or
  // their sum when both stages are present. See CONTEXT.md's Tail budget
  // entry.
  [[nodiscard]] std::uint64_t tailBudgetFrames() const noexcept;
  // Exact DSP-owned bytes: the heap-allocated pimpl's object storage, its
  // owned-container capacities, and every owned Split/Diffuser/Downmix
  // sub-object reachable from it.
  [[nodiscard]] std::size_t ownedBytes() const noexcept;

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

} // namespace rvrbotron::dsp
