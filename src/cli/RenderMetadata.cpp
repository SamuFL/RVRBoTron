#include "rvrbotron/cli/RenderMetadata.h"

#include "rvrbotron/HarnessError.h"
#include "rvrbotron/dsp/Sample.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <string_view>

namespace rvrbotron::cli {
namespace {

std::string_view configurationInputName(
    const ConfigurationInput input) noexcept {
  switch (input) {
  case ConfigurationInput::defaults:
    return "defaults";
  case ConfigurationInput::requested:
    return "requested";
  case ConfigurationInput::resolved:
    return "resolved";
  }
  return "unknown";
}

std::string_view platformName() noexcept {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__linux__)
  return "linux";
#else
  return "unknown";
#endif
}

std::string_view architectureName() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
  return "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
  return "arm64";
#else
  return "unknown";
#endif
}

} // namespace

void writeRenderMetadata(const std::filesystem::path& path,
                         const RenderMetadata& metadata) {
  const nlohmann::json document{
      {"formatVersion", 1},
      {"rendererVersion", RVRBOTRON_VERSION},
      {"platform", platformName()},
      {"architecture", architectureName()},
      {"samplePrecision",
       sizeof(dsp::Sample) == sizeof(double) ? "float64" : "float32"},
      {"blockSize", metadata.blockSize},
      {"configurationInput",
       configurationInputName(metadata.configurationInput)},
      {"inputFilename", metadata.inputFilename},
      {"inputSha256", metadata.inputSha256},
      {"sampleRate", metadata.audio.sampleRate},
      {"channels", metadata.audio.channels},
      {"frames", metadata.renderedFrames},
  };

  std::ofstream output(path);
  if (!output) {
    throw HarnessError(
        ErrorCategory::ioFailure, "could not write render.json");
  }
  output << document.dump(2) << '\n';
  output.flush();
  if (!output) {
    throw HarnessError(
        ErrorCategory::ioFailure, "could not write render.json");
  }
}

} // namespace rvrbotron::cli
