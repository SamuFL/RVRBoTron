#include "rvrbotron/config/ResolveConfig.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace rvrbotron::config {
namespace {

[[noreturn]] void fail(const std::string_view path,
                       const std::string_view reason) {
  throw std::runtime_error(
      "configuration error at " + std::string(path) + ": " +
      std::string(reason));
}

} // namespace

dsp::ResolvedConfig resolveConfig(const ReverbConfig& requested,
                                  const std::uint32_t sampleRate) {
  const dsp::ResolvedConfig resolved{
      requested.formatVersion.value_or(1),
      requested.seed.value_or(0),
      sampleRate,
      {},
  };
  validateResolvedConfig(resolved);
  return resolved;
}

void validateResolvedConfig(const dsp::ResolvedConfig& resolved) {
  if (resolved.formatVersion != 1) {
    fail("/formatVersion", "expected integer 1");
  }
  if (resolved.sampleRate == 0) {
    fail("/sampleRate", "expected value greater than zero");
  }
}

} // namespace rvrbotron::config
