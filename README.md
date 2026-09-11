# RVRBoTron

RVRBoTron is a research-grade algorithmic reverb laboratory. It is designed to make reverb structures easy to configure, render, measure, compare, and reproduce while developing a production-quality DSP core for a future audio plugin.

## Project documentation

- [Configure the Composition](docs/guides/configure-the-composition.md) -- every request field, with examples
- [Run experiments](docs/guides/run-experiments.md) -- catalogs, sweeps, and listening material
- [Reverb design overview](docs/design/reverb/README.md)
- [Stage specifications](docs/design/reverb/stages/)
- [Domain vocabulary](CONTEXT.md)
- [Engineering workflow](docs/agents/workflow.md)

Active implementation work is tracked in this repository's GitHub Issues.

## Quick start

Install CMake 3.25 or newer, Ninja, Python 3, and a C++17 compiler. Git LFS is
also required to download or add the curated listening samples. Rendering and
`tools/analyze_render.py` need only the standard library; the diffusion,
tail, and Early Tap support analyzers additionally need the packages in
`tools/requirements.txt` (`pip3 install -r tools/requirements.txt`).

### Configure, build, and test

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
```

The `double` preset runs the same suite with float64 DSP samples:

```bash
cmake --preset double
cmake --build --preset double
ctest --preset double
```

The `release` and `release-double` presets build optimized (`Release`)
binaries for meaningful `benchmark` timing evidence; `default`/`double` stay
Debug builds for development:

```bash
cmake --preset release
cmake --build --preset release
```

Every CI job runs the complete command-level suite and proves exact identity
from its own committed fixtures; artifacts are not transferred between
machines. Float builds run on macOS arm64, macOS Intel, and Windows x86_64.
Representative float64 builds run on macOS arm64 and Windows x86_64. CI runs
for pull requests targeting `develop` or `main` and for pushes to `main`.
The committed compatibility fixtures cover mono and stereo PCM16, PCM24,
PCM32, IEEE float32, and IEEE float64 WAV at 44.1, 48, and 96 kHz.

Regenerate those deterministic technical fixtures with:

```bash
python3 tests/fixtures/generate_wav_matrix.py
```

### Render a Requested Configuration

Create `request.json`:

```json
{
  "formatVersion": 2,
  "seed": 0,
  "composition": {"stages": []}
}
```

Render a committed technical fixture through the empty Composition:

```bash
build/default/rvrbotron render \
  --input tests/fixtures/audio/matrix/identity-stereo-48000-pcm16.wav \
  --config request.json \
  --block-size 512 \
  --output build/requested-result
```

The immutable Render Result contains:

| Path | Contents |
| --- | --- |
| `output.wav` | Canonical IEEE float32 WAV, or float64 with the `double` preset |
| `request.json` | Exact bytes of the user-authored request; present for requested renders |
| `resolved.json` | Complete Resolved Configuration used by the DSP |
| `render.json` | Input provenance, renderer facts, selected precision, block size, and audio facts |
| `captures/` | Optional manifested Stage captures from `--capture-stages all`: N-Channel Split/Diffusion-Step captures, plus the stereo Main-stereo (and, when configured, Early-stereo) branch captures (issue #113) |
| `analysis/` | Append-only, versioned analysis artifacts added after rendering |

`render.json` records the input filename and SHA-256, renderer version,
platform, architecture, sample precision, block size, configuration input
mode, sample rate, output channel count, input frame count, output frame
count, the resolved Pre-delay authorised before Split (`preDelayFrames`,
0 for the empty identity Composition or zero Pre-delay, issue #133), and
the resolved Tail budget authorised for draining past input EOF
(`tailBudgetFrames`, 0 for a Composition with no Diffuser or Feedback
Loop -- kept separate from `preDelayFrames` so onset delay is never
confused with decay duration). It intentionally contains no timestamp,
host or user identity, full input path, or source-tree fingerprint.

Rendering builds the evidence in a temporary sibling and publishes it
atomically. Choose a fresh output path for every render because an existing
destination is never overwritten.

### Rerender the Resolved Configuration

Use the emitted configuration without resolving the request again:

```bash
build/default/rvrbotron render \
  --input tests/fixtures/audio/matrix/identity-stereo-48000-pcm16.wav \
  --resolved build/requested-result/resolved.json \
  --output build/resolved-result
