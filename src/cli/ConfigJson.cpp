#include "rvrbotron/cli/ConfigJson.h"

#include "rvrbotron/HarnessError.h"
#include "rvrbotron/config/ResolveConfig.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
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

std::optional<double> parseOptionalNumber(
    const Json& value,
    const std::string_view path) {
  if (value.is_null()) {
    return std::nullopt;
  }
  return parseNumber(value, path);
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
  if (name == "uniform-random") {
    return dsp::DelayStrategy::uniformRandom;
  }
  if (name == "even") {
    return dsp::DelayStrategy::even;
  }
  fail(path, "expected segmented-random, uniform-random, or even");
}

dsp::MixMatrixType parseMix(
    const Json& value,
    const std::string_view path) {
  const auto name = parseString(value, path);
  if (name == "hadamard") {
    return dsp::MixMatrixType::hadamard;
  }
  if (name == "householder") {
    return dsp::MixMatrixType::householder;
  }
  if (name == "random-orthogonal") {
    return dsp::MixMatrixType::randomOrthogonal;
  }
  fail(path, "expected hadamard, householder, or random-orthogonal");
}

dsp::ModulationInterpolation parseModulationInterpolation(
    const Json& value,
    const std::string_view path) {
  const auto name = parseString(value, path);
  if (name == "lagrange3") {
    return dsp::ModulationInterpolation::lagrange3;
  }
  if (name == "linear") {
    return dsp::ModulationInterpolation::linear;
  }
  if (name == "allpass") {
    return dsp::ModulationInterpolation::allpass;
  }
  fail(path, "expected lagrange3, linear, or allpass");
}

dsp::ModulationShape parseModulationShape(
    const Json& value,
    const std::string_view path) {
  const auto name = parseString(value, path);
  if (name == "smoothed-random") {
    return dsp::ModulationShape::smoothedRandom;
  }
  if (name == "sine") {
    return dsp::ModulationShape::sine;
  }
  if (name == "triangle") {
    return dsp::ModulationShape::triangle;
  }
  fail(path, "expected smoothed-random, sine, or triangle");
}

config::ModulationConfig parseRequestedModulation(
    const Json& value,
    const std::string_view path) {
  requireObject(value, path);
  rejectUnknownFields(
      value,
      path,
      {"depthMs", "rateHz", "shape", "channelFraction", "interpolation"});
  config::ModulationConfig modulation;
  if (value.contains("depthMs")) {
    modulation.depthMs =
        parseNumber(value.at("depthMs"), std::string(path) + "/depthMs");
  }
  if (value.contains("rateHz")) {
    modulation.rateHz =
        parseNumber(value.at("rateHz"), std::string(path) + "/rateHz");
  }
  if (value.contains("shape")) {
    modulation.shape = parseModulationShape(
        value.at("shape"), std::string(path) + "/shape");
  }
  if (value.contains("channelFraction")) {
    modulation.channelFraction = parseNumber(
        value.at("channelFraction"), std::string(path) + "/channelFraction");
  }
  if (value.contains("interpolation")) {
    modulation.interpolation = parseModulationInterpolation(
        value.at("interpolation"), std::string(path) + "/interpolation");
  }
  return modulation;
}

dsp::GainMode parseGainMode(
    const Json& value,
    const std::string_view path) {
  const auto name = parseString(value, path);
  if (name == "per-channel") {
    return dsp::GainMode::perChannel;
  }
  if (name == "uniform") {
    return dsp::GainMode::uniform;
  }
  fail(path, "expected per-channel or uniform");
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
  const auto name = parseString(value, path);
  if (name == "select") {
    return dsp::DownmixStrategy::select;
  }
  if (name == "orthogonal-rows") {
    return dsp::DownmixStrategy::orthogonalRows;
  }
  if (name == "halves") {
    return dsp::DownmixStrategy::halves;
  }
  if (name == "alternating") {
    return dsp::DownmixStrategy::alternating;
  }
  fail(path, "expected select, orthogonal-rows, halves, or alternating");
}

// Named for error messages naming the actual requested strategy (issue
// #110) rather than a stale strategy name hardcoded from when `select`
// had only one alternative.
const char* downmixStrategyLabel(const dsp::DownmixStrategy strategy) {
  switch (strategy) {
  case dsp::DownmixStrategy::select:
    return "select";
  case dsp::DownmixStrategy::orthogonalRows:
    return "orthogonal-rows";
  case dsp::DownmixStrategy::halves:
    return "halves";
  case dsp::DownmixStrategy::alternating:
    return "alternating";
  }
  fail("/composition", "unsupported Downmix strategy");
}

