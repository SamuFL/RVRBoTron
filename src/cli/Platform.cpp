#include "rvrbotron/cli/Platform.h"

namespace rvrbotron::cli {

std::string_view platformName() noexcept {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__linux__)
  return "linux";
#else
  return "unknown";
#endif
}

std::string_view architectureName() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
  return "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
  return "arm64";
#else
  return "unknown";
#endif
}

std::string compilerName() {
#if defined(__clang__)
  return "clang-" + std::to_string(__clang_major__) + "." +
         std::to_string(__clang_minor__) + "." +
         std::to_string(__clang_patchlevel__);
#elif defined(__GNUC__)
  return "gcc-" + std::to_string(__GNUC__) + "." +
         std::to_string(__GNUC_MINOR__) + "." +
         std::to_string(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
  return "msvc-" + std::to_string(_MSC_VER);
#else
  return "unknown";
#endif
}

} // namespace rvrbotron::cli
