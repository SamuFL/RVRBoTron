#include "rvrbotron/config/ConfigJson.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace rvrbotron::config {
namespace {

using Json = nlohmann::json;

[[noreturn]] void fail(const std::string_view path,
                       const std::string_view reason) {
  throw std::runtime_error(
      "configuration error at " + std::string(path) + ": " +
      std::string(reason));
}

Json readJson(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error(
        "could not open configuration: " + path.string());
  }

  try {
    return Json::parse(input);
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

std::uint32_t readFormatVersion(const Json& object,
                                const bool required) {
  if (!object.contains("formatVersion")) {
    if (required) {
      fail("/formatVersion", "required field is missing");
    }
    return 1;
  }

  const auto& value = object.at("formatVersion");
  if (!value.is_number_unsigned() || value != 1) {
    fail("/formatVersion", "expected integer 1");
  }
  return 1;
}

std::uint64_t readSeed(const Json& object, const bool required) {
  if (!object.contains("seed")) {
    if (required) {
      fail("/seed", "required field is missing");
    }
    return 0;
  }

  const auto& value = object.at("seed");
  if (!value.is_number_unsigned()) {
    fail("/seed", "expected unsigned 64-bit integer");
  }
  return value.get<std::uint64_t>();
}

void readComposition(const Json& object, const bool required) {
  if (!object.contains("composition")) {
    if (required) {
      fail("/composition", "required field is missing");
    }
    return;
  }

  const auto& composition = object.at("composition");
  requireObject(composition, "/composition");
  rejectUnknownFields(composition, "/composition", {"stages"});

  if (!composition.contains("stages")) {
    if (required) {
      fail("/composition/stages", "required field is missing");
    }
    return;
  }

  const auto& stages = composition.at("stages");
  if (!stages.is_array() || !stages.empty()) {
    fail("/composition/stages", "expected empty array");
  }
}

} // namespace

dsp::ResolvedConfig
resolveDefaultConfig(const std::uint32_t sampleRate) noexcept {
  return {1, 0, sampleRate, {}};
}

dsp::ResolvedConfig
resolveRequestedConfig(const std::filesystem::path& path,
                       const std::uint32_t sampleRate) {
  const auto requested = readJson(path);
  requireObject(requested, "/");
  rejectUnknownFields(
      requested, "/", {"formatVersion", "seed", "composition"});
  readComposition(requested, false);

  return {
      readFormatVersion(requested, false),
      readSeed(requested, false),
      sampleRate,
      {},
  };
}

dsp::ResolvedConfig
readResolvedConfig(const std::filesystem::path& path) {
  const auto resolved = readJson(path);
  requireObject(resolved, "/");
  rejectUnknownFields(
      resolved,
      "/",
      {"formatVersion", "seed", "sampleRate", "composition"});

  if (!resolved.contains("sampleRate")) {
    fail("/sampleRate", "required field is missing");
  }

  const auto& sampleRateValue = resolved.at("sampleRate");
  if (!sampleRateValue.is_number_unsigned()) {
    fail("/sampleRate", "expected unsigned 32-bit integer");
  }
  const auto wideSampleRate = sampleRateValue.get<std::uint64_t>();
  if (wideSampleRate > std::numeric_limits<std::uint32_t>::max()) {
    fail("/sampleRate", "expected unsigned 32-bit integer");
  }
  const auto sampleRate = static_cast<std::uint32_t>(wideSampleRate);
  if (sampleRate == 0) {
    fail("/sampleRate", "expected value greater than zero");
  }
  readComposition(resolved, true);

  return {
      readFormatVersion(resolved, true),
      readSeed(resolved, true),
      sampleRate,
      {},
  };
}

} // namespace rvrbotron::config
