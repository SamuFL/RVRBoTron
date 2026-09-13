#pragma once

#include "rvrbotron/dsp/Sample.h"

#include <cstddef>
#include <vector>

namespace rvrbotron::dsp {

void buildAllpassState(
    const std::vector<bool>& channelModulated,
    std::vector<Sample>& state,
    std::vector<std::size_t>& index);

} // namespace rvrbotron::dsp
