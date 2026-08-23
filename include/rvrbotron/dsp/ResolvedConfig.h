#pragma once

#include <cstdint>

namespace rvrbotron::dsp {

struct ResolvedComposition {};

struct ResolvedConfig {
  std::uint32_t formatVersion = 1;
  std::uint64_t seed = 0;
  std::uint32_t sampleRate = 0;
  ResolvedComposition composition;
};

} // namespace rvrbotron::dsp
