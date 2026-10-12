#pragma once

#include "rvrbotron/dsp/ProcessChunk.h"
#include "rvrbotron/dsp/Sample.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace rvrbotron::dsp {

class DelayLine {
public:
  DelayLine(
      std::vector<std::uint64_t> delaysSamples,
      std::vector<std::uint64_t> bufferSizes);

  [[nodiscard]] Sample read(const std::size_t channel) const noexcept {
    const auto ring = bufferSizes_[channel];
    const auto delay = delays_[channel];
    const auto position = positions_[channel];
    const auto readPosition =
        position >= delay ? position - delay : ring - (delay - position);
    return storage_[offsets_[channel] + readPosition];
  }

  void write(const std::size_t channel, const Sample value) noexcept {
    const auto index = offsets_[channel] + positions_[channel];
    storage_[index] = value;
    const auto ring = static_cast<std::size_t>(bufferSizes_[channel]);
    positions_[channel] = (positions_[channel] + 1) % ring;
  }

  // Writes `frames` consecutive samples, as `frames` calls to write().
  void writeBlock(const std::size_t channel,
                  const Sample* values,
                  std::size_t frames) noexcept {
    const auto ring = static_cast<std::size_t>(bufferSizes_[channel]);
    auto* const storage = storage_.data() + offsets_[channel];
    auto position = positions_[channel];
    if (frames >= ring) {
      // Only the last `ring` samples survive; the first of them lands
      // where the ring position ends up.
      values += frames - ring;
      position = (position + frames) % ring;
      frames = ring;
    }
    const auto first = std::min(frames, ring - position);
    std::copy_n(values, first, storage + position);
    std::copy_n(values + first, frames - first, storage);
    positions_[channel] = (position + frames) % ring;
  }

  // Reads the `frames` samples that read() would return over the next
  // `frames` frames, without writing. Exact only while every one of them
  // is already in the ring: frames <= delaySamples(channel).
  void readBlock(const std::size_t channel,
                 Sample* const values,
                 const std::size_t frames) const noexcept {
    const auto ring = static_cast<std::size_t>(bufferSizes_[channel]);
    const auto delay = static_cast<std::size_t>(delays_[channel]);
    const auto* const storage = storage_.data() + offsets_[channel];
    const auto position = positions_[channel];
    const auto readPosition =
        position >= delay ? position - delay : ring - (delay - position);
    const auto first = std::min(frames, ring - readPosition);
    std::copy_n(storage + readPosition, first, values);
    std::copy_n(storage, frames - first, values + first);
  }

  // Delays `frames` samples by the Channel's fixed delay, as `frames`
  // read()-then-write() pairs: output frames older than the chunk come
  // from the ring, the rest straight from `input`. `input` and `output`
  // must not overlap.
  void delayBlock(const std::size_t channel,
                  const Sample* const input,
                  Sample* const output,
                  const std::size_t frames) noexcept {
    const auto delay = static_cast<std::size_t>(delays_[channel]);
    const auto fromRing = frames < delay ? frames : delay;
    readBlock(channel, output, fromRing);
    for (std::size_t frame = fromRing; frame < frames; ++frame) {
      output[frame] = input[frame - delay];
    }
    writeBlock(channel, input, frames);
  }

  // The fractional reads take `frameOffset` to read as if that many
  // frames had already been written, without writing them: the ring index
  // wraps through the same `% ring` either way.
  [[nodiscard]] Sample readFraction(
      const std::size_t channel,
      const double lookbackSamples,
      const std::size_t frameOffset = 0) const noexcept {
    const auto ring = bufferSizes_[channel];
    const auto offset = offsets_[channel];
    const auto position = positions_[channel] + frameOffset;
    const auto base = std::floor(lookbackSamples);
    const auto frac = static_cast<Sample>(lookbackSamples - base);
    const auto baseLookback = static_cast<std::int64_t>(base);
    const auto points = lagrangePoints(
        storage_.data() + offset, position, ring, baseLookback);
    return lagrange3(frac, points);
  }

  // readFraction() for `frames` consecutive frames, the i-th as
  // readFraction(channel, lookbacks[i], i), without writing: exact while
  // every sample read is already in the ring. Gathers first, then
  // interpolates the whole chunk at once.
  void readFractionBlock(const std::size_t channel,
                         const double* const lookbacks,
                         Sample* const values,
                         const std::size_t frames) const noexcept {
    const auto ring = bufferSizes_[channel];
    const auto* const storage = storage_.data() + offsets_[channel];
    Sample fracs[kMaxChunkFrames];
    LagrangePoints points[kMaxChunkFrames];
    for (std::size_t frame = 0; frame < frames; ++frame) {
      const auto base = std::floor(lookbacks[frame]);
      fracs[frame] = static_cast<Sample>(lookbacks[frame] - base);
      points[frame] = lagrangePoints(
          storage, positions_[channel] + frame, ring,
          static_cast<std::int64_t>(base));
    }
    for (std::size_t frame = 0; frame < frames; ++frame) {
      values[frame] = lagrange3(fracs[frame], points[frame]);
    }
  }

