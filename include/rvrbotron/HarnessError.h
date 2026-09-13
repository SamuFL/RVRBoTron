#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace rvrbotron {

enum class ErrorCategory {
  malformedJson = 2,
  unsupportedAudio = 3,
  invalidConfiguration = 4,
  ioFailure = 5,
  internalProcessingFailure = 6,
  invalidArguments = 7,
};

constexpr int exitCode(const ErrorCategory category) noexcept {
  return static_cast<int>(category);
}

constexpr std::string_view categoryName(
    const ErrorCategory category) noexcept {
  switch (category) {
  case ErrorCategory::malformedJson:
    return "malformed_json";
  case ErrorCategory::unsupportedAudio:
    return "unsupported_audio";
  case ErrorCategory::invalidConfiguration:
    return "invalid_configuration";
  case ErrorCategory::ioFailure:
    return "io_failure";
  case ErrorCategory::internalProcessingFailure:
    return "internal_processing_failure";
  case ErrorCategory::invalidArguments:
    return "invalid_arguments";
  }
  return "internal_processing_failure";
}

class HarnessError : public std::runtime_error {
public:
  HarnessError(ErrorCategory category,
               std::string reason,
               std::optional<std::string> location = std::nullopt)
      : std::runtime_error(reason),
        category_(category),
        reason_(std::move(reason)),
        location_(std::move(location)) {}

  [[nodiscard]] ErrorCategory category() const noexcept {
    return category_;
  }

  [[nodiscard]] const std::string& reason() const noexcept {
    return reason_;
  }

  [[nodiscard]] const std::optional<std::string>& location() const noexcept {
    return location_;
  }

private:
  ErrorCategory category_;
  std::string reason_;
  std::optional<std::string> location_;
};

} // namespace rvrbotron
