#include "rvrbotron/config/ResolvedConfigJson.h"

#include "rvrbotron/HarnessError.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <type_traits>
#include <variant>

namespace rvrbotron::config {
namespace {

using Json = nlohmann::json;

const char* normalisationName(
    const dsp::EnergyNormalisation normalisation) {
  switch (normalisation) {
  case dsp::EnergyNormalisation::energy:
    return "energy";
  case dsp::EnergyNormalisation::none:
    return "none";
  }
  throw HarnessError(
      ErrorCategory::invalidConfiguration,
      "unsupported normalisation");
}

const char* delayStrategyName(const dsp::DelayStrategy strategy) {
  switch (strategy) {
  case dsp::DelayStrategy::segmentedRandom:
    return "segmented-random";
  case dsp::DelayStrategy::even:
    return "even";
  }
  throw HarnessError(
      ErrorCategory::invalidConfiguration,
      "unsupported Diffusion Step delay strategy");
}

const char* polarityName(const dsp::PolarityStrategy polarity) {
  switch (polarity) {
  case dsp::PolarityStrategy::seededRandom:
    return "seeded-random";
  case dsp::PolarityStrategy::none:
    return "none";
  }
  throw HarnessError(
      ErrorCategory::invalidConfiguration,
      "unsupported Diffusion Step polarity");
}

const char* splitStrategyName(const dsp::SplitStrategyType strategy) {
  switch (strategy) {
  case dsp::SplitStrategyType::duplicate:
    return "duplicate";
  case dsp::SplitStrategyType::stereoHalves:
    return "stereo-halves";
  case dsp::SplitStrategyType::stereoInterleave:
    return "stereo-interleave";
  }
  throw HarnessError(
      ErrorCategory::invalidConfiguration,
      "unsupported Split strategy");
}

Json splitJson(const dsp::ResolvedSplit& split) {
  return {
      {"type", "split"},
      {"inputChannels", split.inputChannels},
      {"channels", split.channels},
      {"strategy", splitStrategyName(split.strategy)},
      {"normalisation", normalisationName(split.normalisation)},
      {"sourceGain", split.sourceGain},
      {"channelGain", split.channelGain},
  };
}

Json diffuserJson(const dsp::ResolvedDiffuser& diffuser) {
  Json steps = Json::array();
  for (const auto& step : diffuser.steps) {
    Json matrix = Json::array();
    const auto channels = step.delaysSamples.size();
    for (std::size_t row = 0; row < channels; ++row) {
      Json values = Json::array();
      for (std::size_t column = 0; column < channels; ++column) {
        values.push_back(step.matrix[row * channels + column]);
      }
      matrix.push_back(std::move(values));
    }
    steps.push_back(
        {
            {"index", step.index},
            {"lengthSamples", step.lengthSamples},
            {"lengthMs", step.lengthMs},
            {"delayStrategy", delayStrategyName(step.delayStrategy)},
            {"delaysSamples", step.delaysSamples},
            {"delaysMs", step.delaysMs},
            {"bufferSizes", step.bufferSizes},
            {"shuffle", step.shuffle},
            {"permutation", step.permutation},
            {"polarity", polarityName(step.polarity)},
            {"polaritySigns", step.polaritySigns},
            {"mix", "hadamard"},
            {"matrix", std::move(matrix)},
        });
  }
  return {
      {"type", "diffuser"},
      {"totalSamples", diffuser.totalSamples},
      {"steps", std::move(steps)},
  };
}

Json downmixJson(const dsp::ResolvedDownmix& downmix) {
  return {
      {"type", "downmix"},
      {"inputChannels", downmix.inputChannels},
      {"outputChannels", downmix.outputChannels},
      {"strategy", "select"},
      {"normalisation", normalisationName(downmix.normalisation)},
      {"compensation", downmix.compensation},
  };
}

Json compositionJson(const dsp::ResolvedComposition& composition) {
  Json stages = Json::array();
  for (const auto& stage : composition.stages) {
    stages.push_back(std::visit(
        [](const auto& value) -> Json {
          using Stage = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Stage, dsp::ResolvedSplit>) {
            return splitJson(value);
          } else if constexpr (
              std::is_same_v<Stage, dsp::ResolvedDiffuser>) {
            return diffuserJson(value);
          } else {
            return downmixJson(value);
          }
        },
        stage));
  }
  return {{"stages", std::move(stages)}};
}

} // namespace

void writeResolvedConfig(const std::filesystem::path& path,
                         const dsp::ResolvedConfig& config) {
  std::ofstream output(path);
  if (!output) {
    throw HarnessError(
        ErrorCategory::ioFailure, "could not write resolved.json");
  }

  const Json document{
      {"formatVersion", config.formatVersion},
      {"seed", config.seed},
      {"sampleRate", config.sampleRate},
      {"composition", compositionJson(config.composition)},
  };
  output << document.dump(2) << '\n';
  output.flush();
  if (!output) {
    throw HarnessError(
        ErrorCategory::ioFailure, "could not write resolved.json");
  }
}

} // namespace rvrbotron::config
