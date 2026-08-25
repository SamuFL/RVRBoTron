#include "rvrbotron/cli/ConfigJson.h"

#include "rvrbotron/HarnessError.h"
#include "rvrbotron/config/ResolveConfig.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>

namespace rvrbotron::cli {
namespace {

using Json = nlohmann::json;

[[noreturn]] void fail(const std::string_view path,
                       const std::string_view reason) {
  throw HarnessError(
      ErrorCategory::invalidConfiguration,
      std::string(reason),
      std::string(path));
}

Json parseJson(const std::string_view contents) {
  try {
    return Json::parse(contents);
  } catch (const Json::parse_error& error) {
    throw HarnessError(
        ErrorCategory::malformedJson,
        "malformed JSON: " + std::string(error.what()),
        "/");
  } catch (const Json::exception& error) {
    throw HarnessError(
        ErrorCategory::malformedJson,
        "malformed JSON: " + std::string(error.what()),
        "/");
  }
}

void requireObject(const Json& value, const std::string_view path) {
  if (!value.is_object()) {
    fail(path, "expected object");
  }
}

void requireArray(const Json& value, const std::string_view path) {
  if (!value.is_array()) {
    fail(path, "expected array");
  }
}

void requireField(const Json& object,
                  const std::string_view field,
                  const std::string_view path) {
  if (!object.contains(field)) {
    fail(
        std::string(path) + "/" + std::string(field),
        "required field is missing");
  }
}

void rejectUnknownFields(
    const Json& object,
    const std::string_view path,
    const std::initializer_list<std::string_view> allowed) {
  for (const auto& [key, value] : object.items()) {
    static_cast<void>(value);
    bool found = false;
    for (const auto candidate : allowed) {
      if (key == candidate) {
        found = true;
        break;
      }
    }
    if (!found) {
      const std::string childPath =
          path == "/" ? "/" + key : std::string(path) + "/" + key;
      fail(childPath, "unknown field");
    }
  }
}

std::string parseString(const Json& value, const std::string_view path) {
  if (!value.is_string()) {
    fail(path, "expected string");
  }
  return value.get<std::string>();
}

bool parseBoolean(const Json& value, const std::string_view path) {
  if (!value.is_boolean()) {
    fail(path, "expected boolean");
  }
  return value.get<bool>();
}

double parseNumber(const Json& value, const std::string_view path) {
  if (!value.is_number()) {
    fail(path, "expected number");
  }
  const auto number = value.get<double>();
  if (!std::isfinite(number)) {
    fail(path, "expected finite number");
  }
  return number;
}

std::uint32_t parseUnsigned32(const Json& value,
                              const std::string_view path) {
  if (!value.is_number_unsigned()) {
    fail(path, "expected unsigned 32-bit integer");
  }
  const auto wideValue = value.get<std::uint64_t>();
  if (wideValue > std::numeric_limits<std::uint32_t>::max()) {
    fail(path, "expected unsigned 32-bit integer");
  }
  return static_cast<std::uint32_t>(wideValue);
}

std::uint64_t parseUnsigned64(const Json& value,
                              const std::string_view path) {
  if (!value.is_number_unsigned()) {
    fail(path, "expected unsigned 64-bit integer");
  }
  return value.get<std::uint64_t>();
}

std::uint64_t parseSeed(const Json& value) {
  return parseUnsigned64(value, "/seed");
}

dsp::SplitStrategyType parseSplitStrategy(
    const Json& value,
    const std::string_view path) {
  const auto name = parseString(value, path);
  if (name == "duplicate") {
    return dsp::SplitStrategyType::duplicate;
  }
  if (name == "stereo-halves") {
    return dsp::SplitStrategyType::stereoHalves;
  }
  if (name == "stereo-interleave") {
    return dsp::SplitStrategyType::stereoInterleave;
  }
  fail(path, "expected duplicate, stereo-halves, or stereo-interleave");
}

dsp::EnergyNormalisation parseNormalisation(
    const Json& value,
    const std::string_view path) {
  const auto name = parseString(value, path);
  if (name == "energy") {
    return dsp::EnergyNormalisation::energy;
  }
  if (name == "none") {
    return dsp::EnergyNormalisation::none;
  }
  fail(path, "expected energy or none");
}

dsp::DelayStrategy parseDelayStrategy(
    const Json& value,
    const std::string_view path) {
  const auto name = parseString(value, path);
  if (name == "segmented-random") {
    return dsp::DelayStrategy::segmentedRandom;
  }
  if (name == "even") {
    return dsp::DelayStrategy::even;
  }
  fail(path, "expected segmented-random or even");
}

dsp::MixMatrixType parseMix(
    const Json& value,
    const std::string_view path) {
  if (parseString(value, path) != "hadamard") {
    fail(path, "expected hadamard");
  }
  return dsp::MixMatrixType::hadamard;
}

dsp::PolarityStrategy parsePolarity(
    const Json& value,
    const std::string_view path) {
  const auto name = parseString(value, path);
  if (name == "seeded-random") {
    return dsp::PolarityStrategy::seededRandom;
  }
  if (name == "none") {
    return dsp::PolarityStrategy::none;
  }
  fail(path, "expected seeded-random or none");
}

dsp::DownmixStrategy parseDownmixStrategy(
    const Json& value,
    const std::string_view path) {
  if (parseString(value, path) != "select") {
    fail(path, "expected select");
  }
  return dsp::DownmixStrategy::select;
}

config::DiffusionDistribution parseDistribution(
    const Json& value,
    const std::string_view path) {
  const auto name = parseString(value, path);
  if (name == "even") {
    return config::DiffusionDistribution::even;
  }
  if (name == "doubling") {
    return config::DiffusionDistribution::doubling;
  }
  fail(path, "expected even or doubling");
}

config::SplitConfig parseRequestedSplit(
    const Json& value,
    const std::string_view path) {
  rejectUnknownFields(
      value, path, {"type", "channels", "strategy", "normalisation"});
  config::SplitConfig split;
  if (value.contains("channels")) {
    split.channels =
        parseUnsigned32(value.at("channels"), std::string(path) + "/channels");
  }
  if (value.contains("strategy")) {
    split.strategy =
        parseSplitStrategy(value.at("strategy"), std::string(path) + "/strategy");
  }
  if (value.contains("normalisation")) {
    split.normalisation = parseNormalisation(
        value.at("normalisation"), std::string(path) + "/normalisation");
  }
  return split;
}

config::DiffusionStepConfig parseRequestedStep(
    const Json& value,
    const std::string_view path) {
  requireObject(value, path);
  rejectUnknownFields(
      value, path, {"delayStrategy", "mix", "shuffle", "polarity"});
  config::DiffusionStepConfig step;
  if (value.contains("delayStrategy")) {
    step.delayStrategy = parseDelayStrategy(
        value.at("delayStrategy"), std::string(path) + "/delayStrategy");
  }
  if (value.contains("mix")) {
    step.mix = parseMix(value.at("mix"), std::string(path) + "/mix");
  }
  if (value.contains("shuffle")) {
    step.shuffle =
        parseBoolean(value.at("shuffle"), std::string(path) + "/shuffle");
  }
  if (value.contains("polarity")) {
    step.polarity = parsePolarity(
        value.at("polarity"), std::string(path) + "/polarity");
  }
  return step;
}

config::DiffuserConfig parseRequestedDiffuser(
    const Json& value,
    const std::string_view path) {
  rejectUnknownFields(
      value, path, {"type", "steps", "totalMs", "distribution", "step"});
  config::DiffuserConfig diffuser;
  if (value.contains("steps")) {
    diffuser.steps =
        parseUnsigned32(value.at("steps"), std::string(path) + "/steps");
  }
  if (value.contains("totalMs")) {
    diffuser.totalMs =
        parseNumber(value.at("totalMs"), std::string(path) + "/totalMs");
  }
  if (value.contains("distribution")) {
    diffuser.distribution = parseDistribution(
        value.at("distribution"), std::string(path) + "/distribution");
  }
  if (value.contains("step")) {
    diffuser.step =
        parseRequestedStep(value.at("step"), std::string(path) + "/step");
  }
  return diffuser;
}

config::DownmixConfig parseRequestedDownmix(
    const Json& value,
    const std::string_view path) {
  rejectUnknownFields(
      value, path, {"type", "strategy", "normalisation"});
  config::DownmixConfig downmix;
  if (value.contains("strategy")) {
    downmix.strategy = parseDownmixStrategy(
        value.at("strategy"), std::string(path) + "/strategy");
  }
  if (value.contains("normalisation")) {
    downmix.normalisation = parseNormalisation(
        value.at("normalisation"), std::string(path) + "/normalisation");
  }
  return downmix;
}

config::CompositionConfig parseRequestedComposition(const Json& value) {
  requireObject(value, "/composition");
  rejectUnknownFields(value, "/composition", {"stages"});

  config::CompositionConfig composition;
  if (!value.contains("stages")) {
    return composition;
  }

  composition.stagesSpecified = true;
  const auto& stages = value.at("stages");
  requireArray(stages, "/composition/stages");
  for (std::size_t index = 0; index < stages.size(); ++index) {
    const auto path = "/composition/stages/" + std::to_string(index);
    const auto& stage = stages.at(index);
    requireObject(stage, path);
    requireField(stage, "type", path);
    const auto type = parseString(stage.at("type"), path + "/type");
    if (type == "split") {
      composition.stages.emplace_back(parseRequestedSplit(stage, path));
    } else if (type == "diffuser") {
      composition.stages.emplace_back(parseRequestedDiffuser(stage, path));
    } else if (type == "downmix") {
      composition.stages.emplace_back(parseRequestedDownmix(stage, path));
    } else {
      fail(path + "/type", "expected split, diffuser, or downmix");
    }
  }
  return composition;
}

std::vector<std::uint64_t> parseUnsigned64Array(
    const Json& value,
    const std::string_view path) {
  requireArray(value, path);
  std::vector<std::uint64_t> result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    result.push_back(parseUnsigned64(
        value.at(index), std::string(path) + "/" + std::to_string(index)));
  }
  return result;
}

std::vector<std::uint32_t> parseUnsigned32Array(
    const Json& value,
    const std::string_view path) {
  requireArray(value, path);
  std::vector<std::uint32_t> result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    result.push_back(parseUnsigned32(
        value.at(index), std::string(path) + "/" + std::to_string(index)));
  }
  return result;
}

