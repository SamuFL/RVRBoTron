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

// The Early Reflections tap seam (issues #111/#112, docs/design/reverb/
// stages/07-early-reflections.md's "What this forces on the
// architecture"): one entry in a caller-owned, caller-sorted array, not a
// registered observer. `accumulator` must point to at least as many
// Samples as the Diffuser has Channels -- every entry in an array passed
// to the same processFrame call shares one accumulator, since the Early
// envelope's taps are shaped and summed into a single N-Channel frame
// before Downmix (see EarlyReflections). `gain` scales this tap's own
// completed post-step frame (its resolved shaping gain, issue #112)
// before it is added (+=) into the accumulator once its `stepIndex`
// runs -- no callback registration, no retained per-tap audio, no
// allocation, and no perturbation of the Diffuser's own Main output.
struct DiffuserEarlyTap {
  std::uint32_t stepIndex = 0;
  Sample gain = Sample{1};
  Sample* accumulator = nullptr;
};

class Diffuser {
public:
  explicit Diffuser(const ResolvedDiffuser& config);
  ~Diffuser();

  // `earlyTaps` must be sorted ascending by stepIndex with unique indices
  // (the canonical order Resolved Configuration already stores them in;
  // see ResolvedEarlyReflections::taps) -- processFrame relies on that
  // order to match taps against steps in a single forward pass, without
  // rescanning the whole array for every step.
  void processFrame(
      const Sample* inputs,
      Sample* outputs,
      DiffuserCaptureSink* captureSink = nullptr,
      const DiffuserEarlyTap* earlyTaps = nullptr,
      std::size_t earlyTapCount = 0) noexcept;

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
