# Run experiments

How to run RVRBoTron's experiment catalogs and sweeps, listen to what they
produce, and add your own.

An **experiment** here means: render the same material through many
configurations that differ along one axis at a time, measure each one, and
compare them. The catalogs are versioned JSON, so a run is reproducible and
reviewable.

*Why the axes are what they are lives in the [reverb design
corpus](../design/reverb/README.md), not here. Contributing changes is
covered by the [engineering workflow](../agents/workflow.md).*

---

## Before you start

```bash
cmake --preset release && cmake --build --preset release
pip3 install -r tools/requirements.txt
git lfs install && git lfs pull      # curated listening samples
```

Use the **release** build. The default build is fine for correctness but its
benchmark numbers are meaningless.

---

## Which experiment do I want?

| I want to explore… | Run | Analyzer |
| --- | --- | --- |
| Echo density and diffusion structure | `run_diffusion_catalog.py` | `analyze_diffusion.py` |
| Decay: size, RT60, mixing, gain | `run_tail_sweep.py` | `analyze_tail.py` |
| Frequency-dependent decay (Damping) | `run_damping_sweep.py` | `analyze_tail_v2.py` |
| Movement (Modulation) | `run_modulation_sweep.py` | `analyze_modulation.py` |
| Stereo image (Early/Main Downmix, Width) | `run_spatial_sweep.py` | `analyze_downmix.py` |

Each has a matching catalog in `tools/` (for example
`tools/tail_sweep_v1.json`) holding one Reference configuration plus the
named axes that override it.

---

## How the sweeps work

The four sweeps (`tail`, `damping`, `modulation`, `spatial`) share one shape,
so learning one teaches the rest.

**One axis at a time.** Every point changes exactly one thing from the
Reference. Nothing is combinatorial — a sweep of 5 axes with 2 values each is
11 points (Reference plus 10), not 32 combinations.

