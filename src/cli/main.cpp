#include "rvrbotron/HarnessError.h"
#include "rvrbotron/cli/ConfigJson.h"
#include "rvrbotron/cli/FileSha256.h"
#include "rvrbotron/cli/RenderMetadata.h"
#include "rvrbotron/cli/RenderResultTransaction.h"
#include "rvrbotron/config/ResolveConfig.h"
#include "rvrbotron/config/ResolvedConfigJson.h"
#include "rvrbotron/dsp/Reverb.h"
#include "rvrbotron/io/WavStream.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <csignal>
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
volatile std::sig_atomic_t interruptedSignal = 0;

void handleInterruption(const int signal) noexcept {
  interruptedSignal = signal;
}

void installInterruptionHandlers() {
  if (std::signal(SIGINT, handleInterruption) == SIG_ERR ||
      std::signal(SIGTERM, handleInterruption) == SIG_ERR) {
    throw rvrbotron::HarnessError(
        rvrbotron::ErrorCategory::internalProcessingFailure,
        "could not install interruption handlers");
  }
}

void throwIfInterrupted() {
  if (interruptedSignal != 0) {
    throw rvrbotron::HarnessError(
        rvrbotron::ErrorCategory::internalProcessingFailure,
        "render interrupted");
  }
}

struct RenderArguments {
  std::filesystem::path input;
  std::filesystem::path output;
  std::optional<std::filesystem::path> requestedConfig;
  std::optional<std::filesystem::path> resolvedConfig;
  std::size_t blockSize{kBlockSize};
};

enum class ErrorFormat {
  text,
  json,
};

ErrorFormat requestedErrorFormat(const int argc, char** argv) noexcept {
  for (int index = 2; index + 1 < argc; index += 2) {
    if (std::string_view(argv[index]) == "--error-format" &&
        std::string_view(argv[index + 1]) == "json") {
      return ErrorFormat::json;
    }
  }
  return ErrorFormat::text;
}

int reportError(const rvrbotron::HarnessError& error,
                const ErrorFormat format) {
  if (format == ErrorFormat::json) {
    nlohmann::json diagnostic{
        {"category", rvrbotron::categoryName(error.category())},
        {"exitCode", rvrbotron::exitCode(error.category())},
        {"reason", error.reason()},
    };
    if (error.location().has_value()) {
      diagnostic["location"] = *error.location();
    }
    std::cerr << diagnostic.dump() << '\n';
  } else {
    std::cerr << rvrbotron::categoryName(error.category());
    if (error.location().has_value()) {
      std::cerr << " at " << *error.location();
    }
    std::cerr << ": " << error.reason() << '\n';
  }
  return rvrbotron::exitCode(error.category());
}

std::size_t parseBlockSize(const std::string_view value) {
  std::size_t blockSize = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), blockSize);
  if (result.ec != std::errc{} ||
      result.ptr != value.data() + value.size() || blockSize == 0) {
    throw rvrbotron::HarnessError(
        rvrbotron::ErrorCategory::invalidArguments,
        "--block-size requires a positive integer");
  }
  return blockSize;
}

RenderArguments parseArguments(const int argc, char** argv) {
  if (argc < 2 || std::string(argv[1]) != "render" || argc % 2 != 0) {
    throw rvrbotron::HarnessError(
        rvrbotron::ErrorCategory::invalidArguments,
        "usage: rvrbotron render --input <wav> [--config <request.json> | "
        "--resolved <resolved.json>] [--block-size <frames>] "
        "[--error-format <text|json>] --output <result-dir>");
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
    } else if (option == "--error-format") {
      const std::string_view format = argv[index + 1];
      if (format != "text" && format != "json") {
        throw rvrbotron::HarnessError(
            rvrbotron::ErrorCategory::invalidArguments,
            "--error-format requires text or json");
      }
    } else {
      throw rvrbotron::HarnessError(
          rvrbotron::ErrorCategory::invalidArguments,
          "unknown option: " + option);
    }
  }

  if (arguments.input.empty() || arguments.output.empty()) {
    throw rvrbotron::HarnessError(
        rvrbotron::ErrorCategory::invalidArguments,
        "--input and --output are required");
  }
  if (arguments.requestedConfig.has_value() &&
      arguments.resolvedConfig.has_value()) {
    throw rvrbotron::HarnessError(
        rvrbotron::ErrorCategory::invalidArguments,
        "--config and --resolved are mutually exclusive");
  }
  if (arguments.requestedConfig.has_value() &&
      arguments.requestedConfig->empty()) {
    throw rvrbotron::HarnessError(
        rvrbotron::ErrorCategory::invalidArguments,
        "--config requires a non-empty path");
  }
  if (arguments.resolvedConfig.has_value() &&
      arguments.resolvedConfig->empty()) {
    throw rvrbotron::HarnessError(
        rvrbotron::ErrorCategory::invalidArguments,
        "--resolved requires a non-empty path");
  }

  return arguments;
}

