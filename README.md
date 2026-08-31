# RVRBoTron

RVRBoTron is a research-grade algorithmic reverb laboratory. It is designed to make reverb structures easy to configure, render, measure, compare, and reproduce while developing a production-quality DSP core for a future audio plugin.

## Project documentation

- [Reverb design overview](docs/design/reverb/README.md)
- [Stage specifications](docs/design/reverb/stages/)
- [Domain vocabulary](CONTEXT.md)
- [Engineering workflow](docs/agents/workflow.md)

Active implementation work is tracked in this repository's GitHub Issues.

## Quick start

Install CMake 3.25 or newer, Ninja, Python 3, and a C++17 compiler. Git LFS is
also required to download or add the curated listening samples. Rendering and
`tools/analyze_render.py` need only the standard library; the diffusion
analyzer additionally needs the packages in `tools/requirements.txt`
(`pip3 install -r tools/requirements.txt`).

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
  "formatVersion": 1,
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
| `captures/` | Optional manifested N-Channel Stage captures from `--capture-stages all` |
| `analysis/` | Append-only, versioned analysis artifacts added after rendering |

`render.json` records the input filename and SHA-256, renderer version,
platform, architecture, sample precision, block size, configuration input
mode, sample rate, output channel count, input frame count, output frame
count, and the resolved Tail budget authorised for draining past input EOF
(0 for a Composition with no Diffuser or Feedback Loop). It intentionally
contains no timestamp, host or user identity, full input path, or
source-tree fingerprint.

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

