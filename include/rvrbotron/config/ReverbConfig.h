#pragma once

#include <cstdint>
#include <optional>

namespace rvrbotron::config {

struct CompositionConfig {
  bool stagesSpecified = false;
};

struct ReverbConfig {
  std::optional<std::uint32_t> formatVersion;
  std::optional<std::uint64_t> seed;
  std::optional<CompositionConfig> composition;
};

} // namespace rvrbotron::config
