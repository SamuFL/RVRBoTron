#include "rvrbotron/HarnessError.h"
#include "rvrbotron/cli/Benchmark.h"
#include "rvrbotron/cli/ConfigJson.h"
#include "rvrbotron/cli/FileSha256.h"
#include "rvrbotron/cli/RenderMetadata.h"
#include "rvrbotron/cli/RenderResultTransaction.h"
#include "rvrbotron/config/ResolveConfig.h"
#include "rvrbotron/config/ResolvedConfigJson.h"
#include "rvrbotron/dsp/Reverb.h"
#include "rvrbotron/io/WavStream.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
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
  bool captureAllStages{false};
  std::uint64_t memoryBudgetBytes{
      rvrbotron::config::kDefaultDiffuserMemoryBudgetBytes};
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

std::uint64_t parseMemoryBudgetMib(const std::string_view value) {
  std::uint64_t mib = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), mib);
  if (result.ec != std::errc{} ||
      result.ptr != value.data() + value.size() || mib == 0 ||
      mib > std::numeric_limits<std::uint64_t>::max() / (1024ULL * 1024ULL)) {
    throw rvrbotron::HarnessError(
        rvrbotron::ErrorCategory::invalidArguments,
        "--memory-budget-mib requires a positive integer");
  }
  return mib * 1024ULL * 1024ULL;
}

