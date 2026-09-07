#include "rvrbotron/dsp/PositionalRandom.h"

#include <array>
#include <cstdint>
#include <iostream>

int main() {
  using rvrbotron::dsp::positionalSplitMix64V1;
  using rvrbotron::dsp::positionalUnitDoubleV1;
  using rvrbotron::dsp::splitMix64;

  const std::array<std::pair<std::uint64_t, std::uint64_t>, 4>
      splitMixVectors{{
          {0x0000000000000000ULL, 0xe220a8397b1dcdafULL},
          {0x0000000000000001ULL, 0x910a2dec89025cc1ULL},
          {0xffffffffffffffffULL, 0xe4d971771b652c20ULL},
          {0x123456789abcdef0ULL, 0x161922c645ce50e8ULL},
      }};
  for (const auto [input, expected] : splitMixVectors) {
    if (splitMix64(input) != expected) {
      std::cerr << "SplitMix64 fixed vector mismatch\n";
      return 1;
    }
  }

  struct PositionalVector {
    std::uint64_t seed;
    std::uint64_t usage;
    std::uint64_t item;
    std::uint64_t value;
    std::uint64_t draw;
    std::uint64_t expected;
  };
  const std::array<PositionalVector, 4> positionalVectors{{
      {0x0000000000000000ULL,
       0x445354455044454cULL,
       0,
       0,
       0,
       0x7208d8b3feec7426ULL},
      {0x0000000000000001ULL,
       0x445354455044454cULL,
       0,
       0,
       0,
       0x428315a75e0362d4ULL},
      {0x0123456789abcdefULL,
       0x4453544550534846ULL,
       3,
       7,
       0,
       0xede7f3016220eb8dULL},
      {0xffffffffffffffffULL,
       0x4453544550504f4cULL,
       9,
       2,
       4,
       0x7382a19b66b5ce30ULL},
  }};
  for (const auto& vector : positionalVectors) {
    const auto actual = positionalSplitMix64V1(
        vector.seed,
        vector.usage,
        vector.item,
        vector.value,
        vector.draw);
    if (actual != vector.expected) {
      std::cerr << "positional SplitMix64 fixed vector mismatch\n";
      return 1;
    }
  }
  if (positionalUnitDoubleV1(
          0,
          0x445354455044454cULL,
          0,
          0,
          0) != 0.44544748682430835 ||
      positionalUnitDoubleV1(
          0x0123456789abcdefULL,
          0x4453544550534846ULL,
          3,
          7,
          0) != 0.929320514524196) {
    std::cerr << "positional high-53-bit double vector mismatch\n";
    return 1;
  }

  return 0;
}