std::vector<double> parseNumberArray(
    const Json& value,
    const std::string_view path) {
  requireArray(value, path);
  std::vector<double> result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    result.push_back(parseNumber(
        value.at(index), std::string(path) + "/" + std::to_string(index)));
  }
  return result;
}

std::vector<int> parseSignArray(
    const Json& value,
    const std::string_view path) {
  requireArray(value, path);
  std::vector<int> result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    const auto childPath =
        std::string(path) + "/" + std::to_string(index);
    if (!value.at(index).is_number_integer()) {
      fail(childPath, "expected -1 or 1");
    }
    const auto sign = value.at(index).get<int>();
    if (sign != -1 && sign != 1) {
      fail(childPath, "expected -1 or 1");
    }
    result.push_back(sign);
  }
  return result;
}

dsp::ResolvedSplit parseResolvedSplit(
    const Json& value,
    const std::string_view path) {
  rejectUnknownFields(
      value,
      path,
      {"type",
       "inputChannels",
       "channels",
       "strategy",
       "normalisation",
       "sourceGain",
       "channelGain"});
  for (const auto field :
       {"inputChannels",
        "channels",
        "strategy",
        "normalisation",
        "sourceGain",
        "channelGain"}) {
    requireField(value, field, path);
  }
  return {
      parseUnsigned32(
          value.at("inputChannels"), std::string(path) + "/inputChannels"),
      parseUnsigned32(value.at("channels"), std::string(path) + "/channels"),
      parseSplitStrategy(
          value.at("strategy"), std::string(path) + "/strategy"),
      parseNormalisation(
          value.at("normalisation"), std::string(path) + "/normalisation"),
      parseNumber(value.at("sourceGain"), std::string(path) + "/sourceGain"),
      parseNumber(value.at("channelGain"), std::string(path) + "/channelGain"),
  };
}