RenderArguments parseArguments(const int argc, char** argv) {
  if (argc % 2 != 0) {
    throw rvrbotron::HarnessError(
        rvrbotron::ErrorCategory::invalidArguments,
        "usage: rvrbotron render --input <wav> [--config <request.json> | "
        "--resolved <resolved.json>] [--block-size <frames>] "
        "[--capture-stages all] [--memory-budget-mib <mebibytes>] "
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
    } else if (option == "--memory-budget-mib") {
      arguments.memoryBudgetBytes = parseMemoryBudgetMib(argv[index + 1]);
    } else if (option == "--capture-stages") {
      if (std::string_view(argv[index + 1]) != "all") {
        throw rvrbotron::HarnessError(
            rvrbotron::ErrorCategory::invalidArguments,
            "--capture-stages requires all");
      }
      arguments.captureAllStages = true;
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

class WavStageCaptureSink final
    : public rvrbotron::dsp::StageCaptureSink {
public:
  WavStageCaptureSink(const std::filesystem::path& resultPath,
                      const std::uint32_t channels,
                      const std::uint32_t sampleRate,
                      const std::uint32_t diffusionStepCount)
      : resultPath_(resultPath),
        channels_(channels),
        sampleRate_(sampleRate) {
    const auto captureDirectory = resultPath_ / "captures";
    std::error_code error;
    std::filesystem::create_directories(captureDirectory, error);
    if (error) {
      throw rvrbotron::HarnessError(
          rvrbotron::ErrorCategory::ioFailure,
          "could not create Stage capture directory");
    }
    split_ = std::make_unique<rvrbotron::io::WavWriter>(
        captureDirectory / "00-split.wav", channels_, sampleRate_);
    diffusionSteps_.reserve(diffusionStepCount);
    diffusionStepFrames_.assign(diffusionStepCount, 0);
    for (std::uint32_t step = 0; step < diffusionStepCount; ++step) {
      diffusionSteps_.push_back(
          std::make_unique<rvrbotron::io::WavWriter>(
              captureDirectory / diffusionStepFileName(step),
              channels_,
              sampleRate_));
    }
  }

  void captureFrame(
      const rvrbotron::dsp::StageCaptureBoundary boundary,
      const std::uint32_t index,
      const rvrbotron::dsp::Sample* channels,
      const std::size_t channelCount) noexcept override {
    if (failed_ || channelCount != channels_) {
      failed_ = true;
      return;
    }
    try {
      if (boundary == rvrbotron::dsp::StageCaptureBoundary::split) {
        if (index != 0) {
          failed_ = true;
          return;
        }
        split_->writeFrames(channels, 1);
        ++splitFrames_;
      } else {
        if (index >= diffusionSteps_.size()) {
          failed_ = true;
          return;
        }
        diffusionSteps_[index]->writeFrames(channels, 1);
        ++diffusionStepFrames_[index];
      }
    } catch (...) {
      failed_ = true;
    }
  }

  std::vector<rvrbotron::cli::StageCaptureMetadata> finish() {
    if (failed_) {
      throw rvrbotron::HarnessError(
          rvrbotron::ErrorCategory::ioFailure,
          "could not write Stage capture");
    }
    split_->close();
    const auto splitPath =
        std::filesystem::path("captures") / "00-split.wav";
    std::vector<rvrbotron::cli::StageCaptureMetadata> metadata{
        {
            splitPath.generic_string(),
            "split",
            0,
            rvrbotron::cli::fileSha256(resultPath_ / splitPath),
            sampleRate_,
            channels_,
            splitFrames_,
        },
    };
    metadata.reserve(1 + diffusionSteps_.size());
    for (std::size_t step = 0; step < diffusionSteps_.size(); ++step) {
      diffusionSteps_[step]->close();
      const auto diffusionPath = std::filesystem::path("captures") /
          diffusionStepFileName(static_cast<std::uint32_t>(step));
      metadata.push_back(
          {
              diffusionPath.generic_string(),
              "diffusion-step",
              static_cast<std::uint32_t>(step),
              rvrbotron::cli::fileSha256(resultPath_ / diffusionPath),
              sampleRate_,
              channels_,
              diffusionStepFrames_[step],
          });
    }
    return metadata;
  }

private:
  static std::string diffusionStepFileName(const std::uint32_t step) {
    return "01-diffusion-step-" + std::to_string(step) + ".wav";
  }

  std::filesystem::path resultPath_;
  std::uint32_t channels_;
  std::uint32_t sampleRate_;
  std::unique_ptr<rvrbotron::io::WavWriter> split_;
  std::vector<std::unique_ptr<rvrbotron::io::WavWriter>> diffusionSteps_;
  std::uint64_t splitFrames_{0};
  std::vector<std::uint64_t> diffusionStepFrames_;
  bool failed_{false};
};

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
  const auto inputChannelCount = static_cast<std::size_t>(info.channels);
  if (arguments.blockSize >
      std::numeric_limits<std::size_t>::max() / inputChannelCount) {
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
    config = rvrbotron::config::resolveConfig(
        requested,
        info.sampleRate,
        info.channels,
        arguments.memoryBudgetBytes);
  } else if (arguments.resolvedConfig.has_value()) {
    configurationInput =
        rvrbotron::cli::ConfigurationInput::resolved;
    config = rvrbotron::cli::parseResolvedConfig(
        readFile(*arguments.resolvedConfig), arguments.memoryBudgetBytes);
    if (config.sampleRate != info.sampleRate) {
      throw rvrbotron::HarnessError(
          rvrbotron::ErrorCategory::invalidConfiguration,
          "expected input sample rate " +
              std::to_string(info.sampleRate) + ", got " +
              std::to_string(config.sampleRate),
          "/sampleRate");
    }
    if (!config.composition.stages.empty()) {
      const auto& split =
          std::get<rvrbotron::dsp::ResolvedSplit>(
              config.composition.stages.front());
      if (split.inputChannels != info.channels) {
        throw rvrbotron::HarnessError(
            rvrbotron::ErrorCategory::invalidConfiguration,
            "expected input Channel count " +
                std::to_string(split.inputChannels) + ", got " +
                std::to_string(info.channels),
            "/composition/stages/0/inputChannels");
      }
    }
  } else {
    config = rvrbotron::config::resolveConfig(
        {}, info.sampleRate, info.channels, arguments.memoryBudgetBytes);
  }

  const auto outputChannelCount = config.composition.stages.empty()
                                      ? inputChannelCount
                                      : std::size_t{2};
  if (arguments.blockSize >
      std::numeric_limits<std::size_t>::max() / outputChannelCount) {
    throw rvrbotron::HarnessError(
        rvrbotron::ErrorCategory::invalidArguments,
        "--block-size is too large");
  }
  std::unique_ptr<WavStageCaptureSink> captureSink;
  if (arguments.captureAllStages &&
      !config.composition.stages.empty()) {
    const auto& split =
        std::get<rvrbotron::dsp::ResolvedSplit>(
            config.composition.stages.front());
    const auto& diffuser =
        std::get<rvrbotron::dsp::ResolvedDiffuser>(
            config.composition.stages[1]);
    captureSink = std::make_unique<WavStageCaptureSink>(
        resultPath,
        split.channels,
        info.sampleRate,
        static_cast<std::uint32_t>(diffuser.steps.size()));
  }
  rvrbotron::dsp::Reverb reverb(config, captureSink.get());

  std::vector<rvrbotron::dsp::Sample> inputInterleaved(
      arguments.blockSize * inputChannelCount);
  std::vector<rvrbotron::dsp::Sample> inputSamples(
      arguments.blockSize * inputChannelCount);
  std::vector<const rvrbotron::dsp::Sample*> inputs(inputChannelCount);
  for (std::size_t channel = 0; channel < inputChannelCount; ++channel) {
    inputs[channel] =
        inputSamples.data() + channel * arguments.blockSize;
  }
  std::vector<rvrbotron::dsp::Sample> outputSamples(
      arguments.blockSize * outputChannelCount);
  std::vector<rvrbotron::dsp::Sample*> outputs(outputChannelCount);
  for (std::size_t channel = 0; channel < outputChannelCount; ++channel) {
    outputs[channel] =
        outputSamples.data() + channel * arguments.blockSize;
  }
  std::vector<rvrbotron::dsp::Sample> outputInterleaved(
      arguments.blockSize * outputChannelCount);
  std::uint64_t inputFrames = 0;
  std::uint64_t renderedFrames = 0;

  {
    rvrbotron::io::WavWriter writer(
        resultPath / "output.wav",
        static_cast<std::uint32_t>(outputChannelCount),
        info.sampleRate);

    while (true) {
      throwIfInterrupted();
      const auto framesRead =
          reader.readFrames(inputInterleaved.data(), arguments.blockSize);
      throwIfInterrupted();
      if (framesRead == 0) {
        break;
      }

      for (std::size_t frame = 0; frame < framesRead; ++frame) {
        for (std::size_t channel = 0; channel < inputChannelCount; ++channel) {
          inputSamples[channel * arguments.blockSize + frame] =
              inputInterleaved[frame * inputChannelCount + channel];
        }
      }

      reverb.process(
          inputs.data(),
          inputChannelCount,
          outputs.data(),
          outputChannelCount,
          framesRead);

      for (std::size_t frame = 0; frame < framesRead; ++frame) {
        for (std::size_t channel = 0; channel < outputChannelCount; ++channel) {
          outputInterleaved[frame * outputChannelCount + channel] =
              outputSamples[channel * arguments.blockSize + frame];
        }
      }
      writer.writeFrames(outputInterleaved.data(), framesRead);
      inputFrames += framesRead;
      renderedFrames += framesRead;
    }

    std::uint64_t remainingTail = reverb.finiteTailFrames();
    std::fill(
        inputSamples.begin(),
        inputSamples.end(),
        rvrbotron::dsp::Sample{0});
    while (remainingTail != 0) {
      throwIfInterrupted();
      const auto frames = static_cast<std::size_t>(
          std::min<std::uint64_t>(remainingTail, arguments.blockSize));
      reverb.process(
          inputs.data(),
          inputChannelCount,
          outputs.data(),
          outputChannelCount,
          frames);
      for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < outputChannelCount; ++channel) {
          outputInterleaved[frame * outputChannelCount + channel] =
              outputSamples[channel * arguments.blockSize + frame];
        }
      }
      writer.writeFrames(outputInterleaved.data(), frames);
      renderedFrames += frames;
      remainingTail -= frames;
    }
    writer.close();
  }

  std::vector<rvrbotron::cli::StageCaptureMetadata> stageCaptures;
  if (captureSink != nullptr) {
    stageCaptures = captureSink->finish();
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
          static_cast<std::uint32_t>(outputChannelCount),
          inputFrames,
          renderedFrames,
          arguments.blockSize,
          arguments.captureAllStages
              ? std::optional<std::string>{"all-v1"}
              : std::nullopt,
          std::move(stageCaptures),
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
    const std::string command = argc >= 2 ? argv[1] : "";
    if (command == "render") {
      render(parseArguments(argc, argv));
    } else if (command == "benchmark") {
      rvrbotron::cli::runBenchmark(argc, argv);
    } else {
      throw rvrbotron::HarnessError(
          rvrbotron::ErrorCategory::invalidArguments,
          "usage: rvrbotron <render|benchmark> ...");
    }
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
