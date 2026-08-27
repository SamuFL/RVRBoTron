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

} // namespace rvrbotron::dsp
