#include "rvrbotron/cli/Benchmark.h"

#include "rvrbotron/HarnessError.h"
#include "rvrbotron/cli/ConfigJson.h"
#include "rvrbotron/cli/Platform.h"
#include "rvrbotron/dsp/Reverb.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#if defined(_WIN32)
// windows.h must precede psapi.h: psapi.h uses types (ULONG_PTR, PVOID, ...)
// that only windows.h defines.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <cstdio>
#endif

namespace rvrbotron::cli {
namespace {

using Clock = std::chrono::steady_clock;

struct BenchmarkArguments {
  std::filesystem::path resolvedConfig;
  std::size_t blockSize = 128;
  double warmupSeconds = 1.0;
  double measureSeconds = 5.0;
  std::optional<std::filesystem::path> jsonOutput;
};

[[noreturn]] void failArguments(const std::string_view reason) {
  throw HarnessError(ErrorCategory::invalidArguments, std::string(reason));
}

std::size_t parseBlockSize(const std::string_view value) {
  std::size_t blockSize = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), blockSize);
  if (result.ec != std::errc{} ||
      result.ptr != value.data() + value.size() || blockSize == 0) {
    failArguments("--block-size requires a positive integer");
  }
  return blockSize;
}

double parseSeconds(const std::string_view value,
                    const std::string_view option) {
  double seconds = 0.0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), seconds);
  if (result.ec != std::errc{} ||
      result.ptr != value.data() + value.size() || !(seconds >= 0.0)) {
    failArguments(
        std::string(option) + " requires a nonnegative number of seconds");
  }
  return seconds;
}

BenchmarkArguments parseArguments(const int argc, char** const argv) {
  if (argc % 2 != 0) {
    failArguments(
        "usage: rvrbotron benchmark --resolved <resolved.json> "
        "[--block-size <frames>] [--warmup-seconds <seconds>] "
        "[--measure-seconds <seconds>] [--json <report.json>] "
        "[--error-format <text|json>]");
  }

  BenchmarkArguments arguments;
  for (int index = 2; index < argc; index += 2) {
    const std::string option = argv[index];
    if (option == "--resolved") {
      arguments.resolvedConfig = argv[index + 1];
    } else if (option == "--block-size") {
      arguments.blockSize = parseBlockSize(argv[index + 1]);
    } else if (option == "--warmup-seconds") {
      arguments.warmupSeconds =
          parseSeconds(argv[index + 1], "--warmup-seconds");
    } else if (option == "--measure-seconds") {
      arguments.measureSeconds =
          parseSeconds(argv[index + 1], "--measure-seconds");
    } else if (option == "--json") {
      arguments.jsonOutput = argv[index + 1];
    } else if (option == "--error-format") {
      const std::string_view format = argv[index + 1];
      if (format != "text" && format != "json") {
        failArguments("--error-format requires text or json");
      }
    } else {
      failArguments("unknown option: " + option);
    }
  }

  if (arguments.resolvedConfig.empty()) {
    failArguments("--resolved is required");
  }
  return arguments;
}

std::string readFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw HarnessError(
        ErrorCategory::ioFailure,
        "could not open Resolved Configuration: " + path.string());
  }
  return {
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>(),
  };
}

// Best-effort process resident-set-size snapshot; nullopt if unsupported.
// Allocator and OS scheduling noise make this evidence qualitative, not a
// precise DSP-owned accounting -- see Reverb::ownedBytes for the exact
// figure.
std::optional<std::uint64_t> residentSetSizeBytes() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters{};
  if (!GetProcessMemoryInfo(
          GetCurrentProcess(), &counters, sizeof(counters))) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(counters.WorkingSetSize);
#elif defined(__APPLE__)
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  const auto result = task_info(
      mach_task_self(),
      MACH_TASK_BASIC_INFO,
      reinterpret_cast<task_info_t>(&info),
      &count);
  if (result != KERN_SUCCESS) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(info.resident_size);
#elif defined(__linux__)
  std::FILE* status = std::fopen("/proc/self/status", "r");
  if (status == nullptr) {
    return std::nullopt;
  }
  unsigned long long kib = 0;
  bool found = false;
  char line[256];
  while (std::fgets(line, sizeof(line), status) != nullptr) {
    if (std::sscanf(line, "VmRSS: %llu kB", &kib) == 1) {
      found = true;
      break;
    }
  }
  std::fclose(status);
  return found
             ? std::optional<std::uint64_t>(
                   static_cast<std::uint64_t>(kib) * 1024ULL)
             : std::nullopt;
