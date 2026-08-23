#include "rvrbotron/io/WavStream.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace rvrbotron::io {
namespace {

std::uint16_t readU16(const std::uint8_t* bytes) noexcept {
  return static_cast<std::uint16_t>(
      bytes[0] | static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t readU32(const std::uint8_t* bytes) noexcept {
  return static_cast<std::uint32_t>(
      bytes[0] | static_cast<std::uint32_t>(bytes[1]) << 8 |
      static_cast<std::uint32_t>(bytes[2]) << 16 |
      static_cast<std::uint32_t>(bytes[3]) << 24);
}

void readAt(std::ifstream& input,
            const std::uint64_t offset,
            std::uint8_t* destination,
            const std::size_t byteCount) {
  input.seekg(static_cast<std::streamoff>(offset));
  input.read(
      reinterpret_cast<char*>(destination),
      static_cast<std::streamsize>(byteCount));
  if (!input) {
    throw std::runtime_error("truncated input WAV");
  }
}

void validateWavHeader(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    throw std::runtime_error("could not open input WAV: " + path.string());
  }
  const auto end = input.tellg();
  if (end < 0) {
    throw std::runtime_error("could not inspect input WAV: " + path.string());
  }
  const auto fileSize = static_cast<std::uint64_t>(end);
  if (fileSize < 12) {
    throw std::runtime_error("malformed input WAV");
  }

  std::array<std::uint8_t, 16> bytes{};
  readAt(input, 0, bytes.data(), 12);
  if (std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
      std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
    throw std::runtime_error("malformed input WAV");
  }

  const auto riffSize = static_cast<std::uint64_t>(readU32(bytes.data() + 4));
  const auto riffEnd = riffSize + 8;
  if (riffEnd > fileSize) {
    throw std::runtime_error("truncated input WAV");
  }
  if (riffEnd < 12) {
    throw std::runtime_error("malformed input WAV");
  }

  bool foundFormat = false;
  bool foundData = false;
  std::uint16_t formatTag = 0;
  std::uint16_t channels = 0;
  std::uint32_t sampleRate = 0;
  std::uint32_t byteRate = 0;
  std::uint16_t blockAlign = 0;
  std::uint16_t bitsPerSample = 0;
  std::uint32_t dataSize = 0;

  std::uint64_t offset = 12;
  while (offset + 8 <= riffEnd) {
    readAt(input, offset, bytes.data(), 8);
    const auto chunkSize =
        static_cast<std::uint64_t>(readU32(bytes.data() + 4));
    const auto chunkData = offset + 8;
    const auto chunkEnd = chunkData + chunkSize;
    const auto paddedEnd = chunkEnd + (chunkSize % 2);
    if (chunkEnd > riffEnd || paddedEnd > fileSize) {
      throw std::runtime_error("truncated input WAV");
    }

    if (std::memcmp(bytes.data(), "fmt ", 4) == 0) {
      if (chunkSize < 16) {
        throw std::runtime_error("malformed input WAV");
      }
      readAt(input, chunkData, bytes.data(), 16);
      formatTag = readU16(bytes.data());
      channels = readU16(bytes.data() + 2);
      sampleRate = readU32(bytes.data() + 4);
      byteRate = readU32(bytes.data() + 8);
      blockAlign = readU16(bytes.data() + 12);
      bitsPerSample = readU16(bytes.data() + 14);
      foundFormat = true;
    } else if (std::memcmp(bytes.data(), "data", 4) == 0) {
      dataSize = static_cast<std::uint32_t>(chunkSize);
      foundData = true;
    }
    offset = paddedEnd;
  }

  if (!foundFormat || !foundData || offset != riffEnd) {
    throw std::runtime_error("malformed input WAV");
  }
  if (channels == 0 || channels > 2) {
    throw std::runtime_error("WAV channel count must be mono or stereo");
  }

  const bool supportedPcm =
      formatTag == DR_WAVE_FORMAT_PCM &&
      (bitsPerSample == 16 || bitsPerSample == 24 ||
       bitsPerSample == 32);
  const bool supportedFloat =
      formatTag == DR_WAVE_FORMAT_IEEE_FLOAT &&
      (bitsPerSample == 32 || bitsPerSample == 64);
  if (!supportedPcm && !supportedFloat) {
    throw std::runtime_error("unsupported WAV encoding");
  }

  const auto bytesPerSample = static_cast<std::uint16_t>(bitsPerSample / 8);
  const auto expectedBlockAlign =
      static_cast<std::uint16_t>(channels * bytesPerSample);
  if (sampleRate == 0 || blockAlign != expectedBlockAlign ||
      byteRate != sampleRate * expectedBlockAlign ||
      dataSize % expectedBlockAlign != 0) {
    throw std::runtime_error("malformed input WAV");
  }
}

std::uint64_t readLittleEndian(const std::uint8_t* bytes,
                               const std::size_t byteCount) noexcept {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < byteCount; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
  }
  return value;
}

dsp::Sample decodePcm(const std::uint8_t* bytes,
                      const std::uint32_t bitsPerSample) {
  const auto byteCount = static_cast<std::size_t>(bitsPerSample / 8);
  const auto encoded = readLittleEndian(bytes, byteCount);
  const auto signBit = std::uint64_t{1} << (bitsPerSample - 1);
  const auto range = std::uint64_t{1} << bitsPerSample;
  const auto value = encoded >= signBit
                         ? static_cast<std::int64_t>(encoded - range)
                         : static_cast<std::int64_t>(encoded);
  return static_cast<dsp::Sample>(
      static_cast<double>(value) / static_cast<double>(signBit));
}

dsp::Sample decodeIeeeFloat(const std::uint8_t* bytes,
                            const std::uint32_t bitsPerSample) {
  if (bitsPerSample == 32) {
    const auto encoded =
        static_cast<std::uint32_t>(readLittleEndian(bytes, 4));
    float value = 0.0F;
    std::memcpy(&value, &encoded, sizeof(value));
    return static_cast<dsp::Sample>(value);
  }

  const auto encoded = readLittleEndian(bytes, 8);
  double value = 0.0;
  std::memcpy(&value, &encoded, sizeof(value));
  return static_cast<dsp::Sample>(value);
}

} // namespace

