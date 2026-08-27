#pragma once

#include <string>
#include <string_view>

namespace rvrbotron::cli {

[[nodiscard]] std::string_view platformName() noexcept;
[[nodiscard]] std::string_view architectureName() noexcept;
[[nodiscard]] std::string compilerName();

} // namespace rvrbotron::cli