#else
  return std::nullopt;
#endif
}

std::string_view samplePrecisionName() noexcept {
  return sizeof(dsp::Sample) == sizeof(double) ? "float64" : "float32";
}

bool isOptimizedBuildType(const std::string_view buildType) noexcept {
  return buildType == "Release" || buildType == "RelWithDebInfo" ||
         buildType == "MinSizeRel";
}

std::string_view mixMatrixTypeName(const dsp::MixMatrixType mix) noexcept {
  switch (mix) {
  case dsp::MixMatrixType::hadamard:
    return "hadamard";
  case dsp::MixMatrixType::householder:
    return "householder";
  case dsp::MixMatrixType::randomOrthogonal:
    return "random-orthogonal";
  }
  return "unknown";
}

std::string_view modulationInterpolationName(
    const dsp::ModulationInterpolation interpolation) noexcept {
  switch (interpolation) {
  case dsp::ModulationInterpolation::lagrange3:
    return "lagrange3";
  case dsp::ModulationInterpolation::linear:
    return "linear";
  }
  return "unknown";
}

// Whether a resolved Modulation object actually moves at least one
// Channel (see docs/design/reverb/stages/06-modulation.md's "Identity is
// guaranteed by construction, not by arithmetic"): an omitted object, an
// explicit zero depth, and a zero channelFraction all resolve to an empty
// `channelModulated` and therefore never reach the interpolated read path
// this benchmark exists to cost -- reporting an interpolation method for
// any of those would name a choice that made no difference to the
// measured time.
bool modulationActive(
    const std::optional<dsp::ResolvedModulation>& modulation) noexcept {
  return modulation.has_value() && !modulation->channelModulated.empty();
}

// Deterministic nonzero pseudo-random block generator (xorshift64*), so
// benchmark runs are reproducible and no measured block is silently zero.
class DeterministicSource {
public:
  explicit DeterministicSource(const std::uint64_t seed) noexcept
      : state_(seed != 0 ? seed : 0x9e3779b97f4a7c15ULL) {}

  dsp::Sample next() noexcept {
    state_ ^= state_ >> 12U;
    state_ ^= state_ << 25U;
    state_ ^= state_ >> 27U;
    const auto bits = state_ * 0x2545f4914f6cdd1dULL;
    const auto value =
        static_cast<int>((bits >> 32U) % 2000U) - 1000;
    // Nonzero: shift the [-1000, 999] range away from zero, then rescale.
    const auto shifted = value >= 0 ? value + 1 : value;
    return static_cast<dsp::Sample>(static_cast<double>(shifted) / 1000.0);
  }

private:
  std::uint64_t state_;
};

struct TimingSummary {
  std::size_t blockCount = 0;
  double medianSeconds = 0.0;
  double p95Seconds = 0.0;
  double worstSeconds = 0.0;
  std::size_t missedDeadlineCount = 0;
};

TimingSummary summarize(std::vector<double> blockSeconds,
                        const double realTimeBudgetSeconds) {
  const auto missedDeadlineCount = static_cast<std::size_t>(std::count_if(
      blockSeconds.begin(), blockSeconds.end(),
      [realTimeBudgetSeconds](const double sample) {
        return sample > realTimeBudgetSeconds;
      }));
  std::sort(blockSeconds.begin(), blockSeconds.end());
  const auto percentile = [&](const double fraction) {
    if (blockSeconds.empty()) {
      return 0.0;
    }
    const auto index = static_cast<std::size_t>(
        fraction * static_cast<double>(blockSeconds.size() - 1));
    return blockSeconds[index];
  };
  return {
      blockSeconds.size(),
      percentile(0.5),
      percentile(0.95),
      blockSeconds.empty() ? 0.0 : blockSeconds.back(),
      missedDeadlineCount,
  };
}

} // namespace