  [[nodiscard]] Sample readFractionLinear(
      const std::size_t channel,
      const double lookbackSamples,
      const std::size_t frameOffset = 0) const noexcept {
    const auto ring = bufferSizes_[channel];
    const auto offset = offsets_[channel];
    const auto position = positions_[channel] + frameOffset;
    const auto base = std::floor(lookbackSamples);
    const auto frac = static_cast<Sample>(lookbackSamples - base);
    const auto baseLookback = static_cast<std::int64_t>(base);
    const auto [p1, p2] = adjacentPoints(
        storage_.data() + offset, position, ring, baseLookback);
    return p1 + frac * (p2 - p1);
  }

  [[nodiscard]] Sample readFractionAllpass(
      const std::size_t channel,
      const double lookbackSamples,
      Sample& state,
      const std::size_t frameOffset = 0) const noexcept {
    const auto ring = bufferSizes_[channel];
    const auto offset = offsets_[channel];
    const auto position = positions_[channel] + frameOffset;
    const auto base = std::floor(lookbackSamples);
    const auto frac = static_cast<Sample>(lookbackSamples - base);
    const auto baseLookback = static_cast<std::int64_t>(base);
    const auto [x0, x1] = adjacentPoints(
        storage_.data() + offset, position, ring, baseLookback);
    const auto coefficient = (Sample{1} - frac) / (Sample{1} + frac);
    const auto output = coefficient * x0 + x1 - coefficient * state;
    state = output;
    return output;
  }

  [[nodiscard]] std::uint64_t delaySamples(
      const std::size_t channel) const noexcept {
    return delays_[channel];
  }

  [[nodiscard]] std::size_t ownedStorageBytes() const noexcept;

private:
  // Cubic Lagrange interpolation through 4 equally spaced samples at
  // relative positions -1, 0, 1, 2, evaluated at `frac` in [0, 1]
  // between p1 (frac=0) and p2 (frac=1).
  struct LagrangePoints {
    Sample p0;
    Sample p1;
    Sample p2;
    Sample p3;
  };

  [[nodiscard]] static Sample lagrange3(
      const Sample frac, const LagrangePoints& points) noexcept {
    const auto [p0, p1, p2, p3] = points;
    const auto c0 = -frac * (frac - Sample{1}) * (frac - Sample{2}) /
        Sample{6};
    const auto c1 =
        (frac + Sample{1}) * (frac - Sample{1}) * (frac - Sample{2}) /
        Sample{2};
    const auto c2 =
        -(frac + Sample{1}) * frac * (frac - Sample{2}) / Sample{2};
    const auto c3 = (frac + Sample{1}) * frac * (frac - Sample{1}) /
        Sample{6};
    return c0 * p0 + c1 * p1 + c2 * p2 + c3 * p3;
  }

  // Where a lookback lands in a ring whose next write goes to `position`:
  // (position + ring - lookback % ring) % ring, with a negative lookback
  // wrapping as its unsigned two's-complement value. A lookback already
  // inside the ring, from a position less than two rings in, needs only
  // subtractions; the modulo covers everything else.
  [[nodiscard]] static std::uint64_t ringPosition(
      const std::uint64_t position,
      const std::uint64_t ring,
      const std::int64_t lookback) noexcept {
    if (lookback >= 0 && static_cast<std::uint64_t>(lookback) < ring) {
      auto at = position + ring - static_cast<std::uint64_t>(lookback);
      at = at >= ring ? at - ring : at;
      at = at >= ring ? at - ring : at;
      return at >= ring ? at % ring : at;
    }
    const auto wrapped = static_cast<std::uint64_t>(lookback) % ring;
    return (position + ring - wrapped) % ring;
  }

  // The samples at lookbacks base - 1 ... base + 2. Consecutive
  // non-negative lookbacks sit at consecutive ring positions, so one
  // position locates all four.
  [[nodiscard]] static LagrangePoints lagrangePoints(
      const Sample* const storage,
      const std::uint64_t position,
      const std::uint64_t ring,
      const std::int64_t base) noexcept {
    if (base >= 1) {
      const auto at1 = ringPosition(position, ring, base);
      const auto at2 = olderPosition(at1, ring);
      return {storage[newerPosition(at1, ring)], storage[at1], storage[at2],
              storage[olderPosition(at2, ring)]};
    }
    return {storage[ringPosition(position, ring, base - 1)],
            storage[ringPosition(position, ring, base)],
            storage[ringPosition(position, ring, base + 1)],
            storage[ringPosition(position, ring, base + 2)]};
  }

  // The samples at lookbacks base and base + 1.
  [[nodiscard]] static std::pair<Sample, Sample> adjacentPoints(
      const Sample* const storage,
      const std::uint64_t position,
      const std::uint64_t ring,
      const std::int64_t base) noexcept {
    const auto at = ringPosition(position, ring, base);
    return {storage[at],
            base >= 0 ? storage[olderPosition(at, ring)]
                      : storage[ringPosition(position, ring, base + 1)]};
  }

  // The ring position one sample further back in time, and one sample
  // more recent.
  [[nodiscard]] static std::uint64_t olderPosition(
      const std::uint64_t at, const std::uint64_t ring) noexcept {
    return at == 0 ? ring - 1 : at - 1;
  }
  [[nodiscard]] static std::uint64_t newerPosition(
      const std::uint64_t at, const std::uint64_t ring) noexcept {
    return at + 1 == ring ? 0 : at + 1;
  }

  std::vector<std::uint64_t> delays_;
  std::vector<std::uint64_t> bufferSizes_;
  std::vector<std::size_t> offsets_;
  std::vector<std::size_t> positions_;
  std::vector<Sample> storage_;
};

} // namespace rvrbotron::dsp
