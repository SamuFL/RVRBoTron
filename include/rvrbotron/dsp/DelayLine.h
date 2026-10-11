#pragma once

#include "rvrbotron/dsp/ProcessChunk.h"
#include "rvrbotron/dsp/Sample.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
    const auto* const storage = storage_.data() + offset;
    Sample p0;
    Sample p1;
    Sample p2;
    Sample p3;
    if (baseLookback >= 1) {
      // Consecutive non-negative lookbacks sit at consecutive ring
      // positions, so one modulo locates all four.
      const auto at1 = ringPosition(position, ring, baseLookback);
      const auto at2 = olderPosition(at1, ring);
      p0 = storage[newerPosition(at1, ring)];
      p1 = storage[at1];
      p2 = storage[at2];
      p3 = storage[olderPosition(at2, ring)];
    } else {
      p0 = storage[ringPosition(position, ring, baseLookback - 1)];
      p1 = storage[ringPosition(position, ring, baseLookback)];
      p2 = storage[ringPosition(position, ring, baseLookback + 1)];
      p3 = storage[ringPosition(position, ring, baseLookback + 2)];
    }
    return lagrange3(frac, p0, p1, p2, p3);
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
    Sample p0[kMaxChunkFrames];
    Sample p1[kMaxChunkFrames];
    Sample p2[kMaxChunkFrames];
    Sample p3[kMaxChunkFrames];
    for (std::size_t frame = 0; frame < frames; ++frame) {
      const auto position = positions_[channel] + frame;
      const auto base = std::floor(lookbacks[frame]);
      fracs[frame] = static_cast<Sample>(lookbacks[frame] - base);
      const auto baseLookback = static_cast<std::int64_t>(base);
      if (baseLookback >= 1 &&
          static_cast<std::uint64_t>(baseLookback) < ring) {
        // ringPosition() without division: the lookback is already
        // reduced, and position < 2 * ring while frames stay below the
        // delay, so two subtractions finish the job; the modulo only
        // guards a caller that breaks that precondition.
        auto at1 = position + ring - static_cast<std::uint64_t>(baseLookback);
        at1 = at1 >= ring ? at1 - ring : at1;
        at1 = at1 >= ring ? at1 - ring : at1;
        at1 = at1 >= ring ? at1 % ring : at1;
        const auto at2 = olderPosition(at1, ring);
        p0[frame] = storage[newerPosition(at1, ring)];
        p1[frame] = storage[at1];
        p2[frame] = storage[at2];
        p3[frame] = storage[olderPosition(at2, ring)];
      } else {
        p0[frame] = storage[ringPosition(position, ring, baseLookback - 1)];
        p1[frame] = storage[ringPosition(position, ring, baseLookback)];
        p2[frame] = storage[ringPosition(position, ring, baseLookback + 1)];
        p3[frame] = storage[ringPosition(position, ring, baseLookback + 2)];
      }
    }
    for (std::size_t frame = 0; frame < frames; ++frame) {
      values[frame] = lagrange3(
          fracs[frame], p0[frame], p1[frame], p2[frame], p3[frame]);
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
    const auto* const storage = storage_.data() + offset;
    const auto at1 = ringPosition(position, ring, baseLookback);
    const auto p1 = storage[at1];
    const auto p2 = baseLookback >= 0
        ? storage[olderPosition(at1, ring)]
        : storage[ringPosition(position, ring, baseLookback + 1)];
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
    const auto* const storage = storage_.data() + offset;
    const auto at0 = ringPosition(position, ring, baseLookback);
    const auto x0 = storage[at0];
    const auto x1 = baseLookback >= 0
        ? storage[olderPosition(at0, ring)]
        : storage[ringPosition(position, ring, baseLookback + 1)];
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
  [[nodiscard]] static Sample lagrange3(
      const Sample frac,
      const Sample p0,
      const Sample p1,
      const Sample p2,
      const Sample p3) noexcept {
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

  // Where a lookback lands in a ring whose next write goes to `position`.
  // A negative lookback wraps as its unsigned two's-complement value.
  [[nodiscard]] static std::uint64_t ringPosition(
      const std::uint64_t position,
      const std::uint64_t ring,
      const std::int64_t lookback) noexcept {
    const auto wrapped = static_cast<std::uint64_t>(lookback) % ring;
    return (position + ring - wrapped) % ring;
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
