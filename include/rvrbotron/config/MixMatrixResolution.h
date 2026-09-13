#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace rvrbotron::config {

[[nodiscard]] std::optional<std::size_t> matrixElementCount(
    std::uint32_t channels) noexcept;

[[nodiscard]] std::vector<double> resolveHadamardMatrix(
    std::uint32_t channels);

[[nodiscard]] std::vector<double> resolveHouseholderMatrix(
    std::uint32_t channels);

constexpr std::uint64_t kMixMatrixRandomOrthogonalUsage =
    0x4d49584f5254484fULL;

[[nodiscard]] std::vector<double> fillRandomOrthogonalSeed(
    std::uint32_t channels, std::uint64_t seed, std::uint64_t usage);

[[nodiscard]] std::optional<std::vector<double>> householderQrOrthogonalize(
    std::vector<double> matrix, std::uint32_t channels);

[[nodiscard]] std::optional<std::vector<double>>
resolveRandomOrthogonalMatrix(
    std::uint32_t channels, std::uint64_t seed, std::uint64_t usage);

} // namespace rvrbotron::config
