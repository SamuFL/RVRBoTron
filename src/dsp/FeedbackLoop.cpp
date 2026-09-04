#include "rvrbotron/dsp/FeedbackLoop.h"

#include "rvrbotron/dsp/MixMatrix.h"
#include "rvrbotron/dsp/OwnedBytes.h"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace rvrbotron::dsp {
namespace {

// Explicit, portable denormal flush for the feedback write path (see
// docs/design/reverb/stages/04-feedback-loop.md and issue #54): a long
// decaying tail must not stall on denormals, but a CPU flush-to-zero mode
// would be platform-specific behaviour inside the DSP, in conflict with the
// cross-platform equivalence tolerances already committed (ADR-0001). A
// plain magnitude comparison against the smallest normal value is
// deterministic and identical on every supported platform instead. This is
// unconditional and independent of the still-dormant `silenceFloorDb` seam
// (see ResolvedConfig.h), which will one day raise the flush to an audible
// threshold rather than merely a numerically clean one.
constexpr Sample kDenormalFlushThreshold =
    std::numeric_limits<Sample>::min();

Sample flushDenormal(const Sample value) noexcept {
  return std::abs(value) < kDenormalFlushThreshold ? Sample{0} : value;
}

} // namespace

FeedbackLoop::FeedbackLoop(const ResolvedFeedbackLoop& config)
    : channels_(config.delaysSamples.size()),
      tailBudgetSamples_(config.tailBudgetSamples),
      blockSizeBoundSamples_(config.blockSizeBoundSamples),
      delays_(config.delaysSamples),
      delayOffsets_(channels_),
      delayPositions_(channels_, 0),
      mix_(nullptr),
      fedBack_(channels_) {
  if (config.bufferSizes.size() != channels_ ||
      config.gains.size() != channels_) {
    throw std::invalid_argument(
        "Feedback Loop requires one resolved buffer size and gain per "
        "Channel");
  }

  std::size_t totalDelayStorage = 0;
  for (std::size_t channel = 0; channel < channels_; ++channel) {
    delayOffsets_[channel] = totalDelayStorage;
    const auto delay = delays_[channel];
    const auto bufferSize = config.bufferSizes[channel];
    if (delay > std::numeric_limits<std::size_t>::max() ||
        bufferSize > std::numeric_limits<std::size_t>::max()) {
      throw std::length_error("Feedback Loop delay storage is too large");
    }
    if (bufferSize < delay) {
      throw std::invalid_argument(
          "Feedback Loop resolved buffer is shorter than its delay");
    }
    const auto storageSize = static_cast<std::size_t>(bufferSize);
    if (storageSize >
        std::numeric_limits<std::size_t>::max() - totalDelayStorage) {
      throw std::length_error("Feedback Loop delay storage is too large");
    }
    totalDelayStorage += storageSize;
  }
  delayStorage_.assign(totalDelayStorage, Sample{0});

  gains_.reserve(config.gains.size());
  for (const auto gain : config.gains) {
    if (!(gain > 0.0) || !(gain < 1.0)) {
      throw std::invalid_argument(
          "Feedback Loop gains must be strictly between zero and one");
    }
    gains_.push_back(static_cast<Sample>(gain));
  }
  mix_ = makeMixMatrix(config.mix, channels_, config.matrix);

  dampingEnabled_ = config.damping.has_value();
  if (dampingEnabled_) {
    const auto& damping = *config.damping;
    if (damping.highShelfGains.size() != channels_ ||
        damping.highShelfB0.size() != channels_ ||
        damping.highShelfB1.size() != channels_ ||
        damping.highShelfA1.size() != channels_ ||
        damping.lowShelfGains.size() != channels_ ||
        damping.lowShelfB0.size() != channels_ ||
        damping.lowShelfB1.size() != channels_ ||
        damping.lowShelfA1.size() != channels_) {
      throw std::invalid_argument(
          "Feedback Loop requires one resolved Damping coefficient set per "
          "Channel");
    }
    // A unity ratio bypasses its own section rather than processing it (see
    // docs/design/reverb/stages/05-damping.md's invariants): the
    // coefficients below do not simplify to identity under floating-point
    // rounding, so skipping the arithmetic entirely is what makes an
    // explicit unity Damping bit-identical to Damping disabled -- and each
    // section's bypass is independent, so a unity high ratio alongside an
    // active low ratio only skips the high shelf.
    const auto loadShelf =
        [this](
            const std::vector<double>& gains,
            const std::vector<double>& b0,
            const std::vector<double>& b1,
            const std::vector<double>& a1,
            std::vector<Sample>& targetB0,
            std::vector<Sample>& targetB1,
            std::vector<Sample>& targetA1,
            std::vector<Sample>& prevInput,
            std::vector<Sample>& prevOutput) {
          targetB0.reserve(channels_);
          targetB1.reserve(channels_);
          targetA1.reserve(channels_);
          for (std::size_t channel = 0; channel < channels_; ++channel) {
            const auto gain = gains[channel];
            const auto coefficient0 = b0[channel];
            const auto coefficient1 = b1[channel];
            const auto coefficientA = a1[channel];
            if (!std::isfinite(gain) || !(gain > 0.0) ||
                !std::isfinite(coefficient0) ||
                !std::isfinite(coefficient1) ||
                !std::isfinite(coefficientA)) {
              throw std::invalid_argument(
                  "Feedback Loop requires finite resolved Damping "
                  "coefficients");
            }
            const auto sampleA1 = static_cast<Sample>(coefficientA);
            // A one-pole section's own state is stable only while its pole
            // magnitude |a1| stays below one (#77); resolution never emits
            // such a value for this stage's canonical shelf coefficients,
            // so a violation here means deliberately unstable or corrupted
            // resolved data, rejected before it can ever process a sample.
            if (!(std::abs(sampleA1) < Sample{1})) {
              throw std::invalid_argument(
                  "Feedback Loop requires a stable resolved Damping shelf "
                  "pole (|a1| < 1)");
            }
            targetB0.push_back(static_cast<Sample>(coefficient0));
            targetB1.push_back(static_cast<Sample>(coefficient1));
            targetA1.push_back(sampleA1);
          }
          prevInput.assign(channels_, Sample{0});
          prevOutput.assign(channels_, Sample{0});
        };
    highShelfBypassed_ = damping.highRatio == 1.0;
    loadShelf(
        damping.highShelfGains,
        damping.highShelfB0,
        damping.highShelfB1,
        damping.highShelfA1,
        highShelfB0_,
        highShelfB1_,
        highShelfA1_,
        highShelfPrevInput_,
        highShelfPrevOutput_);
    lowShelfBypassed_ = damping.lowRatio == 1.0;
    loadShelf(
        damping.lowShelfGains,
        damping.lowShelfB0,
        damping.lowShelfB1,
        damping.lowShelfA1,
        lowShelfB0_,
        lowShelfB1_,
        lowShelfA1_,
        lowShelfPrevInput_,
        lowShelfPrevOutput_);
  }
}

