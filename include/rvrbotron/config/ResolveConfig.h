#pragma once

#include "rvrbotron/config/ReverbConfig.h"
#include "rvrbotron/dsp/ResolvedConfig.h"

#include <cstdint>

namespace rvrbotron::config {

[[nodiscard]] dsp::ResolvedConfig
resolveConfig(const ReverbConfig& requested,
              std::uint32_t sampleRate,
              std::uint32_t inputChannels,
              std::uint64_t memoryBudgetBytes =
                  kDefaultDiffuserMemoryBudgetBytes);

void validateResolvedConfig(const dsp::ResolvedConfig& resolved,
                            std::uint64_t memoryBudgetBytes =
                                kDefaultDiffuserMemoryBudgetBytes);

} // namespace rvrbotron::config
