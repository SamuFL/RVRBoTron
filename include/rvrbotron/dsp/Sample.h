#pragma once

namespace rvrbotron::dsp {

#if defined(RVRBOTRON_SAMPLE_DOUBLE)
using Sample = double;
#else
using Sample = float;
#endif

} // namespace rvrbotron::dsp
