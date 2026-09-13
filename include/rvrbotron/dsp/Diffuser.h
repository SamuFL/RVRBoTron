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

struct DiffuserEarlyTap {
  std::uint32_t stepIndex = 0;
  Sample gain = Sample{1};
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
