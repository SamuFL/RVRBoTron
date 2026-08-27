#include "rvrbotron/cli/RenderMetadata.h"

#include "rvrbotron/HarnessError.h"
#include "rvrbotron/cli/Platform.h"
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

} // namespace

void writeRenderMetadata(const std::filesystem::path& path,
                         const RenderMetadata& metadata) {
  nlohmann::json document{
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
      {"channels", metadata.outputChannels},
      {"frames", metadata.renderedFrames},
      {"inputFrames", metadata.inputFrames},
      {"tailBudgetFrames", metadata.tailBudgetFrames},
  };
  if (metadata.stageCaptureProfile.has_value()) {
    document["stageCaptureProfile"] = *metadata.stageCaptureProfile;
    document["stageCaptures"] = nlohmann::json::array();
    for (const auto& capture : metadata.stageCaptures) {
      document["stageCaptures"].push_back(
          {
              {"path", capture.path},
              {"boundary", capture.boundary},
              {"index", capture.index},
              {"sha256", capture.sha256},
              {"sampleRate", capture.sampleRate},
              {"channels", capture.channels},
              {"frames", capture.frames},
          });
    }
  }

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
