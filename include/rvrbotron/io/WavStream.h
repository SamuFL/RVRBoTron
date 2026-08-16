#pragma once

#include "rvrbotron/dsp/Sample.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace rvrbotron::io {

struct WavInfo {
  std::uint32_t channels;
  std::uint32_t sampleRate;
  std::uint64_t frameCount;
};

class WavReader {
public:
  explicit WavReader(const std::filesystem::path& path);
  ~WavReader();

  WavReader(const WavReader&) = delete;
  WavReader& operator=(const WavReader&) = delete;

  [[nodiscard]] WavInfo info() const noexcept;
  std::size_t readFrames(dsp::Sample* interleaved, std::size_t frameCount);

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

class WavWriter {
public:
  WavWriter(const std::filesystem::path& path,
            std::uint32_t channels,
            std::uint32_t sampleRate);
  ~WavWriter();

  WavWriter(const WavWriter&) = delete;
  WavWriter& operator=(const WavWriter&) = delete;

  void writeFrames(const dsp::Sample* interleaved, std::size_t frameCount);

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

} // namespace rvrbotron::io