```

### Render the first Reference Diffusion Step

Format version 2 also accepts the ordered `[split, diffuser, downmix]` and
`[split, feedback-loop, downmix]` Composition shapes (every shape and field is
documented in [Configure the
Composition](docs/guides/configure-the-composition.md)).
The Diffuser resolves to an ordered chain of Hadamard Diffusion
Steps (`4` by default) feeding a select Downmix. Diagnostic ablations
support `normalisation: "none"`, `delayStrategy: "even"` or
`"uniform-random"`, `shuffle: false`, and `polarity: "none"`:

```json
{
  "formatVersion": 2,
  "seed": 42,
  "composition": {
    "stages": [
      {
        "type": "split",
        "channels": 8,
        "strategy": "duplicate",
        "normalisation": "energy"
      },
      {
        "type": "diffuser",
        "steps": 1,
        "totalMs": 40,
        "distribution": "even",
        "step": {
          "delayStrategy": "segmented-random",
          "mix": "hadamard",
          "shuffle": true,
          "polarity": "seeded-random"
        }
      },
      {
        "type": "downmix",
        "strategy": "select",
        "leftChannel": 0,
        "rightChannel": 1,
        "normalisation": "energy"
      }
    ]
  }
}
```

Non-empty diffusion renders are stereo and wet-only. The renderer drains
silence for the resolved finite Diffuser budget, so `render.json` distinguishes
`inputFrames` from the longer `frames`.

Capture the N-Channel Split and cumulative Diffusion Step without changing
`output.wav`:

```bash
build/default/rvrbotron render \
  --input tests/fixtures/audio/impulse-mono-pcm16-48000.wav \
  --config request.json \
  --capture-stages all \
  --output build/diffusion-result
```

The versioned `all-v2` capture profile writes canonical WAVs under
`captures/`. `render.json` manifests each boundary with its stable path,
SHA-256, sample rate, Channel count, and complete output-timeline frame count.

### Configure the Composition

Every request field, its accepted values, defaults, the valid stage shapes, and
worked examples are in
[Configure the Composition](docs/guides/configure-the-composition.md).


### Capturing and ablating the Main and Early branches

`--capture-stages all` (issue #113) additionally publishes each branch's own
stereo contribution, captured after its own shaping, Downmix, Width, and
level -- immediately before the two are summed into `output.wav`:

| Path | Boundary | Present when |
| --- | --- | --- |
| `captures/02-main-stereo.wav` | `main-stereo` | Always, whenever `composition.stages` is non-empty |
| `captures/03-early-stereo.wav` | `early-stereo` | Only when `composition.early` is configured (with at least one tap) |

Each entry in `render.json`'s `stageCaptures` records a `disabled` flag: `true`
only for a branch that is configured but disabled (`mainEnabled: false`, or
`early.enabled: false`) -- its capture is still written, correctly sized to
the render's own output timeline, but exact zero throughout, since a
disabled branch contributes exact zero rather than a zero-multiplied value.
`disabled` is always `false` for `split`/`diffusion-step` captures, which
have no enablement of their own. A Feedback-Loop-only Main wet path (no
Diffuser) still captures `main-stereo`; it has no Diffusion Steps to capture
and cannot carry an Early Reflections branch, since Early requires a
Diffuser.

`main-stereo` plus (when present) `early-stereo` is the **Wet sum**. With the
default envelope (`wetOnly: true`, 0 dB wet) that is exactly `output.wav`;
with a configured envelope, `output.wav` is the dry contribution plus the
Wet sum scaled by `wetGain` (issue #131). Either way each branch's own
measured energy plus their cross term reconciles with the Wet sum's own
measured energy -- overlapping signals are not simply additive in energy,
since `sum(a+b)^2 != sum(a^2) + sum(b^2)` unless the two are uncorrelated.

### Analyze the Render Result

Render Result analysis is deterministic: it reads only the immutable Render
Result and never touches the clock, the machine, or system load, so
analyzing the same evidence on any machine reproduces the same published
artifact. It is a separate Python command and is never required for
rendering:

```bash
python3 tools/analyze_render.py \
  build/requested-result \
  --source tests/fixtures/audio/matrix/identity-stereo-48000-pcm16.wav
```

It prints a concise summary and atomically adds
`analysis/baseline-v1.json`. The versioned artifact records frame count,
duration, sample rate, channel count, and non-finite sample count, plus
per-channel and combined peak absolute sample, RMS amplitude, and sum of
squares. Metric definitions and linear units are embedded in the artifact.

The optional source comparison runs only when the source SHA-256 matches
`render.json`. It reports exact equality, differing-sample count, maximum
absolute error, and the first mismatch. Analysis artifacts are append-only:
repeating identical analysis succeeds without rewriting, while different
content for the same analyzer version is rejected.

For a captured finite Diffuser, add the separate diffusion artifact:

```bash
python3 tools/analyze_diffusion.py \
  build/diffusion-result \
  --source tests/fixtures/audio/impulse-mono-pcm16-48000.wav
