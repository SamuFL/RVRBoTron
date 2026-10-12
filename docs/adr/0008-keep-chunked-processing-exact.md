# Keep chunked processing exact

`Reverb::process` runs its stages over planar chunks of up to
`kMaxChunkFrames` frames instead of one frame at a time (issue #163), which
made offline rendering about six times faster. A chunk's length follows the
caller's block, so the chunked code must give the same bits for every chunk
length, or Repeat determinism and block-size independence would quietly
depend on how a host happens to call `process`. We decided that chunking may
change loop order, index arithmetic, and where samples live, but never the
expression any sample is computed by.

Floating-point contraction is where that rule is easy to break. ADR-0001
keeps each toolchain's default contraction, and the default
(`-ffp-contract=on` on Clang) only fuses a multiply and an add written in
the same expression. Whether such an expression actually becomes a fused
instruction still depends on the loop around it: v1.0.0's vectorised Downmix
and RandomOrthogonal loops summed their products unfused, then fused the
remainder Channels that did not fill a vector. An element-wise loop over
frames fuses the same way whatever its trip count, but a reduction over
Channels does not. So every product that feeds a running sum (`left += x *
w`) is written as its own statement, which no supported toolchain fuses.
That keeps the sum chunk-invariant and also matches v1.0.0 wherever its
vectorised loop covered the Channels: in float32 builds, Cloud and every
Composition whose dense mixes have a multiple of four Channels render
byte-identically to v1.0.0. Other Channel counts, and float64 builds, whose
vectors hold fewer samples, can differ from v1.0.0 by about one ulp in those
mixes, where v1.0.0's fusing was an accident of vector width rather than a
decision. Toolchains that never contract, such as MSVC on x64, are
unaffected.

The chunk is one frame while a `StageCaptureSink` is attached, so captures
keep their frame-by-frame order; owned memory does not depend on capturing.
The Feedback Loop reads a whole chunk before writing it, so its chunk stays
below the Block-size bound. `tests/test_reverb.cpp` renders two Compositions
that reach every chunked path at several block sizes and requires the bits
of a frame-by-frame render.