dsp::ResolvedDiffusionStep parseResolvedStep(
    const Json& value,
    const std::string_view path) {
  requireObject(value, path);
  rejectUnknownFields(
      value,
      path,
      {"index",
       "lengthSamples",
       "lengthMs",
       "delayStrategy",
       "delaysSamples",
       "delaysMs",
       "bufferSizes",
       "shuffle",
       "permutation",
       "polarity",
       "polaritySigns",
       "mix",
       "matrix"});
  for (const auto field :
       {"index",
        "lengthSamples",
        "lengthMs",
        "delayStrategy",
        "delaysSamples",
        "delaysMs",
        "bufferSizes",
        "shuffle",
        "permutation",
        "polarity",
        "polaritySigns",
        "mix",
        "matrix"}) {
    requireField(value, field, path);
  }

  std::vector<double> matrix;
  const auto matrixPath = std::string(path) + "/matrix";
  const auto& rows = value.at("matrix");
  requireArray(rows, matrixPath);
  for (std::size_t row = 0; row < rows.size(); ++row) {
    auto values = parseNumberArray(
        rows.at(row), matrixPath + "/" + std::to_string(row));
    matrix.insert(matrix.end(), values.begin(), values.end());
  }

  return {
      parseUnsigned32(value.at("index"), std::string(path) + "/index"),
      parseUnsigned64(
          value.at("lengthSamples"), std::string(path) + "/lengthSamples"),
      parseNumber(value.at("lengthMs"), std::string(path) + "/lengthMs"),
      parseDelayStrategy(
          value.at("delayStrategy"), std::string(path) + "/delayStrategy"),
      parseUnsigned64Array(
          value.at("delaysSamples"), std::string(path) + "/delaysSamples"),
      parseNumberArray(
          value.at("delaysMs"), std::string(path) + "/delaysMs"),
      parseUnsigned64Array(
          value.at("bufferSizes"), std::string(path) + "/bufferSizes"),
      parseBoolean(value.at("shuffle"), std::string(path) + "/shuffle"),
      parseUnsigned32Array(
          value.at("permutation"), std::string(path) + "/permutation"),
      parsePolarity(
          value.at("polarity"), std::string(path) + "/polarity"),
      parseSignArray(
          value.at("polaritySigns"), std::string(path) + "/polaritySigns"),
      parseMix(value.at("mix"), std::string(path) + "/mix"),
      std::move(matrix),
  };
}

