#pragma once

#include <cstdint>

namespace rvrbotron::config {

[[nodiscard]] std::uint64_t splitMix64(std::uint64_t input) noexcept;

[[nodiscard]] std::uint64_t positionalSplitMix64V1(
    std::uint64_t seed,
    std::uint64_t usage,
    std::uint64_t itemIndex,
    std::uint64_t valueIndex,
    std::uint64_t drawIndex = 0) noexcept;

[[nodiscard]] double positionalUnitDoubleV1(
    std::uint64_t seed,
    std::uint64_t usage,
    std::uint64_t itemIndex,
    std::uint64_t valueIndex,
    std::uint64_t drawIndex = 0) noexcept;

} // namespace rvrbotron::config