Format version 1 also accepts the ordered `[split, diffuser, downmix]` and
`[split, feedback-loop, downmix]` Composition shapes (the latter documented
in [Sustain a Response with a Feedback Loop](#sustain-a-response-with-a-feedback-loop)
below). The Diffuser resolves to an ordered chain of Hadamard Diffusion
Steps (`4` by default) feeding a select Downmix. Diagnostic ablations
support `normalisation: "none"`, `delayStrategy: "even"` or
`"uniform-random"`, `shuffle: false`, and `polarity: "none"`:

```json
{
  "formatVersion": 1,
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

The versioned `all-v1` capture profile writes canonical WAVs under
`captures/`. `render.json` manifests each boundary with its stable path,
SHA-256, sample rate, Channel count, and complete output-timeline frame count.

### Experiment with a Diffuser Configuration

Every field below is optional; the resolver substitutes the listed default
for anything omitted. Unknown fields anywhere in the request are rejected.
Render a curated listening sample (see
[Validate a Listening Sample Locally](#validate-a-listening-sample-locally))
through a modified `request.json` to hear the effect of each change:

```bash
build/default/rvrbotron render \
  --input samples/listening/PianoDry.wav \
  --config request.json \
  --output build/listening-diffusion-result
```

#### Top level

| Field | Type | Default | Notes |
| --- | --- | --- | --- |
| `formatVersion` | integer | `1` | Only `1` is supported. |
| `seed` | unsigned 64-bit integer | `0` | Drives every seeded-random derivation (delays, shuffle, polarity). |
| `composition.stages` | array | `[]` (empty Composition, exact identity) | When present, must be exactly `[split, diffuser, downmix]`. |

#### `split` stage

| Field | Values | Default |
| --- | --- | --- |
| `channels` | unsigned 32-bit integer (N) | `8` |
| `strategy` | `"duplicate"` \| `"stereo-halves"` \| `"stereo-interleave"` | `"duplicate"` |
| `normalisation` | `"energy"` \| `"none"` | `"energy"` |

`"duplicate"` sums stereo input to `(L + R)/√2` and distributes it to every
Channel; it discards stereo position. `"stereo-halves"` and
`"stereo-interleave"` preserve L/R independently and require an even
Channel count when the source is stereo; `"stereo-halves"` feeds the first
half of the Channels from the left input and the second half from the
right, and `"stereo-interleave"` alternates left/right by Channel index.
For mono input, every strategy resolves to the same mono duplication
mapping.

#### `diffuser` stage

| Field | Values | Default | Notes |
| --- | --- | --- | --- |
| `steps` | unsigned 32-bit integer (N) | `4` | Number of ordered Diffusion Steps sharing `totalMs`/`distribution`. Mutually exclusive with `lengthsMs`. |
| `totalMs` | finite number | `300` | Combined length of every Diffusion Step, apportioned per `distribution`. Mutually exclusive with `lengthsMs`. |
| `distribution` | `"even"` \| `"doubling"` | `"doubling"` | `"even"` gives every step an equal share of `totalMs`; `"doubling"` weights step *i* by `2^i` (each step roughly twice the previous). Mutually exclusive with `lengthsMs`. |
| `lengthsMs` | array of finite numbers (one per step, N ≥ 1) | unset | Explicit per-step lengths in milliseconds, in step order. Use instead of `steps`/`totalMs`/`distribution` for full control over each step's share. |
| `step.delayStrategy` | `"segmented-random"` \| `"uniform-random"` \| `"even"` | `"segmented-random"` | Shared default applied to every step unless overridden in `stepOverrides`. `"uniform-random"` samples each Channel's delay independently with replacement, so it permits duplicate delays and is exempt from the "N distinct positions" requirement the other two strategies enforce. |
| `step.mix` | `"hadamard"` \| `"householder"` \| `"random-orthogonal"` | `"hadamard"` | Shared default applied to every step unless overridden in `stepOverrides`. A matrix of a given type is resolved once and shared across every step that uses it. `"hadamard"` requires a power-of-two Channel count; `"householder"` and `"random-orthogonal"` accept any Channel count. |
| `step.shuffle` | boolean | `true` | Shared default applied to every step unless overridden in `stepOverrides`. |
| `step.polarity` | `"seeded-random"` \| `"none"` | `"seeded-random"` | Shared default applied to every step unless overridden in `stepOverrides`. |
| `stepOverrides` | array of `{index, delayStrategy?, mix?, shuffle?, polarity?}` | unset | Sparse per-step overrides keyed by zero-based step index. Only listed fields are overridden; omitted fields fall back to the shared `step` defaults above. Each index must be unique and within `[0, stepCount)`. |

`"hadamard"` mixes maximally (`N·log₂N` additions) and is the diffuser's
default. `"householder"` subtracts twice the mean of the Channels from every
Channel — cheap, mild mixing, valid for any N. `"random-orthogonal"` is a
seeded dense orthogonal matrix with no Haar-uniformity claim: a fixed
`[-1, 1]` fill is orthogonalized via Householder QR with a fixed sign
convention, and a singular or near-singular fill is rejected outright rather
than silently repaired.

Every Diffusion Step's `delaysSamples`, `permutation`, and `polaritySigns`
are derived from `(seed, step index, Channel)`, so a given step index's
`permutation`/`polaritySigns` are stable across changes to `steps`/`totalMs`/
`lengthsMs` — only `delaysSamples` shifts when a step's own resolved length
changes. Per-step sample lengths are apportioned from `totalMs`/`lengthsMs`
using largest-remainder rounding, so they always sum exactly to the
resolved total.

The resolved Diffuser is rejected before allocation if its estimated DSP
memory footprint (delay lines plus per-step mix matrices) exceeds the
configured budget — 512 MiB by default, overridable with
`--memory-budget-mib <mebibytes>` on `render`. `--capture-stages all` writes
one canonical WAV per resolved step, named `01-diffusion-step-{i}.wav`.

#### `downmix` stage

| Field | Values | Default |
| --- | --- | --- |
| `strategy` | `"select"` | `"select"` (only option) |
| `normalisation` | `"energy"` \| `"none"` | `"energy"` |

`delayStrategy: "even"` or `"uniform-random"`, `shuffle: false`,
`polarity: "none"`, and `normalisation: "none"` are diagnostic ablations for
isolating one DSP behavior at a time; they are not intended as listening
presets.

Keep this table in sync whenever a request field, its accepted values, or its
default changes.

### Sustain a Response with a Feedback Loop

The `[split, feedback-loop, downmix]` Composition shape circulates each
Channel through its own delay line, deliberately loses energy each
circulation via a per-Channel decay gain solved from a requested RT60, and
mixes the Channels orthogonally. Unlike the Diffuser, its output is
**unaligned** (Channels carry different echo times) and **not all-pass** --
the Feedback Loop is the one stage documented to lose energy on purpose.
Render a curated listening sample through it to hear a sustained,
decaying tail:

```bash
build/default/rvrbotron render \
  --input samples/listening/PianoDry.wav \
  --config request.json \
  --output build/tail-result
```

```json
{
  "formatVersion": 1,
  "seed": 0,
  "composition": {
    "stages": [
      {
        "type": "split",
        "channels": 8,
        "strategy": "duplicate",
        "normalisation": "energy"
      },
      {
        "type": "feedback-loop",
        "delayMinMs": 100,
        "delayMaxMs": 200,
        "delayStrategy": "segmented-random",
        "rt60Sec": 2.4,
        "decayMargin": 1.5,
        "mix": "householder",
        "silenceFloorDb": null
      },
      {
        "type": "downmix",
        "strategy": "select"
      }
    ]
  }
}
```

| Field | Values | Default | Notes |
| --- | --- | --- | --- |
| `delayMinMs` / `delayMaxMs` | finite number > 0 | `100` / `200` | Room size; `delayMinMs` also sets the pre-tail gap. |
| `delayStrategy` | `"segmented-random"` \| `"uniform-random"` \| `"even"` | `"segmented-random"` | `"even"` demonstrates flutter -- avoid it for a listening preset. |
| `rt60Sec` | finite number > 0 | `2.4` | Requested decay time at the 1 kHz Reference band; solved into per-Channel gain at configuration. |
| `decayMargin` | finite number > 0 | `1.5` | Multiplies `rt60Sec` to derive the resolved Tail budget (the upper bound on frames rendered past input EOF); `1.5` places the drain's end near -90 dB. |
| `mix` | `"hadamard"` \| `"householder"` \| `"random-orthogonal"` | `"householder"` | Mild mixing is the default here, in contrast to the Diffuser's maximal Hadamard default. |
| `silenceFloorDb` | finite number, or `null`/omitted | `null` (disabled) | Reserved seam for the eventual plugin's runtime idle behavior; dormant here -- disabled output is bit-identical to a build without the field. |

Each Channel's decay gain is solved independently from that Channel's own
loop time (`gain = 10^(-3L/R)`), so every Channel decays at the same rate
regardless of its own delay -- the requested `rt60Sec` is measurable in the
output even with unequal delays. The resolved Tail budget, delays, gains,
and matrix coefficients are recorded in `resolved.json`, so a resolved
rerender reproduces the tail exactly. `render.json`'s `tailBudgetFrames`
records the upper bound actually authorised for this render.

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
requires `inputFrames + resolved diffuser.totalSamples` frames in the output
and every capture, measures the actual captured Split and cumulative Diffusion
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
block size, Channel count, step count, and the mixing matrix used by every
Diffusion Step. Benchmarking a Debug binary prints a prominent warning to
stderr but is not refused.

### Run the Canonical Diffusion Experiment Catalog

`tools/diffusion_catalog_v1.json` is a versioned catalog: one complete
Reference configuration (N=8 duplicate Split, 4-step 300 ms doubling Hadamard
Diffuser) plus ~20 named one-axis overrides (Channel count, Split strategy,
total length, step count, distribution, delay strategy, matrix, and
shuffle/polarity ablations, expressed as JSON-Pointer add/remove/replace
operations against the Reference), and 5 listening cases mapping curated
material to a hypothesis, each paired with its quantitative counterpart's
exact Requested Configuration. Run it with (use `--preset release` for
meaningful benchmark evidence):

```bash
python3 tools/run_diffusion_catalog.py \
  --catalog tools/diffusion_catalog_v1.json \
  --renderer build/release/rvrbotron \
  --analyzer tools/analyze_diffusion.py \
  --mono-source tests/fixtures/audio/impulse-mono-pcm16-48000.wav \
  --stereo-source tests/fixtures/audio/impulse-stereo-left-pcm16-48000.wav \
  --listening-dir samples/listening \
  --output manual_UATs/diffusion-catalog
```

Each quantitative case renders the deterministic impulse basis with
`--capture-stages all`, publishes `analysis/diffusion-v1.json`, and
benchmarks at 128 frames (1 s warm-up, 5 s measured); the Reference
additionally benchmarks 32/64/128/256/512. Listening cases render the
curated (Git-LFS) sample and are skipped, not failed, when the sample isn't
present locally. Every case's output directory is an ordinary immutable
Render Result: a case whose `render.json` already exists is never
re-rendered, and rendering, analysis, and benchmarking are each resumed
independently, so an interrupted or partially failing run picks up exactly
where it left off when rerun with the same command, without replacing
existing evidence. Benchmark reports
aggregate into `<output>/benchmark-summary.json` and a terminal table ranked
slowest-to-fastest with deltas from Reference; `<output>/catalog-report.json`
records every case's completed/resumed/skipped/failed status. This full run
is a local research artifact -- expect on the order of a hundred-plus MB of
audio evidence and several minutes of wall time. CI instead runs a small
tracer catalog (`tests/test_diffusion_catalog_cli.py`) that proves
materialization, pairing, resumability, and aggregation without executing
the full sweep.

### Validate a Listening Sample Locally

Listening material is intentionally excluded from automated tests. After
installing Git LFS, render and analyze a curated sample through the same public
commands:

```bash
git lfs install
git lfs pull
build/default/rvrbotron render \
  --input samples/listening/DryHomePercussions.wav \
  --output build/listening-result
python3 tools/analyze_render.py \
  build/listening-result \
  --source samples/listening/DryHomePercussions.wav
```

The empty Composition should report exact identity. The other curated sounds
and their listening purposes are documented in
[`samples/listening/README.md`](samples/listening/README.md).

### Add a Listening Sample

WAVs in `samples/listening/` are tracked through Git LFS and remain separate
from `tests/fixtures/`:

```bash
git lfs install
git lfs track "samples/listening/*.wav"
git add .gitattributes samples/listening
git lfs ls-files
```

Add the sample's format, listening purpose, provenance, and redistribution
permission to `samples/listening/README.md`.

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
