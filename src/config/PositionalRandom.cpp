#include "rvrbotron/config/PositionalRandom.h"

#include <cmath>

namespace rvrbotron::config {

std::uint64_t splitMix64(const std::uint64_t input) noexcept {
  auto value = input + 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

std::uint64_t positionalSplitMix64V1(
    const std::uint64_t seed,
    const std::uint64_t usage,
    const std::uint64_t itemIndex,
    const std::uint64_t valueIndex,
    const std::uint64_t drawIndex) noexcept {
  auto value = splitMix64(seed ^ usage);
  value = splitMix64(value ^ itemIndex);
  value = splitMix64(value ^ valueIndex);
  return splitMix64(value ^ drawIndex);
}

double positionalUnitDoubleV1(
    const std::uint64_t seed,
    const std::uint64_t usage,
    const std::uint64_t itemIndex,
    const std::uint64_t valueIndex,
    const std::uint64_t drawIndex) noexcept {
  const auto bits = positionalSplitMix64V1(
      seed, usage, itemIndex, valueIndex, drawIndex);
  return std::ldexp(static_cast<double>(bits >> 11U), -53);
}

} // namespace rvrbotron::config
