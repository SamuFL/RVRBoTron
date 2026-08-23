#include "rvrbotron/cli/ConfigJson.h"
#include "rvrbotron/config/ResolveConfig.h"
#include "rvrbotron/config/ResolvedConfigJson.h"
#include "rvrbotron/dsp/Reverb.h"
#include "rvrbotron/io/WavStream.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kBlockSize = 512;

struct RenderArguments {
  std::filesystem::path input;
  std::filesystem::path output;
  std::filesystem::path requestedConfig;
  std::filesystem::path resolvedConfig;
};

RenderArguments parseArguments(const int argc, char** argv) {
  if (argc < 2 || std::string(argv[1]) != "render" || argc % 2 != 0) {
    throw std::runtime_error(
        "usage: rvrbotron render --input <wav> [--config <request.json> | "
        "--resolved <resolved.json>] --output <result-dir>");
  }

  RenderArguments arguments;
  for (int index = 2; index < argc; index += 2) {
    const std::string option = argv[index];
    if (option == "--input") {
      arguments.input = argv[index + 1];
    } else if (option == "--output") {
      arguments.output = argv[index + 1];
    } else if (option == "--config") {
      arguments.requestedConfig = argv[index + 1];
    } else if (option == "--resolved") {
      arguments.resolvedConfig = argv[index + 1];
    } else {
      throw std::runtime_error("unknown option: " + option);
    }
  }

  if (arguments.input.empty() || arguments.output.empty()) {
    throw std::runtime_error("--input and --output are required");
  }
  if (!arguments.requestedConfig.empty() &&
      !arguments.resolvedConfig.empty()) {
    throw std::runtime_error("--config and --resolved are mutually exclusive");
  }

  return arguments;
}

std::string readText(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error(
        "could not open configuration: " + path.string());
  }
  return {
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>(),
  };
}

void writeText(const std::filesystem::path& path,
               const std::string_view contents) {
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("could not write " + path.filename().string());
  }
  output << contents;
}

void writeRenderMetadata(const std::filesystem::path& path,
                         const rvrbotron::io::WavInfo& info,
                         const std::uint64_t frameCount) {
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("could not write render.json");
  }

  output << "{\n"
         << "  \"formatVersion\": 1,\n"
         << "  \"rendererVersion\": \"" << RVRBOTRON_VERSION << "\",\n"
         << "  \"samplePrecision\": \""
         << (sizeof(rvrbotron::dsp::Sample) == sizeof(double) ? "float64"
                                                              : "float32")
         << "\",\n"
         << "  \"sampleRate\": " << info.sampleRate << ",\n"
         << "  \"channels\": " << info.channels << ",\n"
         << "  \"frames\": " << frameCount << ",\n"
         << "  \"blockSize\": " << kBlockSize << "\n"
         << "}\n";
}

void render(const RenderArguments& arguments) {
  if (std::filesystem::exists(arguments.output)) {
    throw std::runtime_error("output path already exists");
  }

  rvrbotron::io::WavReader reader(arguments.input);
  const auto info = reader.info();
  if (info.channels != 1) {
    throw std::runtime_error("the first identity renderer requires mono input");
  }

  rvrbotron::dsp::ResolvedConfig config;
  std::optional<std::string> rawRequest;
  if (!arguments.requestedConfig.empty()) {
    rawRequest = readText(arguments.requestedConfig);
    const auto requested =
        rvrbotron::cli::parseRequestedConfig(*rawRequest);
    config = rvrbotron::config::resolveConfig(requested, info.sampleRate);
  } else if (!arguments.resolvedConfig.empty()) {
    config = rvrbotron::cli::parseResolvedConfig(
        readText(arguments.resolvedConfig));
    if (config.sampleRate != info.sampleRate) {
      throw std::runtime_error(
          "configuration error at /sampleRate: expected input sample rate " +
          std::to_string(info.sampleRate) + ", got " +
          std::to_string(config.sampleRate));
    }
  } else {
    config =
        rvrbotron::config::resolveConfig({}, info.sampleRate);
  }

  if (!std::filesystem::create_directories(arguments.output)) {
    throw std::runtime_error("could not create output directory");
  }

  rvrbotron::dsp::Reverb reverb(config);

  std::vector<rvrbotron::dsp::Sample> samples(kBlockSize);
  rvrbotron::dsp::Sample* channels[]{samples.data()};
  std::uint64_t renderedFrames = 0;

  {
    rvrbotron::io::WavWriter writer(
        arguments.output / "output.wav", info.channels, info.sampleRate);

    while (true) {
      const auto framesRead = reader.readFrames(samples.data(), kBlockSize);
      if (framesRead == 0) {
        break;
      }

      reverb.process(channels, info.channels, framesRead);
      writer.writeFrames(samples.data(), framesRead);
      renderedFrames += framesRead;
    }
  }

  rvrbotron::config::writeResolvedConfig(
      arguments.output / "resolved.json", config);
  if (rawRequest.has_value()) {
    writeText(arguments.output / "request.json", *rawRequest);
  }
  writeRenderMetadata(
      arguments.output / "render.json", info, renderedFrames);

  std::cout << arguments.output.string() << '\n';
}

} // namespace

int main(const int argc, char** argv) {
  try {
    render(parseArguments(argc, argv));
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
