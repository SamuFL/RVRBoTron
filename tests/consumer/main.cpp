// Builds a Reverb from a typed ReverbConfig and processes one block, using
// only what a consumer links: rvrbotron::config and rvrbotron::dsp.

#include "rvrbotron/config/ResolveConfig.h"
#include "rvrbotron/config/ReverbConfig.h"
#include "rvrbotron/dsp/Reverb.h"
#include "rvrbotron/dsp/Sample.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
  using rvrbotron::dsp::Sample;

  rvrbotron::config::SplitConfig split;
  split.channels = 8;

  rvrbotron::config::DownmixConfig downmix;
  downmix.strategy = rvrbotron::dsp::DownmixStrategy::orthogonalRows;

  rvrbotron::config::CompositionConfig composition;
  composition.stagesSpecified = true;
  composition.stages.emplace_back(split);
  composition.stages.emplace_back(rvrbotron::config::DiffuserConfig{});
  composition.stages.emplace_back(rvrbotron::config::FeedbackLoopConfig{});
  composition.stages.emplace_back(downmix);
  composition.wetOnly = false;

  rvrbotron::config::ReverbConfig requested;
  requested.formatVersion = rvrbotron::config::kReverbConfigFormatVersion;
  requested.seed = 1;
  requested.composition = composition;

  constexpr std::uint32_t sampleRate = 48000;
  constexpr std::uint32_t inputChannels = 2;
  constexpr std::uint32_t outputChannels = 2;
  rvrbotron::dsp::Reverb reverb(
      rvrbotron::config::resolveConfig(requested, sampleRate, inputChannels));
  if (reverb.inputChannelCount() != inputChannels ||
      reverb.outputChannelCount() != outputChannels) {
    std::cerr << "unexpected channel counts\n";
    return 1;
  }

  constexpr std::size_t frames = 512;
  std::vector<std::vector<Sample>> input(
      inputChannels, std::vector<Sample>(frames, Sample{0}));
  std::vector<std::vector<Sample>> output(
      outputChannels, std::vector<Sample>(frames, Sample{0}));
  input[0][0] = Sample{1};
  input[1][0] = Sample{1};
  const Sample* inputs[]{input[0].data(), input[1].data()};
  Sample* outputs[]{output[0].data(), output[1].data()};

  reverb.process(inputs, inputChannels, outputs, outputChannels, frames);

  for (const auto& channel : output) {
    for (const Sample sample : channel) {
      if (!std::isfinite(sample)) {
        std::cerr << "non-finite output\n";
        return 1;
      }
    }
  }
  if (output[0][0] == Sample{0} && output[1][0] == Sample{0}) {
    std::cerr << "dry impulse missing from output\n";
    return 1;
  }
  return 0;
}
