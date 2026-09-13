#pragma once

#include <filesystem>
#include <string>

namespace rvrbotron::cli {

[[nodiscard]] std::string fileSha256(const std::filesystem::path& path);

} // namespace rvrbotron::cli
