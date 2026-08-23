#pragma once

#include "rvrbotron/io/WavStream.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace rvrbotron::cli {

enum class ConfigurationInput {
  defaults,
  requested,
  resolved,
};

struct RenderMetadata {
  std::string inputFilename;
  std::string inputSha256;
  ConfigurationInput configurationInput;
  io::WavInfo audio;
  std::uint64_t renderedFrames;
  std::size_t blockSize;
};

void writeRenderMetadata(const std::filesystem::path& path,
                         const RenderMetadata& metadata);

} // namespace rvrbotron::cli
