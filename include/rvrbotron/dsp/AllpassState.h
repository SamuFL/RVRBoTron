#pragma once

#include "rvrbotron/dsp/Sample.h"

#include <cstddef>
#include <vector>

namespace rvrbotron::dsp {

// Builds the compact per-Channel allpass interpolator state a Modulation-
// bearing stage owns when `allpass` is its chosen interpolation (see
// docs/design/reverb/stages/06-modulation.md and issue #93):
// DelayLine::readFractionAllpass needs one persistent output sample per
// Channel actually modulated, so `state` is sized to exactly that count --
// not the full Channel count -- so a Channel `channelFraction` excludes
// allocates none of it at all. `index[channel]` is that Channel's own
// position within `state`; meaningful, and only ever read, for a Channel
// `channelModulated` marks true, since every read site is already guarded
// by the same check. Shared by FeedbackLoop and DiffusionStep, whose own
// construction is otherwise structurally identical for this one piece of
// bookkeeping, so the two can never drift out of sync with each other or
// with `channelModulated`'s own indexing.
void buildAllpassState(
    const std::vector<bool>& channelModulated,
    std::vector<Sample>& state,
    std::vector<std::size_t>& index);

} // namespace rvrbotron::dsp