dsp::ResolvedDiffuser parseResolvedDiffuser(
    const Json& value,
    const std::string_view path) {
  rejectUnknownFields(value, path, {"type", "totalSamples", "steps"});
  requireField(value, "totalSamples", path);
  requireField(value, "steps", path);
  const auto stepsPath = std::string(path) + "/steps";
  const auto& steps = value.at("steps");
  requireArray(steps, stepsPath);
  dsp::ResolvedDiffuser diffuser;
  diffuser.totalSamples =
      parseUnsigned64(value.at("totalSamples"), std::string(path) + "/totalSamples");
  diffuser.steps.reserve(steps.size());
  for (std::size_t index = 0; index < steps.size(); ++index) {
    diffuser.steps.push_back(parseResolvedStep(
        steps.at(index), stepsPath + "/" + std::to_string(index)));
  }
  return diffuser;
}

dsp::ResolvedDownmix parseResolvedDownmix(
    const Json& value,
    const std::string_view path) {
  rejectUnknownFields(
      value,
      path,
      {"type",
       "inputChannels",
       "outputChannels",
       "strategy",
       "normalisation",
       "compensation"});
  for (const auto field :
       {"inputChannels",
        "outputChannels",
        "strategy",
        "normalisation",
        "compensation"}) {
    requireField(value, field, path);
  }
  return {
      parseUnsigned32(
          value.at("inputChannels"), std::string(path) + "/inputChannels"),
      parseUnsigned32(
          value.at("outputChannels"), std::string(path) + "/outputChannels"),
      parseDownmixStrategy(
          value.at("strategy"), std::string(path) + "/strategy"),
      parseNormalisation(
          value.at("normalisation"), std::string(path) + "/normalisation"),
      parseNumber(
          value.at("compensation"), std::string(path) + "/compensation"),
  };
}

dsp::ResolvedComposition parseResolvedComposition(const Json& value) {
  requireObject(value, "/composition");
  rejectUnknownFields(value, "/composition", {"stages"});
  requireField(value, "stages", "/composition");
  const auto& stages = value.at("stages");
  requireArray(stages, "/composition/stages");

  dsp::ResolvedComposition composition;
  for (std::size_t index = 0; index < stages.size(); ++index) {
    const auto path = "/composition/stages/" + std::to_string(index);
    const auto& stage = stages.at(index);
    requireObject(stage, path);
    requireField(stage, "type", path);
    const auto type = parseString(stage.at("type"), path + "/type");
    if (type == "split") {
      composition.stages.emplace_back(parseResolvedSplit(stage, path));
    } else if (type == "diffuser") {
      composition.stages.emplace_back(parseResolvedDiffuser(stage, path));
    } else if (type == "downmix") {
      composition.stages.emplace_back(parseResolvedDownmix(stage, path));
    } else {
      fail(path + "/type", "expected split, diffuser, or downmix");
    }
  }
  return composition;
}

} // namespace

config::ReverbConfig parseRequestedConfig(const std::string_view contents) {
  const auto json = parseJson(contents);
  requireObject(json, "/");
  rejectUnknownFields(
      json, "/", {"formatVersion", "seed", "composition"});

  config::ReverbConfig requested;
  if (json.contains("formatVersion")) {
    requested.formatVersion =
        parseUnsigned32(json.at("formatVersion"), "/formatVersion");
  }
  if (json.contains("seed")) {
    requested.seed = parseSeed(json.at("seed"));
  }
  if (json.contains("composition")) {
    requested.composition =
        parseRequestedComposition(json.at("composition"));
  }
  return requested;
}

dsp::ResolvedConfig parseResolvedConfig(const std::string_view contents) {
  const auto json = parseJson(contents);
  requireObject(json, "/");
  rejectUnknownFields(
      json,
      "/",
      {"formatVersion", "seed", "sampleRate", "composition"});

  for (const auto field :
       {"formatVersion", "seed", "sampleRate", "composition"}) {
    requireField(json, field, "");
  }

  const dsp::ResolvedConfig resolved{
      parseUnsigned32(json.at("formatVersion"), "/formatVersion"),
      parseSeed(json.at("seed")),
      parseUnsigned32(json.at("sampleRate"), "/sampleRate"),
      parseResolvedComposition(json.at("composition")),
  };
  config::validateResolvedConfig(resolved);
  return resolved;
}

} // namespace rvrbotron::cli
