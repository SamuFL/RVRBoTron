# Render and analyze evidence

What a render leaves behind, how to replay it exactly, how to capture what
happened inside the DSP, how to measure it, and how to read a failure.

*Why each measurement is defined the way it is lives in the [reverb design
corpus](../design/reverb/README.md) and the [ADRs](../adr/); this guide is
about running things and reading what comes back.*

---

## Render

```bash
build/default/rvrbotron render \
  --input tests/fixtures/audio/impulse-mono-pcm16-48000.wav \
  --config request.json \
  --output build/result
```

| Flag | Purpose |
| --- | --- |
| `--input <wav>` | Source audio. Required. |
| `--output <dir>` | Render Result directory. Required, and must not already exist. |
| `--config <request.json>` | A Requested configuration. See [Configure the Composition](configure-the-composition.md). |
| `--resolved <resolved.json>` | Replay a Resolved configuration instead. Mutually exclusive with `--config`. |
| `--block-size <frames>` | Processing block size. Does not change output. |
| `--capture-stages all` | Also write Stage captures. |
| `--memory-budget-mib <n>` | Diffuser memory ceiling (default 512). |
| `--error-format <text\|json>` | Diagnostics format. |

With neither `--config` nor `--resolved`, the render is exact identity.

Evidence is built in a temporary sibling directory and published atomically,
so a Render Result is either complete or absent — never half-written. An
existing output path is never overwritten; pick a fresh one each time.

## What a render produces

| Path | Contents |
| --- | --- |
| `output.wav` | IEEE float32, or float64 under the `double` preset |
| `resolved.json` | The complete Resolved configuration the DSP actually used |
| `request.json` | The exact bytes you supplied — only for `--config` renders |
| `render.json` | Provenance and frame accounting (below) |
| `captures/` | Stage captures, only with `--capture-stages all` |
| `analysis/` | Analyzer artifacts, added afterwards |

### `render.json`

| Field | Meaning |
| --- | --- |
| `inputFilename`, `inputSha256` | Source provenance — the filename only, never a full path |
| `rendererVersion`, `platform`, `architecture` | Build provenance |
| `samplePrecision` | `float32` or `float64` |
| `configurationInput` | `defaults`, `requested`, or `resolved` |
| `sampleRate`, `channels` | Output audio facts |
| `blockSize` | Block size used |
| `inputFrames` | Frames read from the source |
| `preDelayFrames` | Resolved Pre-delay, before Split |
| `tailBudgetFrames` | Resolved Tail budget, drained past input EOF |
| `frames` | Total frames written |
| `stageCaptureProfile`, `stageCaptures` | Capture manifest, only with `--capture-stages all` |

**`preDelayFrames` and `tailBudgetFrames` are separate on purpose** — onset
delay is not decay duration. Both are authorised by the Resolved
configuration, and together they account for everything written past the
source:

```text
frames = inputFrames + preDelayFrames + tailBudgetFrames
```

`preDelayFrames` is `0` for the identity Composition and whenever
`preDelayMs` is `0`. `tailBudgetFrames` is `0` for a Composition with neither
a Diffuser nor a Feedback Loop. The renderer always drains the complete
budget, so that equation holds exactly.

`render.json` deliberately records no timestamp, host or user identity, full
input path, or source-tree fingerprint — so the same render on two machines
differs only where it genuinely differs.

## Replay a Resolved configuration

```bash
build/default/rvrbotron render \
  --input tests/fixtures/audio/impulse-mono-pcm16-48000.wav \
  --resolved build/result/resolved.json \
  --output build/replay
```