dsp::DownmixAlignment parseDownmixAlignment(
    const Json& value,
    const std::string_view path) {
  const auto name = parseString(value, path);
  if (name == "aligned") {
    return dsp::DownmixAlignment::aligned;
  }
  if (name == "unaligned") {
    return dsp::DownmixAlignment::unaligned;
  }
  fail(path, "expected aligned or unaligned");
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

config::DiffusionStepConfig parseStepFields(
    const Json& value,
    const std::string_view path) {
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
  if (value.contains("modulation")) {
    step.modulation = parseRequestedModulation(
        value.at("modulation"), std::string(path) + "/modulation");
  }
  return step;
}

config::DiffusionStepConfig parseRequestedStep(
    const Json& value,
    const std::string_view path) {
  requireObject(value, path);
  rejectUnknownFields(
      value,
      path,
      {"delayStrategy", "mix", "shuffle", "polarity", "modulation"});
  return parseStepFields(value, path);
}

config::DiffusionStepOverride parseRequestedStepOverride(
    const Json& value,
    const std::string_view path) {
  requireObject(value, path);
  rejectUnknownFields(
      value,
      path,
      {"index",
       "delayStrategy",
       "mix",
       "shuffle",
       "polarity",
       "modulation"});
  requireField(value, "index", path);
  config::DiffusionStepOverride stepOverride;
  stepOverride.index =
      parseUnsigned32(value.at("index"), std::string(path) + "/index");
  stepOverride.step = parseStepFields(value, path);
  return stepOverride;
}

config::DiffuserConfig parseRequestedDiffuser(
    const Json& value,
    const std::string_view path) {
  rejectUnknownFields(
      value,
      path,
      {"type",
       "steps",
       "totalMs",
       "distribution",
       "lengthsMs",
       "step",
       "stepOverrides"});
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
  if (value.contains("lengthsMs")) {
    if (diffuser.steps.has_value() || diffuser.totalMs.has_value() ||
        diffuser.distribution.has_value()) {
      fail(
          std::string(path) + "/lengthsMs",
          "expected exactly one of lengthsMs or steps/totalMs/distribution");
    }
    diffuser.lengthsMs = parseNumberArray(
        value.at("lengthsMs"), std::string(path) + "/lengthsMs");
    if (diffuser.lengthsMs->empty()) {
      fail(
          std::string(path) + "/lengthsMs",
          "expected at least one step length");
    }
    for (std::size_t index = 0; index < diffuser.lengthsMs->size(); ++index) {
      if (!((*diffuser.lengthsMs)[index] > 0.0)) {
        fail(
            std::string(path) + "/lengthsMs/" + std::to_string(index),
            "expected value greater than zero");
      }
    }
  }
  if (value.contains("step")) {
    diffuser.step =
        parseRequestedStep(value.at("step"), std::string(path) + "/step");
  }
  if (value.contains("stepOverrides")) {
    const auto& overrides = value.at("stepOverrides");
    const auto overridesPath = std::string(path) + "/stepOverrides";
    requireArray(overrides, overridesPath);
    const auto stepCount =
        diffuser.lengthsMs.has_value()
            ? static_cast<std::uint32_t>(diffuser.lengthsMs->size())
            : diffuser.steps.value_or(config::kDefaultDiffuserStepCount);
    std::vector<config::DiffusionStepOverride> parsedOverrides;
    parsedOverrides.reserve(overrides.size());
    std::vector<bool> seenIndex(stepCount, false);
    for (std::size_t index = 0; index < overrides.size(); ++index) {
      const auto entryPath = overridesPath + "/" + std::to_string(index);
      auto stepOverride =
          parseRequestedStepOverride(overrides.at(index), entryPath);
      if (stepOverride.index >= stepCount) {
        fail(
            entryPath + "/index",
            "expected index less than the resolved step count");
      }
      if (seenIndex[stepOverride.index]) {
        fail(entryPath + "/index", "expected distinct step indices");
      }
      seenIndex[stepOverride.index] = true;
      parsedOverrides.push_back(std::move(stepOverride));
    }
    diffuser.stepOverrides = std::move(parsedOverrides);
  }
  return diffuser;
}

config::DampingConfig parseRequestedDamping(
    const Json& value,
    const std::string_view path) {
  requireObject(value, path);
  rejectUnknownFields(
      value, path, {"highRatio", "highHz", "lowRatio", "lowHz"});
  config::DampingConfig damping;
  if (value.contains("highRatio")) {
    damping.highRatio = parseNumber(
        value.at("highRatio"), std::string(path) + "/highRatio");
  }
  if (value.contains("highHz")) {
    damping.highHz =
        parseNumber(value.at("highHz"), std::string(path) + "/highHz");
  }
  if (value.contains("lowRatio")) {
    damping.lowRatio =
        parseNumber(value.at("lowRatio"), std::string(path) + "/lowRatio");
  }
  if (value.contains("lowHz")) {
    damping.lowHz =
        parseNumber(value.at("lowHz"), std::string(path) + "/lowHz");
  }
  return damping;
}

config::FeedbackLoopConfig parseRequestedFeedbackLoop(
    const Json& value,
    const std::string_view path) {
  rejectUnknownFields(
      value,
      path,
      {"type",
       "delayMinMs",
       "delayMaxMs",
       "delayStrategy",
       "rt60Sec",
       "decayMargin",
       "mix",
       "gainMode",
       "silenceFloorDb",
       "damping",
       "modulation"});
  config::FeedbackLoopConfig loop;
  if (value.contains("delayMinMs")) {
    loop.delayMinMs = parseNumber(
        value.at("delayMinMs"), std::string(path) + "/delayMinMs");
  }
  if (value.contains("delayMaxMs")) {
    loop.delayMaxMs = parseNumber(
        value.at("delayMaxMs"), std::string(path) + "/delayMaxMs");
  }
  if (value.contains("delayStrategy")) {
    loop.delayStrategy = parseDelayStrategy(
        value.at("delayStrategy"), std::string(path) + "/delayStrategy");
  }
  if (value.contains("rt60Sec")) {
    loop.rt60Sec =
        parseNumber(value.at("rt60Sec"), std::string(path) + "/rt60Sec");
  }
  if (value.contains("decayMargin")) {
    loop.decayMargin = parseNumber(
        value.at("decayMargin"), std::string(path) + "/decayMargin");
  }
  if (value.contains("mix")) {
    loop.mix = parseMix(value.at("mix"), std::string(path) + "/mix");
  }
  if (value.contains("gainMode")) {
    loop.gainMode = parseGainMode(
        value.at("gainMode"), std::string(path) + "/gainMode");
  }
  if (value.contains("silenceFloorDb") &&
      !value.at("silenceFloorDb").is_null()) {
    loop.silenceFloorDb = parseNumber(
        value.at("silenceFloorDb"), std::string(path) + "/silenceFloorDb");
  }
  if (value.contains("damping")) {
    loop.damping = parseRequestedDamping(
        value.at("damping"), std::string(path) + "/damping");
  }
  if (value.contains("modulation")) {
    loop.modulation = parseRequestedModulation(
        value.at("modulation"), std::string(path) + "/modulation");
  }
  return loop;
}

// `select`'s own leftChannel has no implicit default for the Main Downmix
// (issue #107), so omitting it there is rejected right at this JSON
// boundary. The Early Downmix instead defaults `select` to Channels 0/1
// (issue #111, docs/design/reverb/stages/09-composition.md), so its own
// caller passes `requireExplicitSelectChannel` false here and leaves
// resolveConfig's own withEarlyDownmixDefaults to fill the omission.
config::DownmixConfig parseRequestedDownmix(
    const Json& value,
    const std::string_view path,
    const bool requireExplicitSelectChannel = true) {
  rejectUnknownFields(
      value,
      path,
      {"type",
       "strategy",
       "leftChannel",
       "rightChannel",
       "normalisation",
       "widthDeg"});
  config::DownmixConfig downmix;
  if (value.contains("strategy")) {
    downmix.strategy = parseDownmixStrategy(
        value.at("strategy"), std::string(path) + "/strategy");
  }
  if (downmix.strategy.value_or(dsp::DownmixStrategy::select) ==
      dsp::DownmixStrategy::select) {
    if (requireExplicitSelectChannel) {
      requireField(value, "leftChannel", path);
    }
    if (value.contains("leftChannel")) {
      downmix.leftChannel = parseUnsigned32(
          value.at("leftChannel"), std::string(path) + "/leftChannel");
    }
    if (value.contains("rightChannel")) {
      downmix.rightChannel = parseUnsigned32(
          value.at("rightChannel"), std::string(path) + "/rightChannel");
    }
  } else {
    const auto strategyLabel = downmixStrategyLabel(*downmix.strategy);
    if (value.contains("leftChannel")) {
      fail(
          std::string(path) + "/leftChannel",
          std::string("not applicable to strategy ") + strategyLabel);
    }
    if (value.contains("rightChannel")) {
      fail(
          std::string(path) + "/rightChannel",
          std::string("not applicable to strategy ") + strategyLabel);
    }
  }
  if (value.contains("normalisation")) {
    downmix.normalisation = parseNormalisation(
        value.at("normalisation"), std::string(path) + "/normalisation");
  }
  if (value.contains("widthDeg")) {
    downmix.widthDeg = parseNumber(
        value.at("widthDeg"), std::string(path) + "/widthDeg");
  }
  return downmix;
}

config::EarlyTapConfig parseRequestedEarlyTap(
    const Json& value, const std::string_view path) {
  requireObject(value, path);
  rejectUnknownFields(value, path, {"stepIndex", "gainDb"});
  requireField(value, "stepIndex", path);
  config::EarlyTapConfig tap;
  tap.stepIndex = parseUnsigned32(
      value.at("stepIndex"), std::string(path) + "/stepIndex");
  if (value.contains("gainDb")) {
    tap.gainDb =
        parseNumber(value.at("gainDb"), std::string(path) + "/gainDb");
  }
  return tap;
}

// The parallel Early Reflections branch (issue #111): `taps` is required
// -- an entirely empty `early: {}` request, with no taps key at all,
// still means no branch (see parseRequestedComposition), but a present
// `early` object with an omitted `taps` key would otherwise be
// indistinguishable from one whose taps are simply empty, so this
// requires the key explicitly rather than defaulting it.
// Early's own `downmix` (issue #111) is parsed by the same
// parseRequestedDownmix/parseResolvedDownmix used for the `downmix` stage
// in composition.stages, but -- unlike that stage array -- it never
// passes through the dispatcher in parseRequestedComposition/
// parseResolvedComposition that requires and checks a stage's own `type`
// before ever calling into either parser. Skipping this check would let
// an Early `downmix` whose `type` names a different stage entirely (or,
// on replay, omits `type` where a Main Downmix's own serialized form
// never would) be silently accepted as a Downmix regardless of what it
// claims to be. `requireType` mirrors the stage dispatcher's own
// `requireField(stage, "type", path)` for replay, where resolved.json
// always serializes `type` (see ResolvedConfigJson.cpp's downmixJson);
// the requested JSON schema has no such guarantee -- Early's own
// `downmix` object in a request never carries `type` at all in the
// documented examples -- so a requested `downmix` only has its `type`
// checked when present, not required.
void checkNestedDownmixType(
    const Json& value, const std::string_view path, const bool requireType) {
  requireObject(value, path);
  if (requireType) {
    requireField(value, "type", path);
  }
  if (!value.contains("type")) {
    return;
  }
  const auto type = parseString(value.at("type"), std::string(path) + "/type");
  if (type != "downmix") {
    fail(std::string(path) + "/type", "expected downmix");
  }
}

config::EarlyConfig parseRequestedEarly(
    const Json& value, const std::string_view path) {
  requireObject(value, path);
  rejectUnknownFields(
      value,
      path,
      {"enabled", "levelDb", "decayDbPerSec", "taps", "downmix"});
  config::EarlyConfig early;
  if (value.contains("enabled")) {
    early.enabled =
        parseBoolean(value.at("enabled"), std::string(path) + "/enabled");
  }
  if (value.contains("levelDb")) {
    early.levelDb =
        parseNumber(value.at("levelDb"), std::string(path) + "/levelDb");
  }
  if (value.contains("decayDbPerSec")) {
    early.decayDbPerSec = parseNumber(
        value.at("decayDbPerSec"), std::string(path) + "/decayDbPerSec");
  }
  if (value.contains("taps")) {
    const auto& taps = value.at("taps");
    const auto tapsPath = std::string(path) + "/taps";
    requireArray(taps, tapsPath);
    std::vector<config::EarlyTapConfig> parsedTaps;
    parsedTaps.reserve(taps.size());
    for (std::size_t index = 0; index < taps.size(); ++index) {
      parsedTaps.push_back(
          parseRequestedEarlyTap(
              taps.at(index), tapsPath + "/" + std::to_string(index)));
    }
    early.taps = std::move(parsedTaps);
  }
  if (value.contains("downmix")) {
    const auto downmixPath = std::string(path) + "/downmix";
    checkNestedDownmixType(
        value.at("downmix"), downmixPath, /*requireType=*/false);
    early.downmix = parseRequestedDownmix(
        value.at("downmix"), downmixPath, /*requireExplicitSelectChannel=*/false);
  }
  return early;
}

config::CompositionConfig parseRequestedComposition(const Json& value) {
  requireObject(value, "/composition");
  rejectUnknownFields(
      value,
      "/composition",
      {"stages", "mainEnabled", "mainLevelDb", "early"});

  config::CompositionConfig composition;
  if (value.contains("stages")) {
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
        composition.stages.emplace_back(
            parseRequestedDiffuser(stage, path));
      } else if (type == "feedback-loop") {
        composition.stages.emplace_back(
            parseRequestedFeedbackLoop(stage, path));
      } else if (type == "downmix") {
        composition.stages.emplace_back(
            parseRequestedDownmix(stage, path));
      } else {
        fail(
            path + "/type",
            "expected split, diffuser, feedback-loop, or downmix");
      }
    }
  }

  // mainEnabled/mainLevelDb control the serial Main wet path (issue
  // #109): moot, and rejected, on the empty identity Composition, since
  // there is no branch for them to affect (docs/design/reverb/stages/
  // 09-composition.md's "Branch controls are invalid on the empty
  // identity Composition").
  if (composition.stages.empty()) {
    if (value.contains("mainEnabled")) {
      fail(
          "/composition/mainEnabled",
          "not applicable to the empty identity Composition");
    }
    if (value.contains("mainLevelDb")) {
      fail(
          "/composition/mainLevelDb",
          "not applicable to the empty identity Composition");
    }
    if (value.contains("early")) {
      fail(
          "/composition/early",
          "not applicable to the empty identity Composition");
    }
    return composition;
  }

  if (value.contains("mainEnabled")) {
    composition.mainEnabled = parseBoolean(
        value.at("mainEnabled"), "/composition/mainEnabled");
  }
  if (value.contains("mainLevelDb")) {
    composition.mainLevelDb = parseNumber(
        value.at("mainLevelDb"), "/composition/mainLevelDb");
  }
  if (value.contains("early")) {
    composition.early =
        parseRequestedEarly(value.at("early"), "/composition/early");
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

std::vector<bool> parseBooleanArray(
    const Json& value,
    const std::string_view path) {
  requireArray(value, path);
  std::vector<bool> result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    result.push_back(parseBoolean(
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

dsp::ResolvedModulation parseResolvedModulation(
    const Json& value,
    const std::string_view path) {
  requireObject(value, path);
  rejectUnknownFields(
      value,
      path,
      {"depthMs",
       "rateHz",
       "shape",
       "channelFraction",
       "interpolation",
       "excursionSamples",
       "interpolationMarginSamples",
       "channelSeeds",
       "channelTargetsPerSample",
       "channelPhases",
       "channelModulated"});
  for (const auto field :
       {"depthMs",
        "rateHz",
        "shape",
        "channelFraction",
        "interpolation",
        "excursionSamples",
        "interpolationMarginSamples",
        "channelSeeds",
        "channelTargetsPerSample",
        "channelPhases",
        "channelModulated"}) {
    requireField(value, field, path);
  }
  dsp::ResolvedModulation modulation;
  modulation.depthMs =
      parseNumber(value.at("depthMs"), std::string(path) + "/depthMs");
  modulation.rateHz =
      parseNumber(value.at("rateHz"), std::string(path) + "/rateHz");
  modulation.shape = parseModulationShape(
      value.at("shape"), std::string(path) + "/shape");
  modulation.channelFraction = parseNumber(
      value.at("channelFraction"), std::string(path) + "/channelFraction");
  modulation.interpolation = parseModulationInterpolation(
      value.at("interpolation"), std::string(path) + "/interpolation");
  modulation.excursionSamples = parseNumber(
      value.at("excursionSamples"), std::string(path) + "/excursionSamples");
  modulation.interpolationMarginSamples = parseUnsigned64(
      value.at("interpolationMarginSamples"),
      std::string(path) + "/interpolationMarginSamples");
  modulation.channelSeeds = parseUnsigned64Array(
      value.at("channelSeeds"), std::string(path) + "/channelSeeds");
  modulation.channelTargetsPerSample = parseNumberArray(
      value.at("channelTargetsPerSample"),
      std::string(path) + "/channelTargetsPerSample");
  modulation.channelPhases = parseNumberArray(
      value.at("channelPhases"), std::string(path) + "/channelPhases");
  modulation.channelModulated = parseBooleanArray(
      value.at("channelModulated"), std::string(path) + "/channelModulated");
  return modulation;
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
       "matrix",
       "modulation"});
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

  dsp::ResolvedDiffusionStep step;
  step.index = parseUnsigned32(value.at("index"), std::string(path) + "/index");
  step.lengthSamples = parseUnsigned64(
      value.at("lengthSamples"), std::string(path) + "/lengthSamples");
  step.lengthMs =
      parseNumber(value.at("lengthMs"), std::string(path) + "/lengthMs");
  step.delayStrategy = parseDelayStrategy(
      value.at("delayStrategy"), std::string(path) + "/delayStrategy");
  step.delaysSamples = parseUnsigned64Array(
      value.at("delaysSamples"), std::string(path) + "/delaysSamples");
  step.delaysMs = parseNumberArray(
      value.at("delaysMs"), std::string(path) + "/delaysMs");
  step.bufferSizes = parseUnsigned64Array(
      value.at("bufferSizes"), std::string(path) + "/bufferSizes");
  step.shuffle =
      parseBoolean(value.at("shuffle"), std::string(path) + "/shuffle");
  step.permutation = parseUnsigned32Array(
      value.at("permutation"), std::string(path) + "/permutation");
  step.polarity =
      parsePolarity(value.at("polarity"), std::string(path) + "/polarity");
  step.polaritySigns = parseSignArray(
      value.at("polaritySigns"), std::string(path) + "/polaritySigns");
  step.mix = parseMix(value.at("mix"), std::string(path) + "/mix");
  step.matrix = std::move(matrix);
  // Omitted entirely (rather than emitted as null) when disabled, so a
  // resolved.json written before Diffusion Step Modulation existed
  // remains loadable, and loads back as disabled (see issue #91).
  if (value.contains("modulation")) {
    step.modulation = parseResolvedModulation(
        value.at("modulation"), std::string(path) + "/modulation");
  }
  return step;
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

dsp::ResolvedDamping parseResolvedDamping(
    const Json& value,
    const std::string_view path) {
  requireObject(value, path);
  rejectUnknownFields(
      value,
      path,
      {"highRatio",
       "highHz",
       "lowRatio",
       "lowHz",
       "highShelfGains",
       "highShelfB0",
       "highShelfB1",
       "highShelfA1",
       "lowShelfGains",
       "lowShelfB0",
       "lowShelfB1",
       "lowShelfA1",
       "expectedLowRt60Sec",
       "expectedReferenceRt60Sec",
       "expectedHighRt60Sec",
       "contractionBoundFloat32",
       "contractionMarginFloat32",
       "contractionBoundFloat64",
       "contractionMarginFloat64",
       "slowestResolvedRt60Sec"});
  for (const auto field :
       {"highRatio",
        "highHz",
        "lowRatio",
        "lowHz",
        "highShelfGains",
        "highShelfB0",
        "highShelfB1",
        "highShelfA1",
        "lowShelfGains",
        "lowShelfB0",
        "lowShelfB1",
        "lowShelfA1",
        "expectedLowRt60Sec",
        "expectedReferenceRt60Sec",
        "expectedHighRt60Sec",
        "contractionBoundFloat32",
        "contractionMarginFloat32",
        "contractionBoundFloat64",
        "contractionMarginFloat64",
        "slowestResolvedRt60Sec"}) {
    requireField(value, field, path);
  }
  dsp::ResolvedDamping damping;
  damping.highRatio =
      parseNumber(value.at("highRatio"), std::string(path) + "/highRatio");
  damping.highHz =
      parseNumber(value.at("highHz"), std::string(path) + "/highHz");
  damping.lowRatio =
      parseNumber(value.at("lowRatio"), std::string(path) + "/lowRatio");
  damping.lowHz =
      parseNumber(value.at("lowHz"), std::string(path) + "/lowHz");
  damping.highShelfGains = parseNumberArray(
      value.at("highShelfGains"), std::string(path) + "/highShelfGains");
  damping.highShelfB0 = parseNumberArray(
      value.at("highShelfB0"), std::string(path) + "/highShelfB0");
  damping.highShelfB1 = parseNumberArray(
      value.at("highShelfB1"), std::string(path) + "/highShelfB1");
  damping.highShelfA1 = parseNumberArray(
      value.at("highShelfA1"), std::string(path) + "/highShelfA1");
  damping.lowShelfGains = parseNumberArray(
      value.at("lowShelfGains"), std::string(path) + "/lowShelfGains");
  damping.lowShelfB0 = parseNumberArray(
      value.at("lowShelfB0"), std::string(path) + "/lowShelfB0");
  damping.lowShelfB1 = parseNumberArray(
      value.at("lowShelfB1"), std::string(path) + "/lowShelfB1");
  damping.lowShelfA1 = parseNumberArray(
      value.at("lowShelfA1"), std::string(path) + "/lowShelfA1");
  damping.expectedLowRt60Sec = parseNumberArray(
      value.at("expectedLowRt60Sec"),
      std::string(path) + "/expectedLowRt60Sec");
  damping.expectedReferenceRt60Sec = parseNumberArray(
      value.at("expectedReferenceRt60Sec"),
      std::string(path) + "/expectedReferenceRt60Sec");
  damping.expectedHighRt60Sec = parseNumberArray(
      value.at("expectedHighRt60Sec"),
      std::string(path) + "/expectedHighRt60Sec");
  damping.contractionBoundFloat32 = parseNumberArray(
      value.at("contractionBoundFloat32"),
      std::string(path) + "/contractionBoundFloat32");
  damping.contractionMarginFloat32 = parseNumberArray(
      value.at("contractionMarginFloat32"),
      std::string(path) + "/contractionMarginFloat32");
  damping.contractionBoundFloat64 = parseNumberArray(
      value.at("contractionBoundFloat64"),
      std::string(path) + "/contractionBoundFloat64");
  damping.contractionMarginFloat64 = parseNumberArray(
      value.at("contractionMarginFloat64"),
      std::string(path) + "/contractionMarginFloat64");
  damping.slowestResolvedRt60Sec = parseNumber(
      value.at("slowestResolvedRt60Sec"),
      std::string(path) + "/slowestResolvedRt60Sec");
  return damping;
}

dsp::ResolvedFeedbackLoop parseResolvedFeedbackLoop(
    const Json& value,
    const std::string_view path) {
  rejectUnknownFields(
      value,
      path,
      {"type",
       "channels",
       "delayMinSamples",
       "delayMaxSamples",
       "delayMinMs",
       "delayMaxMs",
       "delayStrategy",
       "delaysSamples",
       "delaysMs",
       "bufferSizes",
       "rt60Sec",
       "gainMode",
       "gains",
       "mix",
       "matrix",
       "decayMargin",
       "tailBudgetSamples",
       "blockSizeBoundSamples",
       "silenceFloorDb",
       "damping",
       "modulation"});
  for (const auto field :
       {"channels",
        "delayMinSamples",
        "delayMaxSamples",
        "delayMinMs",
        "delayMaxMs",
        "delayStrategy",
        "delaysSamples",
        "delaysMs",
        "bufferSizes",
        "rt60Sec",
        "gainMode",
        "gains",
        "mix",
        "matrix",
        "decayMargin",
        "tailBudgetSamples",
        "blockSizeBoundSamples",
        "silenceFloorDb"}) {
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

  dsp::ResolvedFeedbackLoop loop;
  loop.channels =
      parseUnsigned32(value.at("channels"), std::string(path) + "/channels");
  loop.delayMinSamples = parseUnsigned64(
      value.at("delayMinSamples"), std::string(path) + "/delayMinSamples");
  loop.delayMaxSamples = parseUnsigned64(
      value.at("delayMaxSamples"), std::string(path) + "/delayMaxSamples");
  loop.delayMinMs = parseNumber(
      value.at("delayMinMs"), std::string(path) + "/delayMinMs");
  loop.delayMaxMs = parseNumber(
      value.at("delayMaxMs"), std::string(path) + "/delayMaxMs");
  loop.delayStrategy = parseDelayStrategy(
      value.at("delayStrategy"), std::string(path) + "/delayStrategy");
  loop.delaysSamples = parseUnsigned64Array(
      value.at("delaysSamples"), std::string(path) + "/delaysSamples");
  loop.delaysMs = parseNumberArray(
      value.at("delaysMs"), std::string(path) + "/delaysMs");
  loop.bufferSizes = parseUnsigned64Array(
      value.at("bufferSizes"), std::string(path) + "/bufferSizes");
  loop.rt60Sec =
      parseNumber(value.at("rt60Sec"), std::string(path) + "/rt60Sec");
  loop.gainMode = parseGainMode(
      value.at("gainMode"), std::string(path) + "/gainMode");
  loop.gains =
      parseNumberArray(value.at("gains"), std::string(path) + "/gains");
  loop.mix = parseMix(value.at("mix"), std::string(path) + "/mix");
  loop.matrix = std::move(matrix);
  loop.decayMargin = parseNumber(
      value.at("decayMargin"), std::string(path) + "/decayMargin");
  loop.tailBudgetSamples = parseUnsigned64(
      value.at("tailBudgetSamples"),
      std::string(path) + "/tailBudgetSamples");
  loop.blockSizeBoundSamples = parseUnsigned64(
      value.at("blockSizeBoundSamples"),
      std::string(path) + "/blockSizeBoundSamples");
  loop.silenceFloorDb = parseOptionalNumber(
      value.at("silenceFloorDb"), std::string(path) + "/silenceFloorDb");
  if (value.contains("damping")) {
    loop.damping = parseResolvedDamping(
        value.at("damping"), std::string(path) + "/damping");
  }
  if (value.contains("modulation")) {
    loop.modulation = parseResolvedModulation(
        value.at("modulation"), std::string(path) + "/modulation");
  }
  return loop;
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
       "leftChannel",
       "rightChannel",
       "normalisation",
       "compensation",
       "leftRow",
       "rightRow",
       "effectiveLeftRow",
       "effectiveRightRow",
       "alignment",
       "widthDeg",
       "widthMatrix"});
  for (const auto field :
       {"inputChannels",
        "outputChannels",
        "strategy",
        "normalisation",
        "compensation",
        "leftRow",
        "rightRow",
        "effectiveLeftRow",
        "effectiveRightRow",
        "alignment",
        "widthDeg",
        "widthMatrix"}) {
    requireField(value, field, path);
  }
  dsp::ResolvedDownmix downmix;
  downmix.inputChannels = parseUnsigned32(
      value.at("inputChannels"), std::string(path) + "/inputChannels");
  downmix.outputChannels = parseUnsigned32(
      value.at("outputChannels"), std::string(path) + "/outputChannels");
  downmix.strategy = parseDownmixStrategy(
      value.at("strategy"), std::string(path) + "/strategy");
  if (downmix.strategy == dsp::DownmixStrategy::select) {
    requireField(value, "leftChannel", path);
    downmix.leftChannel = parseUnsigned32(
        value.at("leftChannel"), std::string(path) + "/leftChannel");
    if (value.contains("rightChannel")) {
      downmix.rightChannel = parseUnsigned32(
          value.at("rightChannel"), std::string(path) + "/rightChannel");
    }
  } else {
    const auto strategyLabel = downmixStrategyLabel(downmix.strategy);
    if (value.contains("leftChannel")) {
      fail(
          std::string(path) + "/leftChannel",
          std::string("not applicable to strategy ") + strategyLabel);
    }
    if (value.contains("rightChannel")) {
      fail(
          std::string(path) + "/rightChannel",
          std::string("not applicable to strategy ") + strategyLabel);
    }
  }
  downmix.normalisation = parseNormalisation(
      value.at("normalisation"), std::string(path) + "/normalisation");
  downmix.compensation = parseNumber(
      value.at("compensation"), std::string(path) + "/compensation");
  downmix.leftRow = parseNumberArray(
      value.at("leftRow"), std::string(path) + "/leftRow");
  downmix.rightRow = parseNumberArray(
      value.at("rightRow"), std::string(path) + "/rightRow");
  downmix.effectiveLeftRow = parseNumberArray(
      value.at("effectiveLeftRow"),
      std::string(path) + "/effectiveLeftRow");
  downmix.effectiveRightRow = parseNumberArray(
      value.at("effectiveRightRow"),
      std::string(path) + "/effectiveRightRow");
  downmix.alignment = parseDownmixAlignment(
      value.at("alignment"), std::string(path) + "/alignment");
  downmix.widthDeg = parseNumber(
      value.at("widthDeg"), std::string(path) + "/widthDeg");
  downmix.widthMatrix = parseNumberArray(
      value.at("widthMatrix"), std::string(path) + "/widthMatrix");
  return downmix;
}

dsp::ResolvedEarlyTap parseResolvedEarlyTap(
    const Json& value, const std::string_view path) {
  requireObject(value, path);
  const std::initializer_list<std::string_view> fields{
      "stepIndex",
      "gainDb",
      "nominalSupportMinSamples",
      "nominalSupportMaxSamples",
      "nominalSupportMinMs",
      "nominalSupportMaxMs",
      "conservativeSupportMinSamples",
      "conservativeSupportMaxSamples",
      "conservativeSupportMinMs",
      "conservativeSupportMaxMs",
      "shapingGainDb",
      "gain",
  };
  rejectUnknownFields(value, path, fields);
  for (const auto field : fields) {
    requireField(value, field, path);
  }
  dsp::ResolvedEarlyTap tap;
  tap.stepIndex = parseUnsigned32(
      value.at("stepIndex"), std::string(path) + "/stepIndex");
  tap.gainDb =
      parseNumber(value.at("gainDb"), std::string(path) + "/gainDb");
  tap.nominalSupportMinSamples = parseUnsigned64(
      value.at("nominalSupportMinSamples"),
      std::string(path) + "/nominalSupportMinSamples");
  tap.nominalSupportMaxSamples = parseUnsigned64(
      value.at("nominalSupportMaxSamples"),
      std::string(path) + "/nominalSupportMaxSamples");
  tap.nominalSupportMinMs = parseNumber(
      value.at("nominalSupportMinMs"),
      std::string(path) + "/nominalSupportMinMs");
  tap.nominalSupportMaxMs = parseNumber(
      value.at("nominalSupportMaxMs"),
      std::string(path) + "/nominalSupportMaxMs");
  tap.conservativeSupportMinSamples = parseUnsigned64(
      value.at("conservativeSupportMinSamples"),
      std::string(path) + "/conservativeSupportMinSamples");
  tap.conservativeSupportMaxSamples = parseUnsigned64(
      value.at("conservativeSupportMaxSamples"),
      std::string(path) + "/conservativeSupportMaxSamples");
  tap.conservativeSupportMinMs = parseNumber(
      value.at("conservativeSupportMinMs"),
      std::string(path) + "/conservativeSupportMinMs");
  tap.conservativeSupportMaxMs = parseNumber(
      value.at("conservativeSupportMaxMs"),
      std::string(path) + "/conservativeSupportMaxMs");
  tap.shapingGainDb = parseNumber(
      value.at("shapingGainDb"), std::string(path) + "/shapingGainDb");
  tap.gain = parseNumber(value.at("gain"), std::string(path) + "/gain");
  return tap;
}

dsp::ResolvedEarlyReflections parseResolvedEarly(
    const Json& value, const std::string_view path) {
  requireObject(value, path);
  rejectUnknownFields(
      value,
      path,
      {"enabled", "levelDb", "gain", "decayDbPerSec", "taps", "downmix"});
  for (const auto field :
       {"enabled", "levelDb", "gain", "decayDbPerSec", "taps", "downmix"}) {
    requireField(value, field, path);
  }
  dsp::ResolvedEarlyReflections early;
  early.enabled =
      parseBoolean(value.at("enabled"), std::string(path) + "/enabled");
  early.levelDb =
      parseNumber(value.at("levelDb"), std::string(path) + "/levelDb");
  early.gain = parseNumber(value.at("gain"), std::string(path) + "/gain");
  early.decayDbPerSec = parseNumber(
      value.at("decayDbPerSec"), std::string(path) + "/decayDbPerSec");
  const auto& taps = value.at("taps");
  const auto tapsPath = std::string(path) + "/taps";
  requireArray(taps, tapsPath);
  early.taps.reserve(taps.size());
  for (std::size_t index = 0; index < taps.size(); ++index) {
    early.taps.push_back(
        parseResolvedEarlyTap(
            taps.at(index), tapsPath + "/" + std::to_string(index)));
  }
  const auto downmixPath = std::string(path) + "/downmix";
  checkNestedDownmixType(
      value.at("downmix"), downmixPath, /*requireType=*/true);
  early.downmix = parseResolvedDownmix(value.at("downmix"), downmixPath);
  return early;
}

dsp::ResolvedComposition parseResolvedComposition(const Json& value) {
  requireObject(value, "/composition");
  rejectUnknownFields(
      value,
      "/composition",
      {"stages", "mainEnabled", "mainLevelDb", "mainGain", "early"});
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
    } else if (type == "feedback-loop") {
      composition.stages.emplace_back(
          parseResolvedFeedbackLoop(stage, path));
    } else if (type == "downmix") {
      composition.stages.emplace_back(parseResolvedDownmix(stage, path));
    } else {
      fail(
          path + "/type",
          "expected split, diffuser, feedback-loop, or downmix");
    }
  }

  if (composition.stages.empty()) {
    if (value.contains("mainEnabled")) {
      fail(
          "/composition/mainEnabled",
          "not applicable to the empty identity Composition");
    }
    if (value.contains("mainLevelDb")) {
      fail(
          "/composition/mainLevelDb",
          "not applicable to the empty identity Composition");
    }
    if (value.contains("mainGain")) {
      fail(
          "/composition/mainGain",
          "not applicable to the empty identity Composition");
    }
    if (value.contains("early")) {
      fail(
          "/composition/early",
          "not applicable to the empty identity Composition");
    }
    return composition;
  }

  requireField(value, "mainEnabled", "/composition");
  composition.mainEnabled = parseBoolean(
      value.at("mainEnabled"), "/composition/mainEnabled");
  requireField(value, "mainLevelDb", "/composition");
  composition.mainLevelDb = parseNumber(
      value.at("mainLevelDb"), "/composition/mainLevelDb");
  requireField(value, "mainGain", "/composition");
  composition.mainGain = parseNumber(
      value.at("mainGain"), "/composition/mainGain");
  if (value.contains("early")) {
    composition.early =
        parseResolvedEarly(value.at("early"), "/composition/early");
  }
  return composition;
}

} // namespace