```

`analysis/diffusion-v1.json` verifies source and Stage-capture provenance,
requires `inputFrames + preDelayFrames + resolved diffuser.totalSamples`
frames in the output and every capture, measures the actual captured Split
and cumulative Diffusion
Step energies (including each step's relative error from Split), and measures
orthogonality from every serialized resolved matrix (Hadamard, Householder, or
RandomOrthogonal alike). Python does not reconstruct Split mapping or DSP
sample precision. Publication is append-only and idempotent.

The artifact also reports, using NumPy: a signed Correlation matrix at Split
and every Diffusion Step; an Alignment score (pairwise Jaccard overlap of
active-frame sets, -120 dB capture-relative activity floor) and 10 ms
Distinct-arrival density curve measured on the complete Diffuser output,
beside the theoretical N^k Echo-path count; and Coloration (unwindowed,
next-power-of-two FFT peak-to-peak/RMS dB deviation, spectral flatness, and a
compact 1/12-octave curve) for both the combined N-Channel signal and the
diagnostic stereo `output.wav`, reported separately since only the former
carries the all-pass claim.

Pass `--compare <other-render-result>` instead of `--source` to check two
Render Results of the same Resolved Configuration -- typically rendered at
different `--block-size` values -- for exact decoded equality of `output.wav`
and every Stage capture, with first-mismatch detail on failure. This mode
prints its own JSON report and does not publish an artifact.

For a Composition with an Early Reflections branch (issue #112), render with
`--capture-stages all` and add the Tap support artifact:

```bash
python3 tools/analyze_early_support.py build/early-result
```

`analysis/early-support-v1.json` reports, per resolved tap, the measured
first/last non-zero sample, peak sample, and energy-weighted centroid from
that tap's own captured Diffusion Step (-120 dB capture-relative activity
floor, the same convention `analyze_diffusion.py`'s own Alignment evidence
uses), alongside its resolved nominal and conservative Tap support bounds
and whether the measured interval fell within conservative support.
Cancellation can make measured support narrower than the structural bound;
it never rejects a render for falling outside it. Publication is
append-only and idempotent, like every other analyzer here.

For any Composition's Main and (if configured) Early Downmix, render with
`--capture-stages all` and add the spatial-output Downmix artifact (issue
#115) -- it works across every Downmix strategy and either Alignment
expectation, with or without a Feedback Loop:

```bash
python3 tools/analyze_downmix.py build/downmix-result --source tests/fixtures/audio/impulse-mono-pcm16-48000.wav
```

`analysis/downmix-v1.json` reports the Main branch's (and, when
configured, the Early branch's) resolved strategy, Alignment expectation,
and Coherent Downmix ablation tag (`coherentDownmixAblation`, issue #114)
read directly from `resolved.json` rather than re-derived; a measured
Alignment score of that Downmix's own immediate N-Channel source, only
when a Diffusion Step actually is that immediate input -- an unaligned
source (a Feedback Loop between the Diffuser and Downmix, or no Diffuser
at all) has no equivalent capture (ADR-0005), so this is reported
unavailable there rather than measured against the wrong signal. When
that source is available *and* the branch itself was enabled (issue
#113's `disabled` capture flag), it also reports a `branchEnergyRatio` --
the branch's own captured energy relative to that source's, and
`analyze_tail.py`'s own established raised-cosine octave-band filter's
spectral deviation between them -- deliberately reported as the
*combined* effect of the Downmix's row/compensation projection, Width,
and branch level together, not an isolated Width metric, since no
capture exists between Downmix and Width to separate them. A damped tail
never receives an "absolute flatness" spectral claim from this analyzer
as a result of the same Alignment-score gating. It also reports Output
correlation and inter-channel level difference on the combined stereo
output, branch energies and their cross term reconciled against combined
energy (captured immediately before summation, issue #113; reconstructed
at the render's own sample precision, not a higher-precision
approximation, since the renderer's own summation and Early's own
per-tap accumulation happen in that same precision), a peak factor, and
equal-power mono fold-down (energy loss and octave-band spectral
deviation between the folded signal and the stereo pair it was folded
from) -- reusing that same octave-band filter for every spectral
comparison here, rather than a fresh per-bin binning
scheme, so deviation is a stable quantity independent of FFT length.
None of this evidence imposes an acoustic rejection threshold. Publication
is append-only and idempotent.

For a Composition containing a Feedback Loop, add the separate tail
artifact instead -- the diffusion analyzer's all-pass, feedback-free
assumptions do not hold once the tail is present, so it is deliberately not
extended to cover this case. Schroeder backward integration assumes an
impulse response, so analyze a deterministic impulse render of the same
Resolved Configuration rather than a musical sample -- issue #59's sweep
keeps the two renders separate for exactly this reason:

```bash
build/default/rvrbotron render \
  --input tests/fixtures/audio/impulse-stereo-left-pcm16-48000.wav \
  --resolved build/tail-result/resolved.json \
  --output build/tail-impulse-result

