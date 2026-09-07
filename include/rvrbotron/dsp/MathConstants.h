#pragma once

namespace rvrbotron::dsp {

// A single shared definition, since both the Damping shelf math
// (config::resolveHighShelfCoefficients et al.) and Modulation's `sine`
// trajectory need it and this codebase targets C++17 (no
// std::numbers::pi yet).
constexpr double kPi = 3.14159265358979323846;

} // namespace rvrbotron::dsp
