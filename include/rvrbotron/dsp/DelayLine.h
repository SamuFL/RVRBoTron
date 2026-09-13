#pragma once

#include "rvrbotron/dsp/Sample.h"

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

  [[nodiscard]] Sample readFraction(
      const std::size_t channel,
      const double lookbackSamples) const noexcept {
    const auto ring = bufferSizes_[channel];
    const auto offset = offsets_[channel];
    const auto position = positions_[channel];
    const auto base = std::floor(lookbackSamples);
    const auto frac = static_cast<Sample>(lookbackSamples - base);
    const auto baseLookback = static_cast<std::int64_t>(base);
    const auto valueAt = [this, offset, ring, position](
                             const std::int64_t lookback) noexcept {
      const auto wrapped = static_cast<std::uint64_t>(lookback) % ring;
      const auto readPosition = (position + ring - wrapped) % ring;
      return storage_[offset + readPosition];
    };
    const auto p0 = valueAt(baseLookback - 1);
    const auto p1 = valueAt(baseLookback);
    const auto p2 = valueAt(baseLookback + 1);
    const auto p3 = valueAt(baseLookback + 2);
    // Cubic Lagrange interpolation through 4 equally spaced samples at
    // relative positions -1, 0, 1, 2, evaluated at `frac` in [0, 1]
    // between p1 (frac=0) and p2 (frac=1).
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

  [[nodiscard]] Sample readFractionLinear(
      const std::size_t channel,
      const double lookbackSamples) const noexcept {
    const auto ring = bufferSizes_[channel];
    const auto offset = offsets_[channel];
    const auto position = positions_[channel];
    const auto base = std::floor(lookbackSamples);
    const auto frac = static_cast<Sample>(lookbackSamples - base);
    const auto baseLookback = static_cast<std::int64_t>(base);
    const auto valueAt = [this, offset, ring, position](
                             const std::int64_t lookback) noexcept {
      const auto wrapped = static_cast<std::uint64_t>(lookback) % ring;
      const auto readPosition = (position + ring - wrapped) % ring;
      return storage_[offset + readPosition];
    };
    const auto p1 = valueAt(baseLookback);
    const auto p2 = valueAt(baseLookback + 1);
    return p1 + frac * (p2 - p1);
  }

  [[nodiscard]] Sample readFractionAllpass(
      const std::size_t channel,
      const double lookbackSamples,
      Sample& state) const noexcept {
    const auto ring = bufferSizes_[channel];
    const auto offset = offsets_[channel];
    const auto position = positions_[channel];
    const auto base = std::floor(lookbackSamples);
    const auto frac = static_cast<Sample>(lookbackSamples - base);
    const auto baseLookback = static_cast<std::int64_t>(base);
    const auto valueAt = [this, offset, ring, position](
                             const std::int64_t lookback) noexcept {
      const auto wrapped = static_cast<std::uint64_t>(lookback) % ring;
      const auto readPosition = (position + ring - wrapped) % ring;
      return storage_[offset + readPosition];
    };
    const auto x0 = valueAt(baseLookback);
    const auto x1 = valueAt(baseLookback + 1);
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
  std::vector<std::uint64_t> delays_;
  std::vector<std::uint64_t> bufferSizes_;
  std::vector<std::size_t> offsets_;
  std::vector<std::size_t> positions_;
  std::vector<Sample> storage_;
};

} // namespace rvrbotron::dsp
