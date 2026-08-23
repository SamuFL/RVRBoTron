#include "rvrbotron/config/ResolvedConfigJson.h"

#include "rvrbotron/HarnessError.h"

#include <fstream>
namespace rvrbotron::config {

void writeResolvedConfig(const std::filesystem::path& path,
                         const dsp::ResolvedConfig& config) {
  std::ofstream output(path);
  if (!output) {
    throw HarnessError(
        ErrorCategory::ioFailure, "could not write resolved.json");
  }

  output << "{\n"
         << "  \"formatVersion\": " << config.formatVersion << ",\n"
         << "  \"seed\": " << config.seed << ",\n"
         << "  \"sampleRate\": " << config.sampleRate << ",\n"
         << "  \"composition\": {\"stages\": []}\n"
         << "}\n";
  output.flush();
  if (!output) {
    throw HarnessError(
        ErrorCategory::ioFailure, "could not write resolved.json");
  }
}

} // namespace rvrbotron::config
