#pragma once

#include <cstdint>

namespace rvrbotron::dsp {

// A shared deterministic seeding primitive (see docs/design/reverb/stages/
// 06-modulation.md, issue #89): resolution-time randomness (Diffuser and
// Feedback Loop delays, shuffles, polarities, mixing matrices) and the
// Feedback Loop's own runtime Modulation trajectories both need the same
// versioned positional draw, so it lives here in `dsp` -- the one layer
// both `config` (which depends on `dsp`) and `dsp` itself can reach.

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

} // namespace rvrbotron::dsp
