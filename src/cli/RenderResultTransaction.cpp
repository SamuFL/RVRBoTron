#include "rvrbotron/cli/RenderResultTransaction.h"

#include "rvrbotron/HarnessError.h"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <cerrno>
#include <stdio.h>
#endif

#include <cstddef>
#include <string>
#include <system_error>
#include <utility>

namespace rvrbotron::cli {
namespace {

bool pathExists(const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) {
    return false;
  }
  if (error) {
    throw HarnessError(
        ErrorCategory::ioFailure,
        "could not inspect destination: " + error.message());
  }
  return status.type() != std::filesystem::file_type::not_found;
}

std::error_code publishWithoutReplacement(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
#if defined(_WIN32)
  if (MoveFileExW(
          source.c_str(),
          destination.c_str(),
          MOVEFILE_WRITE_THROUGH) != 0) {
    return {};
  }
  return {
      static_cast<int>(GetLastError()),
      std::system_category(),
  };
#elif defined(__APPLE__)
  if (renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL) == 0) {
    return {};
  }
  return {errno, std::generic_category()};
#else
  if (pathExists(destination)) {
    return std::make_error_code(std::errc::file_exists);
  }
  std::error_code error;
  std::filesystem::rename(source, destination, error);
  return error;
#endif
}

} // namespace

RenderResultTransaction::RenderResultTransaction(
    std::filesystem::path destination)
    : destination_(std::move(destination)) {
  if (pathExists(destination_)) {
    throw HarnessError(
        ErrorCategory::ioFailure, "output path already exists");
  }

  const auto parent = destination_.parent_path();
  if (!parent.empty()) {
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
      throw HarnessError(
          ErrorCategory::ioFailure,
          "could not create output parent: " + error.message());
    }
  }

  const auto temporaryPrefix =
      "." + destination_.filename().string() + ".rvrbotron-tmp-";
  for (std::size_t candidate = 0; candidate < 1000; ++candidate) {
    workingPath_ =
        parent / (temporaryPrefix + std::to_string(candidate));
    std::error_code error;
    if (std::filesystem::create_directory(workingPath_, error)) {
      return;
    }
    if (error && error != std::errc::file_exists) {
      throw HarnessError(
          ErrorCategory::ioFailure,
          "could not create temporary Render Result: " + error.message());
    }
  }
  throw HarnessError(
      ErrorCategory::ioFailure,
      "could not reserve temporary Render Result");
}

RenderResultTransaction::~RenderResultTransaction() {
  if (!published_ && !workingPath_.empty()) {
    std::error_code error;
    std::filesystem::remove_all(workingPath_, error);
  }
}

const std::filesystem::path&
RenderResultTransaction::workingPath() const noexcept {
  return workingPath_;
}

void RenderResultTransaction::publish() {
  const auto error =
      publishWithoutReplacement(workingPath_, destination_);
  if (error) {
    if (error == std::errc::file_exists) {
      throw HarnessError(
          ErrorCategory::ioFailure, "output path already exists");
    }
    throw HarnessError(
        ErrorCategory::ioFailure,
        "could not publish Render Result: " + error.message());
  }
  published_ = true;
}

} // namespace rvrbotron::cli
