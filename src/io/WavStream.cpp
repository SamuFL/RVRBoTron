#include "rvrbotron/io/WavStream.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include <stdexcept>
#include <type_traits>

namespace rvrbotron::io {

struct WavReader::Implementation {
  drwav wav{};
};

WavReader::WavReader(const std::filesystem::path& path)
    : implementation_(std::make_unique<Implementation>()) {
  if (!drwav_init_file(&implementation_->wav, path.string().c_str(), nullptr)) {
    throw std::runtime_error("could not open input WAV: " + path.string());
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
  static_assert(
      std::is_same_v<dsp::Sample, float>,
      "The first identity tracer supports only the default float sample type");
  return static_cast<std::size_t>(drwav_read_pcm_frames_f32(
      &implementation_->wav,
      static_cast<drwav_uint64>(frameCount),
      interleaved));
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
      32,
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
