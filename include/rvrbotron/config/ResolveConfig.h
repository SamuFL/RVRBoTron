#pragma once

#include "rvrbotron/config/ReverbConfig.h"
#include "rvrbotron/dsp/ResolvedConfig.h"

#include <cstdint>

namespace rvrbotron::config {

[[nodiscard]] dsp::ResolvedConfig
resolveConfig(const ReverbConfig& requested,
              std::uint32_t sampleRate,
              std::uint32_t inputChannels);

void validateResolvedConfig(const dsp::ResolvedConfig& resolved);

} // namespace rvrbotron::config