std::string readFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw rvrbotron::HarnessError(
        rvrbotron::ErrorCategory::ioFailure,
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
    throw rvrbotron::HarnessError(
        rvrbotron::ErrorCategory::ioFailure,
        "could not write " + path.filename().string());
  }
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.flush();
  if (!output) {
    throw rvrbotron::HarnessError(
        rvrbotron::ErrorCategory::ioFailure,
        "could not write " + path.filename().string());
  }
}

void render(const RenderArguments& arguments) {
  rvrbotron::cli::RenderResultTransaction result(arguments.output);
  const auto& resultPath = result.workingPath();
  throwIfInterrupted();

  rvrbotron::io::WavReader reader(arguments.input);
  const auto info = reader.info();
  if (info.channels == 0 || info.channels > 2) {
    throw rvrbotron::HarnessError(
        rvrbotron::ErrorCategory::unsupportedAudio,
        "the identity renderer requires mono or stereo input");
  }
  const auto channelCount = static_cast<std::size_t>(info.channels);
  if (arguments.blockSize >
      std::numeric_limits<std::size_t>::max() / channelCount) {
    throw rvrbotron::HarnessError(
        rvrbotron::ErrorCategory::invalidArguments,
        "--block-size is too large");
  }

  rvrbotron::dsp::ResolvedConfig config;
  std::optional<std::string> rawRequest;
  auto configurationInput =
      rvrbotron::cli::ConfigurationInput::defaults;
  if (arguments.requestedConfig.has_value()) {
    configurationInput =
        rvrbotron::cli::ConfigurationInput::requested;
    rawRequest = readFile(*arguments.requestedConfig);
    const auto requested =
        rvrbotron::cli::parseRequestedConfig(*rawRequest);
    config = rvrbotron::config::resolveConfig(requested, info.sampleRate);
  } else if (arguments.resolvedConfig.has_value()) {
    configurationInput =
        rvrbotron::cli::ConfigurationInput::resolved;
    config = rvrbotron::cli::parseResolvedConfig(
        readFile(*arguments.resolvedConfig));
    if (config.sampleRate != info.sampleRate) {
      throw rvrbotron::HarnessError(
          rvrbotron::ErrorCategory::invalidConfiguration,
          "expected input sample rate " +
              std::to_string(info.sampleRate) + ", got " +
              std::to_string(config.sampleRate),
          "/sampleRate");
    }
  } else {
    config =
        rvrbotron::config::resolveConfig({}, info.sampleRate);
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
        resultPath / "output.wav", info.channels, info.sampleRate);

    while (true) {
      throwIfInterrupted();
      const auto framesRead =
          reader.readFrames(interleavedSamples.data(), arguments.blockSize);
      throwIfInterrupted();
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
    writer.close();
  }

  rvrbotron::config::writeResolvedConfig(
      resultPath / "resolved.json", config);
  if (rawRequest.has_value()) {
    writeFile(resultPath / "request.json", *rawRequest);
  }
  throwIfInterrupted();
  rvrbotron::cli::writeRenderMetadata(
      resultPath / "render.json",
      {
          arguments.input.filename().string(),
          rvrbotron::cli::fileSha256(arguments.input),
          configurationInput,
          info,
          renderedFrames,
          arguments.blockSize,
      });

  throwIfInterrupted();
  result.publish();
  std::cout << arguments.output.string() << '\n';
}

} // namespace

int main(const int argc, char** argv) {
  const auto errorFormat = requestedErrorFormat(argc, argv);
  try {
    installInterruptionHandlers();
    render(parseArguments(argc, argv));
    return 0;
  } catch (const rvrbotron::HarnessError& error) {
    return reportError(error, errorFormat);
  } catch (const std::exception& error) {
    return reportError(
        {
            rvrbotron::ErrorCategory::internalProcessingFailure,
            error.what(),
        },
        errorFormat);
  } catch (...) {
    return reportError(
        {
            rvrbotron::ErrorCategory::internalProcessingFailure,
            "unknown internal error",
        },
        errorFormat);
  }
}
