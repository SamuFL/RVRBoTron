#pragma once

namespace rvrbotron::cli {

// Parses `argv[2..]` as `benchmark` command options, constructs a Reverb
// from the given Resolved Configuration, and measures processing time and
// DSP-owned memory around Reverb::process, excluding configuration and
// file I/O. Prints a terminal summary and, if requested, writes a JSON
// report. Throws HarnessError on any failure.
void runBenchmark(int argc, char** argv);

} // namespace rvrbotron::cli
