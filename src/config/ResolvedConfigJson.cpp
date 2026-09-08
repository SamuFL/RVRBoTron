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
  case dsp::DelayStrategy::uniformRandom:
    return "uniform-random";
  case dsp::DelayStrategy::even:
    return "even";
  }
  throw HarnessError(
      ErrorCategory::invalidConfiguration,
      "unsupported Diffusion Step delay strategy");
}

const char* mixMatrixTypeName(const dsp::MixMatrixType mix) {
  switch (mix) {
  case dsp::MixMatrixType::hadamard:
    return "hadamard";
  case dsp::MixMatrixType::householder:
    return "householder";
  case dsp::MixMatrixType::randomOrthogonal:
    return "random-orthogonal";
  }
  throw HarnessError(
      ErrorCategory::invalidConfiguration,
      "unsupported Diffusion Step mix");
}

const char* modulationInterpolationName(
    const dsp::ModulationInterpolation interpolation) {
  switch (interpolation) {
  case dsp::ModulationInterpolation::lagrange3:
    return "lagrange3";
  case dsp::ModulationInterpolation::linear:
    return "linear";
  }
  throw HarnessError(
      ErrorCategory::invalidConfiguration,
      "unsupported Modulation interpolation");
}

const char* modulationShapeName(const dsp::ModulationShape shape) {
  switch (shape) {
  case dsp::ModulationShape::smoothedRandom:
    return "smoothed-random";
  case dsp::ModulationShape::sine:
    return "sine";
  case dsp::ModulationShape::triangle:
    return "triangle";
  }
  throw HarnessError(
      ErrorCategory::invalidConfiguration, "unsupported Modulation shape");
}

const char* gainModeName(const dsp::GainMode gainMode) {
  switch (gainMode) {
  case dsp::GainMode::perChannel:
    return "per-channel";
  case dsp::GainMode::uniform:
    return "uniform";
  }
  throw HarnessError(
      ErrorCategory::invalidConfiguration,
      "unsupported Feedback Loop gainMode");
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

// Shared by the Feedback Loop and each Diffusion Step (issues #89/#91):
// the two modulated stages must serialize identically so a comparison
// between them is a comparison of values, not of schema.
Json modulationJson(const dsp::ResolvedModulation& modulation) {
  return {
      {"depthMs", modulation.depthMs},
      {"rateHz", modulation.rateHz},
      {"shape", modulationShapeName(modulation.shape)},
      {"channelFraction", modulation.channelFraction},
      {"interpolation",
       modulationInterpolationName(modulation.interpolation)},
      {"excursionSamples", modulation.excursionSamples},
      {"interpolationMarginSamples", modulation.interpolationMarginSamples},
      {"channelSeeds", modulation.channelSeeds},
      {"channelTargetsPerSample", modulation.channelTargetsPerSample},
      {"channelPhases", modulation.channelPhases},
      {"channelModulated", modulation.channelModulated},
  };
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
    Json stepJson{
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
        {"mix", mixMatrixTypeName(step.mix)},
        {"matrix", std::move(matrix)},
    };
    // Omitted entirely (rather than emitted as null) when disabled, so a
    // step with no Modulation configured remains byte-identical to one
    // written before Diffusion Step Modulation existed (see issue #91).
    if (step.modulation.has_value()) {
      stepJson["modulation"] = modulationJson(*step.modulation);
    }
    steps.push_back(std::move(stepJson));
  }
  return {
      {"type", "diffuser"},
      {"totalSamples", diffuser.totalSamples},
      {"steps", std::move(steps)},
  };
}

Json feedbackLoopJson(const dsp::ResolvedFeedbackLoop& loop) {
  Json matrix = Json::array();
  const auto channels = loop.channels;
  for (std::uint32_t row = 0; row < channels; ++row) {
    Json values = Json::array();
    for (std::uint32_t column = 0; column < channels; ++column) {
      values.push_back(
          loop.matrix[static_cast<std::size_t>(row) * channels + column]);
    }
    matrix.push_back(std::move(values));
  }
  Json document{
      {"type", "feedback-loop"},
      {"channels", loop.channels},
      {"delayMinSamples", loop.delayMinSamples},
      {"delayMaxSamples", loop.delayMaxSamples},
      {"delayMinMs", loop.delayMinMs},
      {"delayMaxMs", loop.delayMaxMs},
      {"delayStrategy", delayStrategyName(loop.delayStrategy)},
      {"delaysSamples", loop.delaysSamples},
      {"delaysMs", loop.delaysMs},
      {"bufferSizes", loop.bufferSizes},
      {"rt60Sec", loop.rt60Sec},
      {"gainMode", gainModeName(loop.gainMode)},
      {"gains", loop.gains},
      {"mix", mixMatrixTypeName(loop.mix)},
      {"matrix", std::move(matrix)},
      {"decayMargin", loop.decayMargin},
      {"tailBudgetSamples", loop.tailBudgetSamples},
      {"blockSizeBoundSamples", loop.blockSizeBoundSamples},
      {"silenceFloorDb",
       loop.silenceFloorDb.has_value() ? Json(*loop.silenceFloorDb)
                                        : Json(nullptr)},
  };
  // Omitted entirely (rather than emitted as null) when disabled, so an
  // existing format-version-1 Resolved Configuration written before
  // Damping existed remains byte-identical to one produced with it
  // disabled today, and loads back as disabled (see issue #75).
  if (loop.damping.has_value()) {
    const auto& damping = *loop.damping;
    document["damping"] = {
        {"highRatio", damping.highRatio},
        {"highHz", damping.highHz},
        {"lowRatio", damping.lowRatio},
        {"lowHz", damping.lowHz},
        {"highShelfGains", damping.highShelfGains},
        {"highShelfB0", damping.highShelfB0},
        {"highShelfB1", damping.highShelfB1},
        {"highShelfA1", damping.highShelfA1},
        {"lowShelfGains", damping.lowShelfGains},
        {"lowShelfB0", damping.lowShelfB0},
        {"lowShelfB1", damping.lowShelfB1},
        {"lowShelfA1", damping.lowShelfA1},
        {"expectedLowRt60Sec", damping.expectedLowRt60Sec},
        {"expectedReferenceRt60Sec", damping.expectedReferenceRt60Sec},
        {"expectedHighRt60Sec", damping.expectedHighRt60Sec},
        {"contractionBoundFloat32", damping.contractionBoundFloat32},
        {"contractionMarginFloat32", damping.contractionMarginFloat32},
        {"contractionBoundFloat64", damping.contractionBoundFloat64},
        {"contractionMarginFloat64", damping.contractionMarginFloat64},
        {"slowestResolvedRt60Sec", damping.slowestResolvedRt60Sec},
    };
  }
  // Omitted entirely (rather than emitted as null) when disabled, so an
  // existing format-version-1 Resolved Configuration written before
  // Modulation existed remains byte-identical to one produced with it
  // disabled today, and loads back as disabled (see issue #89).
  if (loop.modulation.has_value()) {
    document["modulation"] = modulationJson(*loop.modulation);
  }
  return document;
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
          } else if constexpr (
              std::is_same_v<Stage, dsp::ResolvedFeedbackLoop>) {
            return feedbackLoopJson(value);
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
