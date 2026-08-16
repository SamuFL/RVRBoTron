#include "rvrbotron/dsp/Composition.h"
#include "rvrbotron/io/WavStream.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kBlockSize = 512;

struct RenderArguments {
  std::filesystem::path input;
  std::filesystem::path output;
};

RenderArguments parseArguments(const int argc, char** argv) {
  if (argc != 6 || std::string(argv[1]) != "render") {
    throw std::runtime_error(
        "usage: rvrbotron render --input <wav> --output <result-dir>");
  }

  RenderArguments arguments;
  for (int index = 2; index < argc; index += 2) {
    const std::string option = argv[index];
    if (option == "--input") {
      arguments.input = argv[index + 1];
    } else if (option == "--output") {
      arguments.output = argv[index + 1];
    } else {
      throw std::runtime_error("unknown option: " + option);
    }
  }

  if (arguments.input.empty() || arguments.output.empty()) {
    throw std::runtime_error("--input and --output are required");
  }

  return arguments;
}

void writeResolvedConfig(const std::filesystem::path& path,
                         const std::uint32_t sampleRate) {
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("could not write resolved.json");
  }

  output << "{\n"
         << "  \"formatVersion\": 1,\n"
         << "  \"seed\": 0,\n"
         << "  \"sampleRate\": " << sampleRate << ",\n"
         << "  \"composition\": {\"stages\": []}\n"
         << "}\n";
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
         << "  \"samplePrecision\": \"float32\",\n"
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
  if (!std::filesystem::create_directories(arguments.output)) {
    throw std::runtime_error("could not create output directory");
  }

  rvrbotron::io::WavReader reader(arguments.input);
  const auto info = reader.info();
  if (info.channels != 1) {
    throw std::runtime_error("the first identity renderer requires mono input");
  }

  const rvrbotron::dsp::ResolvedConfig config{
      1,
      0,
      info.sampleRate,
  };
  rvrbotron::dsp::Composition composition(config);

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

      composition.process(channels, info.channels, framesRead);
      writer.writeFrames(samples.data(), framesRead);
      renderedFrames += framesRead;
    }
  }

  writeResolvedConfig(arguments.output / "resolved.json", info.sampleRate);
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
