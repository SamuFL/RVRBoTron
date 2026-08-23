#pragma once

#include "rvrbotron/dsp/ResolvedConfig.h"

#include <cstdint>
#include <filesystem>

namespace rvrbotron::config {

[[nodiscard]] dsp::ResolvedConfig
resolveDefaultConfig(std::uint32_t sampleRate) noexcept;

[[nodiscard]] dsp::ResolvedConfig
resolveRequestedConfig(const std::filesystem::path& path,
                       std::uint32_t sampleRate);

[[nodiscard]] dsp::ResolvedConfig
readResolvedConfig(const std::filesystem::path& path);

} // namespace rvrbotron::config
