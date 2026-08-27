#pragma once

#include "rvrbotron/config/ReverbConfig.h"
#include "rvrbotron/dsp/ResolvedConfig.h"

#include <string_view>

namespace rvrbotron::cli {

[[nodiscard]] config::ReverbConfig
parseRequestedConfig(std::string_view contents);

[[nodiscard]] dsp::ResolvedConfig
parseResolvedConfig(std::string_view contents,
                    std::uint64_t memoryBudgetBytes =
                        config::kDefaultDiffuserMemoryBudgetBytes);

} // namespace rvrbotron::cli