python3 tools/analyze_tail.py \
  build/tail-impulse-result \
  --source tests/fixtures/audio/impulse-stereo-left-pcm16-48000.wav
```

Rerendering via `--resolved` requires the new source's Channel count to
match exactly what the first render resolved (the resolved Split's
`inputChannels`), so the impulse fixture here must be stereo -- matching
`samples/listening/PianoDry.wav` above -- not the mono fixture used
elsewhere in this document.

`analysis/tail-v1.json` requires the resolved Composition to be
`[split, feedback-loop, downmix]` or `[split, diffuser, feedback-loop,
downmix]`; any other shape is rejected. It verifies source provenance from
`render.json` and checks the output frame count against
`inputFrames + preDelayFrames + tailBudgetFrames` -- exactly, while the
dormant `silenceFloorDb` seam is disabled, relaxing to an upper bound
once it is enabled, keyed off the Resolved Configuration rather than a
schema change. Measurement reads only `output.wav`; no Stage captures
are required.

RT60 is measured per octave band from 63 Hz to 16 kHz by Schroeder backward
integration of the band-limited stereo output's energy (a raised-cosine
band shape and zero-padded FFT filtering keep the filter a linear rather
than a circular convolution, avoiding leakage across a finite-length
render). T30 (fit over -5 to -35 dB, extrapolated to -60 dB) is the primary
fit; T20 (-5 to -25 dB) is reported beside it -- the two agree on a linear,
single-rate decay and diverge under `gainMode: uniform` at a wide delay
spread, exposing the non-linear, double-sloped decay that mode produces
rather than averaging it away. The Reference-band (1 kHz) T30 estimate is
reported against the requested `rt60Sec` with relative error and whether it
sits within the +/-5% Decay accuracy invariant.

The artifact separately reports the output's non-finite sample count and a
decay-envelope check: the *raw*, non-Schroeder-integrated broadband output
energy, coarsely windowed from input EOF onward, verified non-increasing.
This is deliberately distinct from the per-band Schroeder curves above,
which are non-increasing by construction regardless of what the render
does and so cannot themselves catch a genuine buildup.

The artifact also reports, reusing `analyze_diffusion.py`'s own evidence
functions for direct comparability: an Alignment score (near unity for an
aligned Diffuser-only render's internal Channels, materially lower once the
Feedback Loop circulates and unaligns them, measured here on the stereo
output's left and right positions) and Coloration for the stereo output.
Publication is append-only and idempotent.

### Compare Diffusion Equivalence Across Platforms

Cross-platform equivalence (Repeat determinism holds only on one machine;
see [ADR-0001](docs/adr/0001-cross-platform-reproducibility.md)) is proven
by reducing an already-analyzed Render Result's `analysis/diffusion-v1.json`
to a compact, WAV-free summary and comparing it against other platforms'
summaries within committed tolerances:

```bash
python3 tools/extract_diffusion_equivalence.py \
  build/diffusion-result \
  --output build/diffusion-equivalence.json

python3 tools/compare_diffusion_equivalence.py \
  build/diffusion-equivalence.json other-platform/diffusion-equivalence.json \
  --tolerances tools/diffusion_tolerances_v1.json
```

The extracted summary keeps energy relative error, orthogonality error,
Correlation, Alignment score, Coloration, structural Echo paths, and
measured Distinct-arrival evidence; it drops full Correlation matrices,
per-pair Alignment listings, and per-band Coloration curves. The comparator
groups summaries by sample precision, picks one baseline platform per group
(macOS arm64 when present), and requires the structural N^k Echo-path count
and Distinct-arrival density bin count -- deterministic functions of the
Resolved Configuration, not measurements -- to match exactly, while every
measured metric (including the measured Distinct-arrival counts
themselves) stays within `tools/diffusion_tolerances_v1.json`'s committed
per-precision absolute tolerance; a group with fewer than two platforms is
reported as skipped, not failed. CI runs both platform-independent steps
against the same compact Reference impulse tracer used by
`diffusion_analyzer_contract`, uploads each platform/precision's compact
summary as a build artifact, and runs a final job that downloads every
summary and compares them by precision.

### Compare Tail Equivalence Across Platforms

Tail measurement's octave-band FFT filtering, Schroeder integration, and
curve fitting all run in NumPy, so the same workflow applies to
`analysis/tail-v1.json`:

```bash
python3 tools/extract_tail_equivalence.py \
  build/tail-impulse-result \
  --output build/tail-equivalence.json

