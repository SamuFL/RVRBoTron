#include "rvrbotron/cli/ConfigJson.h"
#include "rvrbotron/config/ResolveConfig.h"
#include "rvrbotron/config/ResolvedConfigJson.h"
#include "rvrbotron/dsp/Reverb.h"
#include "rvrbotron/io/WavStream.h"

#include <charconv>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
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
  std::optional<std::filesystem::path> requestedConfig;
  std::optional<std::filesystem::path> resolvedConfig;
  std::size_t blockSize{kBlockSize};
};

std::size_t parseBlockSize(const std::string_view value) {
  std::size_t blockSize = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), blockSize);
  if (result.ec != std::errc{} ||
      result.ptr != value.data() + value.size() || blockSize == 0) {
    throw std::runtime_error("--block-size requires a positive integer");
  }
  return blockSize;
}

RenderArguments parseArguments(const int argc, char** argv) {
  if (argc < 2 || std::string(argv[1]) != "render" || argc % 2 != 0) {
    throw std::runtime_error(
        "usage: rvrbotron render --input <wav> [--config <request.json> | "
        "--resolved <resolved.json>] [--block-size <frames>] "
        "--output <result-dir>");
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
    } else if (option == "--block-size") {
      arguments.blockSize = parseBlockSize(argv[index + 1]);
    } else {
      throw std::runtime_error("unknown option: " + option);
    }
  }

  if (arguments.input.empty() || arguments.output.empty()) {
    throw std::runtime_error("--input and --output are required");
  }
  if (arguments.requestedConfig.has_value() &&
      arguments.resolvedConfig.has_value()) {
    throw std::runtime_error("--config and --resolved are mutually exclusive");
  }
  if (arguments.requestedConfig.has_value() &&
      arguments.requestedConfig->empty()) {
    throw std::runtime_error("--config requires a non-empty path");
  }
  if (arguments.resolvedConfig.has_value() &&
      arguments.resolvedConfig->empty()) {
    throw std::runtime_error("--resolved requires a non-empty path");
  }

  return arguments;
}

std::string readFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error(
        "could not open configuration: " + path.string());
  }
  return {
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>(),
  };
}

void writeFile(const std::filesystem::path& path,
               const std::string_view contents) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("could not write " + path.filename().string());
  }
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.flush();
  if (!output) {
    throw std::runtime_error("could not write " + path.filename().string());
  }
}

void writeRenderMetadata(const std::filesystem::path& path,
                         const rvrbotron::io::WavInfo& info,
                         const std::uint64_t frameCount,
                         const std::size_t blockSize) {
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
         << "  \"blockSize\": " << blockSize << "\n"
         << "}\n";
}

void render(const RenderArguments& arguments) {
  if (std::filesystem::exists(arguments.output)) {
    throw std::runtime_error("output path already exists");
  }

  rvrbotron::io::WavReader reader(arguments.input);
  const auto info = reader.info();
  if (info.channels == 0 || info.channels > 2) {
    throw std::runtime_error("the identity renderer requires mono or stereo input");
  }
  const auto channelCount = static_cast<std::size_t>(info.channels);
  if (arguments.blockSize >
      std::numeric_limits<std::size_t>::max() / channelCount) {
    throw std::runtime_error("--block-size is too large");
  }

  rvrbotron::dsp::ResolvedConfig config;
  std::optional<std::string> rawRequest;
  if (arguments.requestedConfig.has_value()) {
    rawRequest = readFile(*arguments.requestedConfig);
    const auto requested =
        rvrbotron::cli::parseRequestedConfig(*rawRequest);
    config = rvrbotron::config::resolveConfig(requested, info.sampleRate);
  } else if (arguments.resolvedConfig.has_value()) {
    config = rvrbotron::cli::parseResolvedConfig(
        readFile(*arguments.resolvedConfig));
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

  std::vector<rvrbotron::dsp::Sample> interleavedSamples(
      arguments.blockSize * channelCount);
  std::vector<rvrbotron::dsp::Sample> channelSamples(
      arguments.blockSize * channelCount);
  std::vector<rvrbotron::dsp::Sample*> channels(channelCount);
  for (std::size_t channel = 0; channel < channelCount; ++channel) {
    channels[channel] =
        channelSamples.data() + channel * arguments.blockSize;
  }
  std::uint64_t renderedFrames = 0;

  {
    rvrbotron::io::WavWriter writer(
        arguments.output / "output.wav", info.channels, info.sampleRate);

    while (true) {
      const auto framesRead =
          reader.readFrames(interleavedSamples.data(), arguments.blockSize);
      if (framesRead == 0) {
        break;
      }

      for (std::size_t frame = 0; frame < framesRead; ++frame) {
        for (std::size_t channel = 0; channel < channelCount; ++channel) {
          channels[channel][frame] =
              interleavedSamples[frame * channelCount + channel];
        }
      }

      reverb.process(channels.data(), channelCount, framesRead);

      for (std::size_t frame = 0; frame < framesRead; ++frame) {
        for (std::size_t channel = 0; channel < channelCount; ++channel) {
          interleavedSamples[frame * channelCount + channel] =
              channels[channel][frame];
        }
      }
      writer.writeFrames(interleavedSamples.data(), framesRead);
      renderedFrames += framesRead;
    }
  }

  rvrbotron::config::writeResolvedConfig(
      arguments.output / "resolved.json", config);
  if (rawRequest.has_value()) {
    writeFile(arguments.output / "request.json", *rawRequest);
  }
  writeRenderMetadata(
      arguments.output / "render.json",
      info,
      renderedFrames,
      arguments.blockSize);

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