void runBenchmark(const int argc, char** const argv) {
  const auto arguments = parseArguments(argc, argv);
  const auto contents = readFile(arguments.resolvedConfig);
  const auto config = parseResolvedConfig(contents);
  if (config.composition.stages.empty()) {
    throw HarnessError(
        ErrorCategory::invalidConfiguration,
        "benchmark requires a non-empty Composition",
        "/composition/stages");
  }
  // Every non-empty Composition starts with Split (see stage 09's valid
  // shapes), but a Diffuser is optional -- [split, feedback-loop, downmix]
  // benchmarks a Feedback Loop with no Diffuser at all, so it is searched
  // for rather than assumed to sit at a fixed index.
  const auto& split =
      std::get<dsp::ResolvedSplit>(config.composition.stages[0]);
  const dsp::ResolvedDiffuser* diffuser = nullptr;
  const dsp::ResolvedFeedbackLoop* feedbackLoop = nullptr;
  for (const auto& stage : config.composition.stages) {
    if (const auto* const candidate = std::get_if<dsp::ResolvedDiffuser>(&stage)) {
      diffuser = candidate;
    } else if (
        const auto* const loopCandidate =
            std::get_if<dsp::ResolvedFeedbackLoop>(&stage)) {
      feedbackLoop = loopCandidate;
    }
  }

  const auto buildType = std::string_view(RVRBOTRON_BUILD_TYPE);
  const auto isDebugBuild = !isOptimizedBuildType(buildType);
  if (isDebugBuild) {
    std::cerr
        << "WARNING: benchmarking a non-optimized (" << buildType
        << ") build. Use the release or release-double preset for "
           "meaningful timing evidence.\n";
  }

  dsp::Reverb reverb(config);
  if (reverb.inputChannelCount() > 2 || reverb.outputChannelCount() > 2) {
    throw HarnessError(
        ErrorCategory::invalidConfiguration,
        "benchmark requires mono or stereo Reverb Channel counts");
  }

  const auto inputChannelCount = reverb.inputChannelCount();
  const auto outputChannelCount = reverb.outputChannelCount();
  const auto blockSize = arguments.blockSize;

  std::vector<dsp::Sample> inputStorage(blockSize * inputChannelCount);
  std::vector<const dsp::Sample*> inputs(inputChannelCount);
  for (std::size_t channel = 0; channel < inputChannelCount; ++channel) {
    inputs[channel] = inputStorage.data() + channel * blockSize;
  }
  DeterministicSource source(config.seed ^ 0xd1b54a32d192ed03ULL);
  for (auto& sample : inputStorage) {
    sample = source.next();
  }

  std::vector<dsp::Sample> outputStorage(blockSize * outputChannelCount);
  std::vector<dsp::Sample*> outputs(outputChannelCount);
  for (std::size_t channel = 0; channel < outputChannelCount; ++channel) {
    outputs[channel] = outputStorage.data() + channel * blockSize;
  }

  const auto residentBefore = residentSetSizeBytes();

  const auto warmupDeadline =
      Clock::now() + std::chrono::duration_cast<Clock::duration>(
                          std::chrono::duration<double>(
                              arguments.warmupSeconds));
  while (Clock::now() < warmupDeadline) {
    reverb.process(
        inputs.data(), inputChannelCount, outputs.data(),
        outputChannelCount, blockSize);
  }

  std::vector<double> blockSeconds;
  const auto sampleRate = config.sampleRate;
  const auto estimatedBlocksPerSecond =
      sampleRate == 0 ? 1.0
                      : static_cast<double>(sampleRate) /
                            static_cast<double>(blockSize);
  blockSeconds.reserve(static_cast<std::size_t>(
      arguments.measureSeconds * estimatedBlocksPerSecond * 2.0 + 1.0));

  const auto measureDeadline =
      Clock::now() + std::chrono::duration_cast<Clock::duration>(
                          std::chrono::duration<double>(
                              arguments.measureSeconds));
  do {
    const auto start = Clock::now();
    reverb.process(
        inputs.data(), inputChannelCount, outputs.data(),
        outputChannelCount, blockSize);
    const auto end = Clock::now();
    blockSeconds.push_back(std::chrono::duration<double>(end - start).count());
  } while (Clock::now() < measureDeadline);

  const auto residentAfter = residentSetSizeBytes();
  std::optional<std::int64_t> residentDeltaBytes;
  if (residentBefore.has_value() && residentAfter.has_value()) {
    residentDeltaBytes = static_cast<std::int64_t>(*residentAfter) -
                         static_cast<std::int64_t>(*residentBefore);
  }

  const auto realTimeBudgetSeconds =
      sampleRate == 0 ? 0.0
                      : static_cast<double>(blockSize) /
                            static_cast<double>(sampleRate);
  const auto timing =
      summarize(std::move(blockSeconds), realTimeBudgetSeconds);

  const auto dspOwnedBytes = reverb.ownedBytes();

  nlohmann::json matrixByStep = nlohmann::json::array();
  // One entry per Diffusion Step, null unless that step's own Modulation
  // actually moves at least one Channel -- so a benchmark run against a
  // resolved.json is self-describing about which interpolation method(s)
  // its own timing evidence actually cost, and two runs (say lagrange3
  // against linear) stay directly comparable without the caller having to
  // separately track which resolved.json produced which report (issue
  // #92's "DSP benchmark reports this method's own processing cost").
  nlohmann::json modulationInterpolationByStep = nlohmann::json::array();
  if (diffuser != nullptr) {
    for (const auto& step : diffuser->steps) {
      matrixByStep.push_back(mixMatrixTypeName(step.mix));
      modulationInterpolationByStep.push_back(
          modulationActive(step.modulation)
              ? nlohmann::json(
                    modulationInterpolationName(step.modulation->interpolation))
              : nlohmann::json(nullptr));
    }
  }
  const nlohmann::json feedbackLoopModulationInterpolation =
      feedbackLoop != nullptr && modulationActive(feedbackLoop->modulation)
          ? nlohmann::json(modulationInterpolationName(
                feedbackLoop->modulation->interpolation))
          : nlohmann::json(nullptr);

  const nlohmann::json report{
      {"formatVersion", 1},
      {"provenance",
       {
           {"rendererVersion", RVRBOTRON_VERSION},
           {"platform", platformName()},
           {"architecture", architectureName()},
           {"compiler", compilerName()},
           {"buildType", buildType},
           {"samplePrecision", samplePrecisionName()},
           {"sampleRate", sampleRate},
           {"blockSize", blockSize},
           {"channels", split.channels},
           {"stepCount", diffuser != nullptr ? diffuser->steps.size() : std::size_t{0}},
           {"matrixByStep", matrixByStep},
           {"modulationInterpolationByStep", modulationInterpolationByStep},
           {"feedbackLoopModulationInterpolation",
            feedbackLoopModulationInterpolation},
       }},
      {"warmupSeconds", arguments.warmupSeconds},
      {"measureSeconds", arguments.measureSeconds},
      {"blockCount", timing.blockCount},
      {"medianBlockSeconds", timing.medianSeconds},
      {"p95BlockSeconds", timing.p95Seconds},
      {"worstBlockSeconds", timing.worstSeconds},
      {"realTimeBudgetSeconds", realTimeBudgetSeconds},
      {"medianUtilization",
       realTimeBudgetSeconds > 0.0
           ? timing.medianSeconds / realTimeBudgetSeconds
           : 0.0},
      {"worstUtilization",
       realTimeBudgetSeconds > 0.0
           ? timing.worstSeconds / realTimeBudgetSeconds
           : 0.0},
      {"missedDeadlineCount", timing.missedDeadlineCount},
      {"dspOwnedBytes", dspOwnedBytes},
      {"residentSetSizeDeltaBytes",
       residentDeltaBytes.has_value()
           ? nlohmann::json(*residentDeltaBytes)
           : nlohmann::json(nullptr)},
      {"residentSetSizeDeltaNote",
       "best-effort; allocator- and runtime-noisy evidence, not an exact "
       "DSP accounting -- see dspOwnedBytes for that"},
      {"isDebugBuild", isDebugBuild},
  };

  const auto rendered = report.dump(2);
  std::cout << rendered << '\n';
  if (arguments.jsonOutput.has_value()) {
    std::ofstream output(*arguments.jsonOutput, std::ios::binary);
    if (!output) {
      throw HarnessError(
          ErrorCategory::ioFailure, "could not write benchmark JSON report");
    }
    output << rendered << '\n';
    output.flush();
    if (!output) {
      throw HarnessError(
          ErrorCategory::ioFailure, "could not write benchmark JSON report");
    }
  }
}

} // namespace rvrbotron::cli
