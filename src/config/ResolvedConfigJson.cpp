#include "rvrbotron/config/ResolvedConfigJson.h"

#include <fstream>
#include <stdexcept>

namespace rvrbotron::config {

void writeResolvedConfig(const std::filesystem::path& path,
                         const dsp::ResolvedConfig& config) {
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("could not write resolved.json");
  }

  output << "{\n"
         << "  \"formatVersion\": " << config.formatVersion << ",\n"
         << "  \"seed\": " << config.seed << ",\n"
         << "  \"sampleRate\": " << config.sampleRate << ",\n"
         << "  \"composition\": {\"stages\": []}\n"
         << "}\n";
}

} // namespace rvrbotron::config
