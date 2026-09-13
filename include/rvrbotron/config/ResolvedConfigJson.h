#pragma once

#include "rvrbotron/dsp/ResolvedConfig.h"

#include <filesystem>

namespace rvrbotron::config {

void writeResolvedConfig(const std::filesystem::path& path,
                         const dsp::ResolvedConfig& config);

} // namespace rvrbotron::config