struct WavReader::Implementation {
  drwav wav{};
  std::vector<std::uint8_t> encodedSamples;
};

WavReader::WavReader(const std::filesystem::path& path)
    : implementation_(std::make_unique<Implementation>()) {
  validateWavHeader(path);
  if (!drwav_init_file(&implementation_->wav, path.string().c_str(), nullptr)) {
    throw std::runtime_error("malformed input WAV");
  }
}

WavReader::~WavReader() {
  drwav_uninit(&implementation_->wav);
}

WavInfo WavReader::info() const noexcept {
  return {
      implementation_->wav.channels,
      implementation_->wav.sampleRate,
      implementation_->wav.totalPCMFrameCount,
  };
}

std::size_t WavReader::readFrames(dsp::Sample* interleaved,
                                  const std::size_t frameCount) {
  const auto bytesPerSample =
      static_cast<std::size_t>(implementation_->wav.bitsPerSample / 8);
  const auto channels =
      static_cast<std::size_t>(implementation_->wav.channels);
  if (frameCount >
      std::numeric_limits<std::size_t>::max() / channels / bytesPerSample) {
    throw std::runtime_error("WAV processing block is too large");
  }

  implementation_->encodedSamples.resize(
      frameCount * channels * bytesPerSample);
  const auto framesRead = drwav_read_pcm_frames_le(
      &implementation_->wav,
      static_cast<drwav_uint64>(frameCount),
      implementation_->encodedSamples.data());
  const auto remainingFrames =
      implementation_->wav.totalPCMFrameCount -
      (implementation_->wav.readCursorInPCMFrames -
       static_cast<drwav_uint64>(framesRead));
  const auto expectedFrames = std::min(
      static_cast<drwav_uint64>(frameCount),
      remainingFrames);
  if (framesRead != expectedFrames) {
    throw std::runtime_error("truncated input WAV");
  }
  const auto sampleCount = static_cast<std::size_t>(framesRead) * channels;
  for (std::size_t index = 0; index < sampleCount; ++index) {
    const auto* encoded =
        implementation_->encodedSamples.data() + index * bytesPerSample;
    if (implementation_->wav.translatedFormatTag == DR_WAVE_FORMAT_PCM) {
      interleaved[index] =
          decodePcm(encoded, implementation_->wav.bitsPerSample);
    } else {
      interleaved[index] =
          decodeIeeeFloat(encoded, implementation_->wav.bitsPerSample);
    }
    if (!std::isfinite(interleaved[index])) {
      throw std::runtime_error("non-finite input sample");
    }
  }
  return static_cast<std::size_t>(framesRead);
}

struct WavWriter::Implementation {
  drwav wav{};
};

WavWriter::WavWriter(const std::filesystem::path& path,
                     const std::uint32_t channels,
                     const std::uint32_t sampleRate)
    : implementation_(std::make_unique<Implementation>()) {
  const drwav_data_format format{
      drwav_container_riff,
      DR_WAVE_FORMAT_IEEE_FLOAT,
      channels,
      sampleRate,
      static_cast<drwav_uint32>(sizeof(dsp::Sample) * 8),
  };

  if (!drwav_init_file_write(
          &implementation_->wav, path.string().c_str(), &format, nullptr)) {
    throw std::runtime_error("could not create output WAV: " + path.string());
  }
}

WavWriter::~WavWriter() {
  drwav_uninit(&implementation_->wav);
}

void WavWriter::writeFrames(const dsp::Sample* interleaved,
                            const std::size_t frameCount) {
  const auto written = drwav_write_pcm_frames(
      &implementation_->wav,
      static_cast<drwav_uint64>(frameCount),
      interleaved);
  if (written != frameCount) {
    throw std::runtime_error("could not write all output WAV frames");
  }
}

} // namespace rvrbotron::io
