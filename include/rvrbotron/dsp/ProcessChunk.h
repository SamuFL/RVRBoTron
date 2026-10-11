#pragma once

#include <cstddef>

namespace rvrbotron::dsp {

// Reverb::process runs its stages over chunks of at most this many frames,
// held planar: Channel c of a chunk of n frames starts at c * n. Every
// stage is exact under any chunking, so the chunk length changes speed,
// never output (issue #163). Distinct from the Block-size bound, which
// limits the caller's block.
constexpr std::size_t kMaxChunkFrames = 128;

} // namespace rvrbotron::dsp