config::ReverbConfig parseRequestedConfig(const std::string_view contents) {
  const auto json = parseJson(contents);
  requireObject(json, "/");
  rejectUnknownFields(
      json, "/", {"formatVersion", "seed", "composition"});
  requireField(json, "formatVersion", "");
  const auto formatVersion =
      parseUnsigned32(json.at("formatVersion"), "/formatVersion");
  if (const auto reason = config::formatVersionRejectionReason(formatVersion)) {
    fail("/formatVersion", *reason);
  }

  config::ReverbConfig requested;
  requested.formatVersion = formatVersion;
  if (json.contains("seed")) {
    requested.seed = parseSeed(json.at("seed"));
  }
  if (json.contains("composition")) {
    requested.composition =
        parseRequestedComposition(json.at("composition"));
  }
  return requested;
}

dsp::ResolvedConfig parseResolvedConfig(
    const std::string_view contents,
    const std::uint64_t memoryBudgetBytes) {
  const auto json = parseJson(contents);
  requireObject(json, "/");
  rejectUnknownFields(
      json,
      "/",
      {"formatVersion", "seed", "sampleRate", "composition"});

  requireField(json, "formatVersion", "");
  const auto formatVersion =
      parseUnsigned32(json.at("formatVersion"), "/formatVersion");
  if (const auto reason = config::formatVersionRejectionReason(formatVersion)) {
    fail("/formatVersion", *reason);
  }

  for (const auto field : {"seed", "sampleRate", "composition"}) {
    requireField(json, field, "");
  }

  const dsp::ResolvedConfig resolved{
      formatVersion,
      parseSeed(json.at("seed")),
      parseUnsigned32(json.at("sampleRate"), "/sampleRate"),
      parseResolvedComposition(json.at("composition")),
  };
  config::validateResolvedConfig(resolved, memoryBudgetBytes);
  return resolved;
}

} // namespace rvrbotron::cli