python3 tools/compare_tail_equivalence.py \
  build/tail-equivalence.json other-platform/tail-equivalence.json \
  --tolerances tools/tail_tolerances_v1.json
```

The extracted summary keeps non-finite sample count, each octave band's
T20/T30 RT60 estimate, Alignment score, and Coloration; it drops the full
per-segment decay-envelope energy series and Coloration's twelfth-octave
curve. The comparator groups summaries by sample precision, picks one
baseline platform per group (macOS arm64 when present), and requires the
octave-band count, whether each band's T20/T30 fit exists at all, and the
decay-envelope monotonicity flag -- discrete facts about the committed CI
tracer's decay curve, not continuous measurements -- to match exactly,
while every measured metric (including each band's own RT60 estimate)
stays within `tools/tail_tolerances_v1.json`'s committed per-precision
absolute tolerance; a group with fewer than two platforms is reported as
skipped, not failed, and a candidate platform missing a metric the
baseline reports (for example, a differing band count) fails that metric
explicitly rather than aborting the comparison. CI runs both
platform-independent steps against the same millisecond-scale Feedback
Loop tracer used by `tail_analyzer_contract`, uploads each
platform/precision's compact summary as a build artifact, and runs a final
job that downloads every summary and compares them by precision.

Per [ADR-0001](docs/adr/0001-cross-platform-reproducibility.md), tolerances
are only committed after reviewing real evidence from every supported CI
architecture; see that document for the CI run reviewed and each metric's
observed delta and headroom.

### Benchmark a Resolved Configuration

Benchmark evidence is environment-qualified rather than deterministic: it
depends on the specific machine, build configuration, and system load at
measurement time, so it is never asserted equal across machines or CI runs
-- CI benchmarks are smoke coverage only (the report has a sane shape and
completes), with no performance gate or cross-machine speed assertion.
Measure empirical CPU and memory cost around `Reverb::process`, excluding
configuration and file I/O, using an optimized build (`--preset release` or
`--preset release-double`):

```bash
build/release/rvrbotron benchmark \
  --resolved build/requested-result/resolved.json \
  --block-size 128 \
  --warmup-seconds 1 \
  --measure-seconds 5 \
  --json build/benchmark-report.json
```

The Composition must be non-empty and its Channel counts mono or stereo.
Deterministic nonzero seeded blocks drive the constructed `Reverb` for the
warm-up duration (discarded), then for the measured duration, timing every
block individually; neither phase touches file or JSON I/O. The terminal and
JSON reports are identical and include: median, p95, and worst block time;
the real-time budget, median/worst utilization, and missed-deadline count at
the resolved sample rate and requested block size; exact DSP-owned bytes
(object storage plus every owned-container's actual capacity, not size); a
best-effort process resident-set-size delta, labeled separately as
allocator/runtime-noisy evidence; and provenance -- renderer version,
platform, architecture, compiler, build type, Sample precision, sample rate,
block size, Channel count, step count, the mixing matrix used by every
Diffusion Step, and -- per Diffusion Step and for the Feedback Loop --
the resolved Modulation interpolation method actually exercised (`null`
when that stage's Modulation is absent or inactive), so two reports from
different `interpolation` choices stay directly comparable (#92).
Benchmarking a Debug binary prints a prominent warning to stderr but is
not refused.

### Run experiments

Experiment catalogs and sweeps -- diffusion, tail, damping, modulation, and
spatial output -- plus the curated listening material they run against, live
in [Run experiments](docs/guides/run-experiments.md).


## Diagnostics

Failures are written to stderr with stable exit categories:

| Exit | Category |
| ---: | --- |
| 2 | `malformed_json` |
| 3 | `unsupported_audio` |
| 4 | `invalid_configuration` |
| 5 | `io_failure` |
| 6 | `internal_processing_failure` |
| 7 | `invalid_arguments` |

Pass `--error-format json` to receive the same category, reason, and optional
configuration location as one JSON object.
