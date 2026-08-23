#include "rvrbotron/cli/ConfigJson.h"

#include "rvrbotron/config/ResolveConfig.h"

#include <nlohmann/json.hpp>

#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace rvrbotron::cli {
namespace {

using Json = nlohmann::json;

[[noreturn]] void fail(const std::string_view path,
                       const std::string_view reason) {
  throw std::runtime_error(
      "configuration error at " + std::string(path) + ": " +
      std::string(reason));
}

Json parseJson(const std::string_view contents) {
  try {
    return Json::parse(contents);
  } catch (const Json::parse_error& error) {
    fail("/", "malformed JSON: " + std::string(error.what()));
  }
}

void requireObject(const Json& value, const std::string_view path) {
  if (!value.is_object()) {
    fail(path, "expected object");
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

std::uint64_t parseSeed(const Json& value) {
  if (!value.is_number_unsigned()) {
    fail("/seed", "expected unsigned 64-bit integer");
  }
  return value.get<std::uint64_t>();
}

config::CompositionConfig parseRequestedComposition(const Json& value) {
  requireObject(value, "/composition");
  rejectUnknownFields(value, "/composition", {"stages"});

  config::CompositionConfig composition;
  if (value.contains("stages")) {
    const auto& stages = value.at("stages");
    if (!stages.is_array() || !stages.empty()) {
      fail("/composition/stages", "expected empty array");
    }
    composition.stagesSpecified = true;
  }
  return composition;
}

void parseResolvedComposition(const Json& value) {
  const auto composition = parseRequestedComposition(value);
  if (!composition.stagesSpecified) {
    fail("/composition/stages", "required field is missing");
  }
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
    if (!json.contains(field)) {
      fail("/" + std::string(field), "required field is missing");
    }
  }

  parseResolvedComposition(json.at("composition"));
  const dsp::ResolvedConfig resolved{
      parseUnsigned32(json.at("formatVersion"), "/formatVersion"),
      parseSeed(json.at("seed")),
      parseUnsigned32(json.at("sampleRate"), "/sampleRate"),
      {},
  };
  config::validateResolvedConfig(resolved);
  return resolved;
}

} // namespace rvrbotron::cli
