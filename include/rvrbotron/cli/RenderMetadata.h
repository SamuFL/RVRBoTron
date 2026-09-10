#pragma once

#include "rvrbotron/io/WavStream.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rvrbotron::cli {

enum class ConfigurationInput {
  defaults,
  requested,
  resolved,
};

struct StageCaptureMetadata {
  std::string path;
  std::string boundary;
  std::uint32_t index;
  std::string sha256;
  std::uint32_t sampleRate;
  std::uint32_t channels;
  std::uint64_t frames;
  // True only for a "main-stereo"/"early-stereo" capture whose own
  // branch was disabled (issue #113): the capture still exists, sized
  // to the full render timeline, but is exact zero throughout rather
  // than a genuinely rendered signal. Always false for "split"/
  // "diffusion-step", which have no enablement of their own.
  bool disabled = false;
};

struct RenderMetadata {
  std::string inputFilename;
  std::string inputSha256;
  ConfigurationInput configurationInput;
  io::WavInfo audio;
  std::uint32_t outputChannels;
  std::uint64_t inputFrames;
  std::uint64_t renderedFrames;
  // The resolved Tail budget authorised for draining past input EOF (see
  // CONTEXT.md); 0 for a Composition with no Diffuser or Feedback Loop.
  // The renderer currently always drains the complete budget, so
  // renderedFrames - inputFrames equals this exactly.
  std::uint64_t tailBudgetFrames;
  std::size_t blockSize;
  std::optional<std::string> stageCaptureProfile;
  std::vector<StageCaptureMetadata> stageCaptures;
};

void writeRenderMetadata(const std::filesystem::path& path,
                         const RenderMetadata& metadata);

} // namespace rvrbotron::cli
