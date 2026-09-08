#pragma once

#include "rvrbotron/dsp/Sample.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rvrbotron::dsp {

// A per-Channel circular delay buffer shared by every stage that circulates
// audio through a resolved delay: the Feedback Loop and Diffusion Steps
// (see docs/design/reverb/stages/06-modulation.md, issue #88). One
// instance holds every Channel's delay line in a single contiguous
// allocation, exactly mirroring the layout each stage previously
// duplicated on its own.
//
// DelayLine owns storage, wrap-around indexing, and the integer read/write
// path. It does not own read-before-write ordering: a stage that must read
// every Channel before writing any of them (the Feedback Loop) keeps that
// sequencing explicit in its own frame loop -- calling read() across every
// Channel, and only afterward calling write() across every Channel --
// rather than this class hiding it behind one combined operation.
//
// Constructed with an already-resolved length; it never derives its own
// sizing. Resolution is the only place that knows why a buffer needs to be
// as large as it is (recorded evidence, and a rejection reason at load),
// so this class only enforces the invariant a buffer must satisfy to be
// used safely: never shorter than the delay it serves.
class DelayLine {
public:
  DelayLine(
      std::vector<std::uint64_t> delaysSamples,
      std::vector<std::uint64_t> bufferSizes);

  // The value written exactly delaySamples(channel) frames ago; does not
  // advance anything. Only valid when delaySamples(channel) > 0.
  //
  // Computed from this Channel's own delay rather than by directly
  // indexing the write position, so this stays correct even for a
  // Channel whose resolved buffer reserves headroom beyond its delay
  // (Modulation's Excursion and Interpolation margin). A branch and a
  // subtraction, not a runtime division: the constructor's own invariant
  // (buffer size >= delay) guarantees `position - delay` wraps around at
  // most once, so a single comparison covers it exactly, keeping this
  // path as cheap as a bare index for the common case of no headroom
  // (buffer size == delay), where the wrap never triggers at all.
  [[nodiscard]] Sample read(const std::size_t channel) const noexcept {
    const auto ring = bufferSizes_[channel];
    const auto delay = delays_[channel];
    const auto position = positions_[channel];
    const auto readPosition =
        position >= delay ? position - delay : ring - (delay - position);
    return storage_[offsets_[channel] + readPosition];
  }

  // Writes into the slot "now" -- the position this Channel's cursor
  // currently occupies -- then advances that Channel's position by one,
  // wrapping at its resolved buffer size. Only valid when
  // delaySamples(channel) > 0.
  //
  // Wrapping at the resolved buffer size rather than the delay itself is
  // what lets readFraction() below serve a moving lookback anywhere
  // within that headroom: for a Channel with no Modulation, resolution
  // sizes the buffer to exactly its delay (see docs/design/reverb/stages/
  // 06-modulation.md's "Delay buffers need headroom"), so this is
  // numerically identical to wrapping at the delay, as it always has.
  void write(const std::size_t channel, const Sample value) noexcept {
    const auto index = offsets_[channel] + positions_[channel];
    storage_[index] = value;
    const auto ring = static_cast<std::size_t>(bufferSizes_[channel]);
    positions_[channel] = (positions_[channel] + 1) % ring;
  }

  // Third-order Lagrange interpolated read at an arbitrary, possibly
  // fractional, lookback from "now" -- the position write() will next
  // occupy (see docs/design/reverb/stages/06-modulation.md's "Fractional
  // delay becomes mandatory"). `lookbackSamples` must be finite and, once
  // its four-point stencil (lookback floor()-1 through floor()+2) is
  // accounted for, stay within [1, this Channel's resolved buffer size]:
  // Modulation's resolution rejects any configuration that would not
  // (see the design doc's "The Block-size bound becomes
  // modulation-aware"), so this never wraps into not-yet-written data.
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

  // Linear interpolated read at an arbitrary, possibly fractional,
  // lookback from "now" -- the deliberate ablation alongside
  // readFraction()'s third-order Lagrange (see docs/design/reverb/
  // stages/06-modulation.md's "Fractional delay becomes mandatory" and
  // issue #92): linear interpolation is a lowpass whose cutoff depends
  // on the fractional read position, so inside a circulating loop it
  // acts as unintended, depth-dependent damping -- made available on
  // purpose, to be heard and later measured, rather than hidden.
  // `lookbackSamples` must be finite and, once its two-point stencil
  // (lookback floor() and floor()+1) is accounted for, stay within
  // [0, this Channel's resolved buffer size]: strictly less demanding
  // than readFraction()'s own four-point stencil, so the same resolved
  // buffer -- sized for Lagrange3's worst case -- always has enough
  // headroom for this method too (see "Delay buffers need headroom").
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

  [[nodiscard]] std::uint64_t delaySamples(
      const std::size_t channel) const noexcept {
    return delays_[channel];
  }

  // The backing-vector allocations only. A containing stage owns this
  // object's in-place storage through its own sizeof(*this).
  [[nodiscard]] std::size_t ownedStorageBytes() const noexcept;

private:
  std::vector<std::uint64_t> delays_;
  // Each Channel's resolved buffer size, exactly as passed to the
  // constructor: write()'s wrap-around modulus and readFraction()'s
  // lookback horizon. Equal to `delays_[channel]` for a Channel with no
  // Modulation; larger once Modulation reserves headroom beyond it.
  std::vector<std::uint64_t> bufferSizes_;
  std::vector<std::size_t> offsets_;
  std::vector<std::size_t> positions_;
  std::vector<Sample> storage_;
};

} // namespace rvrbotron::dsp
