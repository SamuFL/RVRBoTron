#pragma once

#include <filesystem>

namespace rvrbotron::cli {

class RenderResultTransaction {
public:
  explicit RenderResultTransaction(std::filesystem::path destination);
  ~RenderResultTransaction();

  RenderResultTransaction(const RenderResultTransaction&) = delete;
  RenderResultTransaction& operator=(const RenderResultTransaction&) = delete;

  [[nodiscard]] const std::filesystem::path& workingPath() const noexcept;
  void publish();

private:
  std::filesystem::path destination_;
  std::filesystem::path workingPath_;
  bool published_{false};
};

} // namespace rvrbotron::cli