FeedbackLoop::~FeedbackLoop() = default;

void FeedbackLoop::processFrame(const Sample* const inputs,
                                Sample* const outputs) noexcept {
  for (std::size_t channel = 0; channel < channels_; ++channel) {
    const auto delay = static_cast<std::size_t>(delays_[channel]);
    const auto delayed = delay == 0
                              ? Sample{0}
                              : delayStorage_
                                    [delayOffsets_[channel] +
                                     delayPositions_[channel]];
    outputs[channel] = delayed;
    fedBack_[channel] = delayed * gains_[channel];
  }

  // Two-shelf Damping runs after decay gain and before mixing, on every
  // circulation, high shelf then low shelf (see docs/design/reverb/stages/
  // 05-damping.md's "Structural note"); each section is skipped
  // independently when bypassed (unity ratio, or Damping disabled --
  // `highShelfBypassed_`/`lowShelfBypassed_` default true).
  if (!highShelfBypassed_) {
    for (std::size_t channel = 0; channel < channels_; ++channel) {
      const auto input = fedBack_[channel];
      const auto output = highShelfB0_[channel] * input +
          highShelfB1_[channel] * highShelfPrevInput_[channel] -
          highShelfA1_[channel] * highShelfPrevOutput_[channel];
      highShelfPrevInput_[channel] = input;
      highShelfPrevOutput_[channel] = output;
      fedBack_[channel] = output;
    }
  }
  if (!lowShelfBypassed_) {
    for (std::size_t channel = 0; channel < channels_; ++channel) {
      const auto input = fedBack_[channel];
      const auto output = lowShelfB0_[channel] * input +
          lowShelfB1_[channel] * lowShelfPrevInput_[channel] -
          lowShelfA1_[channel] * lowShelfPrevOutput_[channel];
      lowShelfPrevInput_[channel] = input;
      lowShelfPrevOutput_[channel] = output;
      fedBack_[channel] = output;
    }
  }

  mix_->mix(fedBack_.data());

  for (std::size_t channel = 0; channel < channels_; ++channel) {
    const auto delay = static_cast<std::size_t>(delays_[channel]);
    if (delay == 0) {
      continue;
    }
    const auto storageIndex =
        delayOffsets_[channel] + delayPositions_[channel];
    delayStorage_[storageIndex] =
        flushDenormal(inputs[channel] + fedBack_[channel]);
    delayPositions_[channel] = (delayPositions_[channel] + 1) % delay;
  }
}

std::size_t FeedbackLoop::channelCount() const noexcept {
  return channels_;
}

std::uint64_t FeedbackLoop::tailBudgetSamples() const noexcept {
  return tailBudgetSamples_;
}

std::uint64_t FeedbackLoop::blockSizeBoundSamples() const noexcept {
  return blockSizeBoundSamples_;
}

std::size_t FeedbackLoop::ownedBytes() const noexcept {
  return sizeof(*this) + ownedVectorBytes(delays_) +
         ownedVectorBytes(delayOffsets_) +
         ownedVectorBytes(delayPositions_) +
         ownedVectorBytes(delayStorage_) + ownedVectorBytes(gains_) +
         ownedVectorBytes(fedBack_) + mix_->ownedBytes() +
         ownedVectorBytes(highShelfB0_) + ownedVectorBytes(highShelfB1_) +
         ownedVectorBytes(highShelfA1_) +
         ownedVectorBytes(highShelfPrevInput_) +
         ownedVectorBytes(highShelfPrevOutput_) +
         ownedVectorBytes(lowShelfB0_) + ownedVectorBytes(lowShelfB1_) +
         ownedVectorBytes(lowShelfA1_) +
         ownedVectorBytes(lowShelfPrevInput_) +
         ownedVectorBytes(lowShelfPrevOutput_);
}

} // namespace rvrbotron::dsp
