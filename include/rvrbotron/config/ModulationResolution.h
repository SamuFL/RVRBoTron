#pragma once

#include <cstdint>
#include <vector>

namespace rvrbotron::config {

constexpr std::uint64_t kModulationInterpolationMarginSamples = 3;

enum class ModulationOwner {
  feedbackLoop,
  diffusionStep,
};

[[nodiscard]] double resolveExcursionSamples(
    double depthMs, double sampleRateHz) noexcept;

[[nodiscard]] double resolveModulationRateSpread(
    std::uint64_t seed,
    ModulationOwner owner,
    std::uint64_t itemIndex,
    std::uint32_t channel) noexcept;

[[nodiscard]] double resolveModulationPhase(
    std::uint64_t seed,
    ModulationOwner owner,
    std::uint64_t itemIndex,
    std::uint32_t channel) noexcept;

[[nodiscard]] bool modulationFitsDelay(
    std::uint64_t delaySamples, double excursionSamples) noexcept;

[[nodiscard]] std::uint64_t resolveModulationHeadroomSamples(
    double excursionSamples) noexcept;

[[nodiscard]] std::uint64_t resolveModulationBlockSizeBoundSamples(
    const std::vector<std::uint64_t>& delaysSamples,
    const std::vector<bool>& channelModulated,
    double excursionSamples) noexcept;

} // namespace rvrbotron::config
