#include "rvrbotron/dsp/Reverb.h"

#include <array>
#include <iostream>

int main() {
  const rvrbotron::dsp::ResolvedConfig config{
      1,
      0,
      48000,
      {},
  };
  rvrbotron::dsp::Reverb reverb(config);

  std::array<rvrbotron::dsp::Sample, 4> samples{
      0.5F,
      -0.25F,
      0.0F,
      1.0F,
  };
  const auto expected = samples;
  rvrbotron::dsp::Sample* channels[]{samples.data()};

  reverb.process(channels, 1, samples.size());

  if (samples != expected) {
    std::cerr << "empty Composition changed caller-owned samples\n";
    return 1;
  }

  return 0;
}
