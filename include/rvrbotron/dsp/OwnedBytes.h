#pragma once

#include <cstddef>
#include <vector>

namespace rvrbotron::dsp {

// Actual allocated bytes backing a vector's owned storage: capacity, not
// size, since capacity is what is genuinely allocated on the heap.
template <typename T>
[[nodiscard]] constexpr std::size_t ownedVectorBytes(
    const std::vector<T>& values) noexcept {
  return values.capacity() * sizeof(T);
}

// std::vector<bool> is bit-packed, not one byte per element -- the
// generic template above would overstate its footprint roughly eightfold
// (`capacity() * sizeof(bool)`), so this overload reports the actual
// whole bytes its packed storage occupies. Ordinary overload resolution
// prefers this exact match over instantiating the template for `bool`.
[[nodiscard]] constexpr std::size_t ownedVectorBytes(
    const std::vector<bool>& values) noexcept {
  return (values.capacity() + 7) / 8;
}

} // namespace rvrbotron::dsp
