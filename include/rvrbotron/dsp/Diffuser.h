#pragma once

#include "rvrbotron/dsp/ResolvedConfig.h"
#include "rvrbotron/dsp/Sample.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace rvrbotron::dsp {

class DiffusionStep;

class DiffuserCaptureSink {
public:
  virtual ~DiffuserCaptureSink() = default;

  virtual void captureDiffusionStepFrame(
      std::uint32_t index,
      const Sample* channels,
      std::size_t channelCount) noexcept = 0;
};

// The Early Reflections tap seam (issue #111, docs/design/reverb/stages/
// 07-early-reflections.md's "What this forces on the architecture"): a
// caller-owned N-Channel accumulator, not a registered observer.
// `accumulator` must point to at least as many Samples as the Diffuser has
// Channels; `processFrame` adds (+=) the configured `stepIndex`'s completed
// post-step frame into it once that step runs, and otherwise leaves it
// untouched -- no callback registration, no retained per-tap audio, no
// allocation, and no perturbation of the Diffuser's own Main output.
struct DiffuserEarlyTap {
  std::uint32_t stepIndex = 0;
  Sample* accumulator = nullptr;
};

class Diffuser {
public:
  explicit Diffuser(const ResolvedDiffuser& config);
  ~Diffuser();

  void processFrame(
      const Sample* inputs,
      Sample* outputs,
      DiffuserCaptureSink* captureSink = nullptr,
      const DiffuserEarlyTap* earlyTap = nullptr) noexcept;

  [[nodiscard]] std::size_t channelCount() const noexcept;
  [[nodiscard]] std::size_t stepCount() const noexcept;
  [[nodiscard]] std::uint64_t totalSamples() const noexcept;
  [[nodiscard]] std::size_t ownedBytes() const noexcept;

private:
  std::size_t channels_;
  std::uint64_t totalSamples_;
  std::vector<std::uint32_t> stepIndices_;
  std::vector<std::unique_ptr<DiffusionStep>> steps_;
};

} // namespace rvrbotron::dsp