`resolved.json` holds every derived value — per-Channel delays, gains,
matrices, shelf coefficients, Modulation trajectories, the Tail budget — so
replay resolves nothing and reproduces `output.wav` bit-identically on the
same binary and precision (Repeat determinism; across machines see
[cross-platform equivalence](#compare-evidence-across-platforms) below).

The new source must have the same Channel count the first render resolved
(the resolved Split's `inputChannels`). Rerendering a stereo-resolved
configuration from a mono file is rejected, not adapted.

## Stage captures

`--capture-stages all` additionally writes the multi-Channel signal at each
named boundary, so you can measure what happened *inside* the Composition
rather than only at its output.

| Path | Boundary | Written when |
| --- | --- | --- |
| `captures/00-split.wav` | After Split | Always (non-empty Composition) |
| `captures/01-diffusion-step-<i>.wav` | After each Diffusion Step | One per resolved step |
| `captures/02-main-stereo.wav` | Main branch, after Downmix, Width, and level | Always (non-empty Composition) |
| `captures/03-early-stereo.wav` | Early branch, same point | Only with an Early branch configured |

The two stereo captures are taken immediately before the branches are summed,
so `main-stereo` plus `early-stereo` is the **Wet sum**. Under the default
envelope (`wetOnly: true`, 0 dB wet) that equals `output.wav`; with an
envelope configured, `output.wav` is the dry contribution plus the Wet sum
scaled by `wetGain`.

Each `stageCaptures` entry carries a `disabled` flag, `true` only for a branch
that is configured but switched off (`mainEnabled: false`, or
`early.enabled: false`). That capture is still written, correctly sized to the
render's timeline, but exact zero throughout — a disabled branch contributes
exact zero rather than a zero-multiplied value. `split` and `diffusion-step`
captures have no enablement of their own, so their flag is always `false`.

Captures share the output timeline, so they are `inputFrames +
preDelayFrames + tailBudgetFrames` long too, and begin with the Pre-delay
interval where one is configured.

## Analyze

Analysis is a separate Python step, never required for rendering. It reads
only the immutable Render Result — never the clock, the machine, or system
load — so the same evidence analyzed anywhere produces the same artifact.
Publication is append-only and idempotent: repeating an identical analysis
succeeds silently, while different content for the same analyzer version is
rejected.

Install the analyzer dependencies once: `pip3 install -r tools/requirements.txt`.

| Analyzer | Use it for | Publishes | Needs captures |
| --- | --- | --- | --- |
| `analyze_render.py` | Any render — baseline audio facts | `baseline-v1.json` | no |
| `analyze_diffusion.py` | A Diffuser-only Composition | `diffusion-v1.json` | yes |
| `analyze_tail.py` | Anything with a Feedback Loop | `tail-v1.json` | no |
| `analyze_tail_v2.py` | A damped Feedback Loop | `tail-v2.json` | no |
| `analyze_modulation.py` | A modulated Feedback Loop | `modulation-v1.json` | no |
| `analyze_downmix.py` | Main/Early Downmix and stereo image | `downmix-v1.json` | yes |
| `analyze_early_support.py` | An Early Reflections branch | `early-support-v1.json` | yes |

### Baseline

```bash
python3 tools/analyze_render.py build/result \
  --source tests/fixtures/audio/impulse-mono-pcm16-48000.wav
```

Frame count, duration, sample rate, channel count, non-finite sample count,
and per-channel plus combined peak, RMS, and sum of squares. Units and metric
definitions are embedded in the artifact.

`--source` adds an **identity** comparison against the input: `identity.equal`,
differing-sample count, maximum absolute error, and first mismatch. An
identity render reports `identity.equal: true` with `differingSampleCount: 0`.

It requires the source SHA-256 to match `render.json`, and the output to have
the same channel count, sample rate, and frame count as the source. So
`--source` suits identity renders; on a Composition that adds a tail the frame
counts differ by design and the analyzer fails with `source audio facts do not
match output.wav`. Omit `--source` there — everything else is still reported.

### Diffusion

```bash
python3 tools/analyze_diffusion.py build/diffusion-result \
  --source tests/fixtures/audio/impulse-mono-pcm16-48000.wav
```

Checks the complete finite response is present, then measures captured Split
and cumulative Diffusion Step energies (with each step's relative error from
Split) and matrix orthogonality from `resolved.json`. It also reports signed
Correlation at every step, an Alignment score, Distinct-arrival density
against the theoretical `N^k` Echo-path count, and Coloration for both the
N-Channel signal and the stereo output — reported separately, since only the
former carries the all-pass claim.

Use `--compare <other-result>` instead of `--source` to check two renders of
the same Resolved configuration — typically at different `--block-size`
values — for exact decoded equality of `output.wav` and every capture. This
mode prints a report and publishes nothing.

### Tail

Schroeder integration assumes an impulse response, so measure a deterministic
impulse render of the same Resolved configuration rather than musical
material:

```bash
build/default/rvrbotron render \
  --input tests/fixtures/audio/impulse-mono-pcm16-48000.wav \
  --resolved build/tail-result/resolved.json \
  --output build/tail-impulse

python3 tools/analyze_tail.py build/tail-impulse \
  --source tests/fixtures/audio/impulse-mono-pcm16-48000.wav
```

Requires a Feedback Loop in the Composition; any other shape is rejected.
RT60 is measured per octave band from 63 Hz to 16 kHz. T30 is the primary fit
with T20 reported beside it — the two agree on a single-rate decay and
diverge under `gainMode: "uniform"` at a wide delay spread, which is the
point of reporting both. The Reference band (1 kHz) is compared against the
requested `rt60Sec`.

It also reports a decay-envelope check: raw, non-Schroeder-integrated
broadband energy from the end of the source onward, verified non-increasing.
That is deliberately separate from the per-band Schroeder curves, which are
non-increasing by construction and so cannot catch a genuine buildup.

`analyze_tail_v2.py` adds Damping-aware predicted-versus-measured decay and
the low/Reference/high ratio summary; `analyze_modulation.py` adds Modulation
evidence. Both take the same arguments.

### Spatial output and Early Reflections

```bash
python3 tools/analyze_downmix.py build/downmix-result \
  --source tests/fixtures/audio/impulse-mono-pcm16-48000.wav

python3 tools/analyze_early_support.py build/early-result
```

`downmix-v1.json` reports each branch's resolved strategy, Alignment
expectation, and Coherent Downmix ablation tag read from `resolved.json`
rather than re-derived; Output correlation, inter-channel level difference,
peak factor, and equal-power mono fold-down on the stereo output; and branch
energies reconciled with their cross term against the Wet sum.

A measured Alignment score and branch energy ratio are reported only when the
Downmix's immediate source is a Diffusion Step. An unaligned source — a
Feedback Loop between Diffuser and Downmix, or no Diffuser at all — has no
equivalent capture, so those are reported unavailable rather than measured
against the wrong signal ([ADR-0005](../adr/0005-measure-movement-at-the-output.md)).
The branch energy ratio is the *combined* effect of Downmix projection, Width,
and branch level; no capture exists between Downmix and Width to separate
them.

`early-support-v1.json` reports, per resolved tap, the measured first and last
non-zero sample, peak, and energy-weighted centroid from that tap's own
captured Diffusion Step, alongside its resolved nominal and conservative Tap
support bounds. Cancellation can make measured support narrower than the
structural bound; that never fails a render.

None of these analyzers impose an acoustic rejection threshold — see
[ADR-0004](../adr/0004-validate-structure-not-acoustics.md).

## Compare evidence across platforms

Repeat determinism holds on one machine; across machines the claim is
*equivalence within declared tolerances*
([ADR-0001](../adr/0001-cross-platform-reproducibility.md)). Each platform
reduces an already-published artifact to a compact, WAV-free summary, and a
comparator checks the summaries against committed tolerances.

```bash
python3 tools/extract_diffusion_equivalence.py build/diffusion-result \
  --output build/diffusion-equivalence.json

python3 tools/compare_diffusion_equivalence.py \
  build/diffusion-equivalence.json other-platform/diffusion-equivalence.json \
  --tolerances tools/diffusion_tolerances_v1.json
```

The tail workflow is identical with `extract_tail_equivalence.py`,
`compare_tail_equivalence.py`, and `tools/tail_tolerances_v1.json`.

Both comparators group summaries by sample precision, pick one baseline per
group (macOS arm64 when present), and split their checks in two:

- **Exact.** Facts that are deterministic functions of the Resolved
  configuration rather than measurements — the structural `N^k` Echo-path
  count and Distinct-arrival bin count; the octave-band count, whether each
  band's fit exists, and the decay-envelope monotonicity flag.
- **Within tolerance.** Everything measured, against the committed
  per-precision tolerance file.

A group with fewer than two platforms is reported skipped, not failed.
Tolerances are only committed after reviewing real evidence from every
supported CI architecture; ADR-0001 records the run reviewed and each
metric's observed headroom.

## Benchmark

```bash
build/release/rvrbotron benchmark \
  --resolved build/result/resolved.json \
  --block-size 128 \
  --warmup-seconds 1 \
  --measure-seconds 5 \
  --json build/benchmark.json
```

Use an optimized build — `--preset release` or `--preset release-double`.
Benchmarking a Debug binary warns loudly but is not refused, and its numbers
are meaningless.

Benchmark evidence is **environment-qualified, not deterministic**: it depends
on the machine, build, and load at measurement time, so it is never asserted
equal across machines. CI runs it as smoke coverage only — the report has a
sane shape and completes — with no performance gate.

Timing covers `Reverb::process` only, excluding configuration and file I/O.
Seeded nonzero blocks drive the constructed `Reverb` for the warm-up
(discarded), then for the measured duration with every block timed
individually. The terminal and JSON reports are identical and cover:

- **Time** — median, p95, and worst block time.
- **Real-time budget** — median and worst utilization, and missed-deadline
  count, at the resolved sample rate and requested block size.
- **Memory** — exact DSP-owned bytes (object storage plus every owned
  container's real capacity), and separately a best-effort process RSS delta,
  labelled as allocator-noisy.
- **Provenance** — renderer version, platform, architecture, compiler, build
  type, precision, sample rate, block size, Channel count, step count, mixing
  matrix, and the Modulation interpolation actually exercised per Diffusion
  Step and for the Feedback Loop (`null` where inactive), so two reports from
  different `interpolation` choices stay comparable.

The Composition must be non-empty, with mono or stereo Channel counts.

## Diagnostics

Failures go to stderr with a stable exit code:

| Exit | Category | Typically |
| ---: | --- | --- |
| 2 | `malformed_json` | The file is not valid JSON |
| 3 | `unsupported_audio` | Input WAV format the renderer does not accept |
| 4 | `invalid_configuration` | A field is missing, wrong, or out of range |
| 5 | `io_failure` | Could not read or write a path |
| 6 | `internal_processing_failure` | A bug — report it |
| 7 | `invalid_arguments` | Bad or missing command-line flags |

Every configuration rejection names the exact path and the reason:

```text
invalid_configuration at /composition/stages/2/leftChannel:
expected a Channel index within [0, N)
```

`--error-format json` returns the same information as one object, which is
what the sweeps and catalogs parse:

```json
{
  "category": "invalid_configuration",
  "exitCode": 4,
  "location": "/composition/stages/2/leftChannel",
  "reason": "expected a Channel index within [0, N)"
}
```

`location` is present only for configuration errors — it is the JSON Pointer
into your request, so it points at the field to fix.
