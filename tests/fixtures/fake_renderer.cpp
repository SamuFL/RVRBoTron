// A stand-in renderer CLI for Research bench hardening tests (issue #139).
//
// Speaks just enough of the real contract (src/cli/main.cpp,
// src/cli/RenderMetadata.cpp) for serve.py to accept it and drive it: a
// usage line with no arguments, an --error-format json invalid_arguments
// diagnostic when --input/--output are missing, and, on a full render
// invocation, a render.json and output.wav under --output.
//
// The real renderer takes minutes for some configurations and cannot be
// told to fail or stall on demand, which several hardening behaviors
// (concurrency rejection, terminal interruption, float64 output rejection)
// need to exercise deterministically. This stand-in reads test-only knobs
// from extra top-level keys in the --config JSON -- keys the real renderer
// would simply ignore -- rather than adding any such thing to serve.py
// itself:
//
//   _sleepSeconds    -- wait this long before finishing, to simulate a slow
//                       render
//   _samplePrecision -- report this in render.json instead of "float32"
//
// Before either sleeping or finishing, a PID file and a STARTED file are
// written under --output, so a test can wait for the render to have
// actually begun and can identify the exact process to check for after
// killing it.
//
// This is a real compiled executable, not a Python script, because
// serve.py launches --renderer directly (as it must for the real renderer
// binary): Windows' CreateProcess cannot run a script through its shebang
// line the way a POSIX exec can, so a Python stand-in cannot serve as a
// --renderer path on every platform this bench and its CI target.

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

const char* kUsage = "usage: rvrbotron <render|benchmark> ...\n";

std::optional<std::string> findOption(const std::vector<std::string>& args,
                                       const std::string& name) {
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == name && i + 1 < args.size()) {
      return args[i + 1];
    }
  }
  return std::nullopt;
}

int fail(const std::string& category, const std::string& reason,
         int exitCode) {
  std::cerr << json{{"category", category}, {"reason", reason}}.dump()
            << "\n";
  return exitCode;
}

long currentPid() {
#ifdef _WIN32
  return static_cast<long>(_getpid());
#else
  return static_cast<long>(getpid());
#endif
}

void writeText(const fs::path& path, const std::string& text) {
  std::ofstream stream(path, std::ios::binary);
  stream << text;
}

void writeMinimalWav(const fs::path& path) {
  // One silent mono PCM16 frame: just enough to be a well-formed WAV.
  static const unsigned char frame[2] = {0, 0};
  static const std::uint32_t sampleRate = 48000;
  static const std::uint16_t blockAlign = 2;
  static const std::uint32_t dataSize = sizeof(frame);
  static const std::uint32_t riffSize = 36 + dataSize;

  std::ofstream stream(path, std::ios::binary);
  stream.write("RIFF", 4);
  stream.write(reinterpret_cast<const char*>(&riffSize), 4);
  stream.write("WAVE", 4);
  stream.write("fmt ", 4);
  const std::uint32_t fmtChunkSize = 16;
  stream.write(reinterpret_cast<const char*>(&fmtChunkSize), 4);
  const std::uint16_t formatTag = 1; // PCM
  stream.write(reinterpret_cast<const char*>(&formatTag), 2);
  const std::uint16_t channels = 1;
  stream.write(reinterpret_cast<const char*>(&channels), 2);
  stream.write(reinterpret_cast<const char*>(&sampleRate), 4);
  const std::uint32_t byteRate = sampleRate * blockAlign;
  stream.write(reinterpret_cast<const char*>(&byteRate), 4);
  stream.write(reinterpret_cast<const char*>(&blockAlign), 2);
  const std::uint16_t bitsPerSample = 16;
  stream.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
  stream.write("data", 4);
  stream.write(reinterpret_cast<const char*>(&dataSize), 4);
  stream.write(reinterpret_cast<const char*>(frame), sizeof(frame));
}

} // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty() || args.front() != "render") {
    std::cerr << kUsage;
    return 1;
  }
  const std::vector<std::string> rest(args.begin() + 1, args.end());

  const auto input = findOption(rest, "--input");
  const auto output = findOption(rest, "--output");
  if (!input || !output) {
    return fail("invalid_arguments", "--input and --output are required", 7);
  }

  const fs::path outputDir(*output);
  std::error_code ec;
  fs::create_directories(outputDir, ec);

  json config = json::object();
  if (const auto configPath = findOption(rest, "--config")) {
    std::ifstream in(*configPath);
    if (!in) {
      return fail("invalid_configuration", "could not read config", 3);
    }
    try {
      in >> config;
    } catch (const json::parse_error&) {
      return fail("invalid_configuration", "malformed JSON", 3);
    }
  }

  writeText(outputDir / "PID", std::to_string(currentPid()));
  writeText(outputDir / "STARTED", "1");

  if (const auto sleepSeconds = config.find("_sleepSeconds");
      sleepSeconds != config.end()) {
    std::this_thread::sleep_for(
        std::chrono::duration<double>(sleepSeconds->get<double>()));
  }

  const json metadata{
      {"formatVersion", 1},
      {"rendererVersion", "fake"},
      {"platform", "test"},
      {"architecture", "test"},
      {"samplePrecision", config.value("_samplePrecision", std::string("float32"))},
      {"blockSize", 512},
      {"configurationInput", "requested"},
      {"inputFilename", fs::path(*input).filename().string()},
      {"inputSha256", std::string(64, '0')},
      {"sampleRate", 48000},
      {"channels", 1},
      {"frames", 1},
      {"inputFrames", 1},
      {"preDelayFrames", 0},
      {"tailBudgetFrames", 0},
  };
  writeText(outputDir / "render.json", metadata.dump());
  writeMinimalWav(outputDir / "output.wav");
  return 0;
}