**Sample plus impulse.** Each point renders both your chosen listening sample
*and* a deterministic impulse, under an identical Resolved Configuration (the
sample's `resolved.json`, replayed with `--resolved`). Measurement runs on the
impulse only: Schroeder integration assumes an impulse response, and programme
material would contaminate the decay curve with its own envelope. You listen to
the sample; the numbers come from the impulse.

**Output is ordered for auditioning.**

```text
<output>/<sample>/00-reference/
<output>/<sample>/01-<axis>/01-<value>/
<output>/<sample>/01-<axis>/02-<value>/
```

**Resumable.** Render, analysis, and benchmark are each resumed independently.
A point whose `render.json` already exists is never re-rendered, so rerunning
the same command after an interruption picks up where it stopped without
replacing existing evidence.

**A missing sample is skipped, not failed.** If Git LFS content isn't pulled,
the run says so and continues.

**One page to listen to it all.** Every sweep regenerates
`<output>/<sample>/listening-report.html`: a self-contained page with every
point playable in place, its measurements, and a ranked cross-point
comparison. No JavaScript, no external requests. Regeneration reads only
already-published evidence, so refreshing a report costs nothing.

Full runs are local research artifacts — expect minutes of wall time and
hundreds of MB of audio. CI never runs them; it runs millisecond-scale tracer
catalogs instead (`tests/test_*_sweep_cli.py`) that prove materialization,
pairing, and resumability.

---

## The diffusion catalog

Varies the Diffuser itself: Channel count, Split strategy, total length, step
count, distribution, delay strategy, matrix, and shuffle/polarity ablations —
22 one-axis overrides against a Reference of N=8 duplicate Split and a
four-step 300 ms doubling Hadamard Diffuser, plus 5 listening cases.

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

This one is shaped differently from the sweeps: quantitative cases render the
impulse basis with `--capture-stages all` and publish
`analysis/diffusion-v1.json`; separate listening cases render curated samples
and pair each to its quantitative counterpart. Add `--skip-listening` for the
quantitative cases only.

Every *quantitative* case is benchmarked at 128 frames (1 s warm-up, 5 s
measured); the Reference also at 32/64/128/256/512. Listening cases are not
benchmarked.

```text
<output>/cases/<name>/            # impulse renders, analysis, benchmark-128.json
<output>/listening/<name>/        # curated-sample renders
<output>/benchmark-summary.json   # ranked slowest-to-fastest, deltas from Reference
<output>/catalog-report.json      # per-case completed/resumed/skipped/failed
```

This catalog has no `listening-report.html` — that is a sweep feature.

## The tail sweep

Decay behaviour: `delay-range`, `rt60`, `mix`, `delay-strategy`, `gain-mode`,
against a Reference Feedback Loop of 100-200 ms delays, RT60 2.4 s, Householder
mixing, per-Channel gain.

```bash
python3 tools/run_tail_sweep.py \
  --catalog tools/tail_sweep_v1.json \
  --renderer build/release/rvrbotron \
  --analyzer tools/analyze_tail.py \
  --sample samples/listening/PianoDry.wav \
  --mono-impulse tests/fixtures/audio/impulse-mono-pcm16-48000.wav \
  --stereo-impulse tests/fixtures/audio/impulse-stereo-left-pcm16-48000.wav \
  --output manual_UATs/tail-sweep
```

The report shows measured decay against requested RT60, with a per-band T30
breakdown behind a toggle, plus benchmark cost per point. Artifacts:
`sweep-report.json`, `benchmark-summary.json`, `listening-report.html`.

The other three sweeps take the same arguments — swap the catalog, analyzer,
and output.

## The damping sweep

Two-shelf Damping: `high-ratio`, `low-ratio`, `high-corner`, `low-corner`,
against the tail Reference plus the Damping baseline (`highRatio` 0.5 at
4000 Hz, `lowRatio` 1.0 at 200 Hz). Ratios above 1.0 (boost) are deliberately
excluded from listening material; `tests/test_feedback_loop_cli.py` covers them
as configuration.

```bash
python3 tools/run_damping_sweep.py \
  --catalog tools/damping_sweep_v1.json \
  --renderer build/release/rvrbotron \
  --analyzer tools/analyze_tail_v2.py \
  --sample samples/listening/PianoDry.wav \
  --mono-impulse tests/fixtures/audio/impulse-mono-pcm16-48000.wav \
  --stereo-impulse tests/fixtures/audio/impulse-stereo-left-pcm16-48000.wav \
  --output manual_UATs/damping-sweep
```

Analysis is tail version 2 (`analysis/tail-v2.json`): predicted-vs-measured
octave-band decay and the low/Reference/high ratio summary. A Reference-band
deviation past 10% from the requested `rt60Sec` is a gentle one-pole shelf's
real transition-band behaviour, so the report flags it (`significantDeviation`)
rather than failing the point — see
[ADR-0004](../adr/0004-validate-structure-not-acoustics.md).

## The modulation sweep

Movement: `depth`, `rate`, `detune-product`, `interpolation`, `target`,
`shape`, `channel-fraction`, `damping-interaction`.

```bash
python3 tools/run_modulation_sweep.py \
  --catalog tools/modulation_sweep_v1.json \
  --renderer build/release/rvrbotron \
  --analyzer tools/analyze_modulation.py \
  --sample samples/listening/PianoDry.wav \
  --mono-impulse tests/fixtures/audio/impulse-mono-pcm16-48000.wav \
  --stereo-impulse tests/fixtures/audio/impulse-stereo-left-pcm16-48000.wav \
  --output manual_UATs/modulation-sweep
```

Adds `<output>/<sample>/01-depth/identity-invariant.json`, which byte-compares
the depth axis's `omitted` and `zero` points: omitting Modulation entirely and
setting `depthMs: 0` must produce identical output. A mismatch is published
rather than hidden — it is evidence of a regression.

## The spatial sweep

Stereo image: tap index, Early level, Early-envelope slope, aligned `select`
versus `sum-all`, Main strategy, selected Channel pair, Early width, Main
width, and N — against an explicit Reference of N=8 with the established
Diffuser and Feedback Loop, taps at Diffusion Steps 0 and 1, -6 dB Early level,
Early `select` Channels 0/1 at 90 degrees, and 0 dB `orthogonal-rows` Main
output at 90 degrees. See
[stage 08, Downmix](../design/reverb/stages/08-downmix.md) for why these axes.

```bash
python3 tools/run_spatial_sweep.py \
  --catalog tools/spatial_sweep_v1.json \
  --renderer build/release/rvrbotron \
  --analyzer tools/analyze_downmix.py \
  --sample samples/listening/PianoDry.wav \
  --mono-impulse tests/fixtures/audio/impulse-mono-pcm16-48000.wav \
  --stereo-impulse tests/fixtures/audio/impulse-stereo-left-pcm16-48000.wav \
  --output manual_UATs/spatial-sweep
```

The impulse render adds `--capture-stages all`, because `analyze_downmix.py`
needs Stage captures. Three things are unique to this sweep:

- **Isolation.** Each axis declares a `scope` (a JSON-Pointer prefix). Every
  point's configuration is diffed against the Reference, and any changed field
  outside that scope fails the point by name — a catalog mistake that would
  silently confound the sweep is caught instead.
- **Determinism.** Each impulse render is repeated into a scratch directory and
  byte-compared against the original; a mismatch fails that point. Published
  per point as `determinism.json`.
- **No benchmarks.** This sweep measures spatial behaviour, not cost.

The `coherent-downmix` axis's `sum-all` point is compared against the
Reference's own `select` Early Downmix once both are analyzed — peak-factor and
spectral-deviation deltas land in `coherent-downmix-comparison.json` as a
non-fatal warning. Neither render is rejected.

---

## Validate a listening sample

Listening material is deliberately excluded from automated tests. To check a
curated sample by hand:

```bash
build/default/rvrbotron render \
  --input samples/listening/DryHomePercussions.wav \
  --output build/listening-result
python3 tools/analyze_render.py \
  build/listening-result \
  --source samples/listening/DryHomePercussions.wav
```

With no configuration the render is exact identity, so
`analysis/baseline-v1.json` reports `identity.equal: true` and
`differingSampleCount: 0`. The curated sounds and their listening purposes are
in [`samples/listening/README.md`](../../samples/listening/README.md).

---

## Add an experiment

You add an **axis** to an existing catalog file. You do not write a new runner,
and you do not write a new `request.json` per point — the runner builds each
point for you by applying your overrides to the catalog's Reference.

Worked example: add a Channel-count axis to the tail sweep, which currently
sweeps `delay-range`, `rt60`, `mix`, `delay-strategy`, and `gain-mode` but
never varies N.

### 1. Open the catalog

Edit the file you passed to `--catalog`. One per experiment:

| Experiment | Catalog file |
| --- | --- |
| Diffusion | `tools/diffusion_catalog_v1.json` |
| Tail | `tools/tail_sweep_v1.json` |
| Damping | `tools/damping_sweep_v1.json` |
| Modulation | `tools/modulation_sweep_v1.json` |
| Spatial | `tools/spatial_sweep_v1.json` |

Each has three top-level keys: `formatVersion`, `reference`, and `axes`.

### 2. Find your key in `reference`

`reference` is an ordinary Requested configuration — the same shape as a
`request.json` you would hand the renderer. This is where override paths come
from. The tail Reference is:

```json
"reference": {
  "formatVersion": 2,
  "seed": 42,
  "composition": {
    "stages": [
      { "type": "split", "channels": 8, "strategy": "duplicate", "normalisation": "energy" },
      { "type": "feedback-loop", "delayMinMs": 100, "delayMaxMs": 200,
        "delayStrategy": "segmented-random", "rt60Sec": 2.4,
        "mix": "householder", "gainMode": "per-channel" },
      { "type": "downmix", "strategy": "select", "leftChannel": 0, "rightChannel": 1,
        "normalisation": "energy" }
    ]
  }
}
```

Read the path off that tree. `stages` is an array, so the Split is `stages/0`,
the Feedback Loop `stages/1`, the Downmix `stages/2`. The Split's `channels`
(N, here 8) therefore lives at:

```text
/composition/stages/0/channels
```

Any field in [Configure the
Composition](configure-the-composition.md) can be addressed this way —
`/composition/stages/1/rt60Sec` for the loop's RT60,
`/composition/stages/2/widthDeg` for Main Width, and so on.

### 3. Add your axis to `axes`

Append one object to the catalog's existing `axes` array:

```json
{
  "name": "channels",
  "hypothesis": "More Channels read as a denser, smoother tail at the same decay time.",
  "values": [
    { "label": "n-4",
      "overrides": [
        { "op": "replace", "path": "/composition/stages/0/channels", "value": 4 }
      ] },
    { "label": "n-16",
      "overrides": [
        { "op": "replace", "path": "/composition/stages/0/channels", "value": 16 }
      ] }
  ]
}
```

`name` and `label` become directory names — appended after the catalog's five
existing axes, this one renders into `06-channels/01-n-4/` and
`06-channels/02-n-16/` — so keep them short and filename-safe. `overrides` are JSON-Pointer `add`, `remove`, and
`replace` operations — state only what differs from the Reference, never the
whole configuration.

Three rules keep a sweep worth running:

- **One axis changes one thing.** Two knobs at once cannot be attributed to
  either.
- **Write the `hypothesis` first.** If you cannot say what you expect to hear,
  the axis is not ready.
- **The spatial catalog also needs `scope`** — the JSON-Pointer prefix your
  axis is allowed to touch (for the axis above that would be
  `/composition/stages/0`). The runner diffs every point against the Reference
  and fails any change falling outside it.

### 4. Check it cheaply

The tracer test materializes every point in milliseconds and fails fast on a
malformed axis or a bad pointer:

```bash
ctest --preset default -R tail_sweep_contract
```

### 5. Run it

Nothing new — the same command from [the tail sweep](#the-tail-sweep) above.
The runner picks up your axis from the catalog:

```bash
python3 tools/run_tail_sweep.py \
  --catalog tools/tail_sweep_v1.json \
  --renderer build/release/rvrbotron \
  --analyzer tools/analyze_tail.py \
  --sample samples/listening/PianoDry.wav \
  --mono-impulse tests/fixtures/audio/impulse-mono-pcm16-48000.wav \
  --stereo-impulse tests/fixtures/audio/impulse-stereo-left-pcm16-48000.wav \
  --output manual_UATs/tail-sweep
```

Your new points appear as `<output>/PianoDry/06-channels/01-n-4/` and
`02-n-16/`, and in the listening report alongside every existing point.
Existing points are not re-rendered — resumability means only the new ones
cost anything.

### The diffusion catalog is shaped differently

It has no `axes`. Instead `cases` (each `name`, `source`, `overrides` — same
JSON-Pointer overrides against `reference`) and `listeningCases` (each `name`,
`sample`, `hypothesis`, and `pairedQuantitativeCase` naming the case whose
numbers back the listening claim).

## Add a listening sample

WAVs in `samples/listening/` are tracked through Git LFS and stay separate
from `tests/fixtures/`:

```bash
git lfs install
git lfs track "samples/listening/*.wav"
git add .gitattributes samples/listening
git lfs ls-files
```

Then add the sample's format, listening purpose, provenance, and
redistribution permission to
[`samples/listening/README.md`](../../samples/listening/README.md).
